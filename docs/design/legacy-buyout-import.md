# Legacy Buyout Import — Design

Status: implemented August 14, 2026, after the round-1 and round-2 reviews
and UX validation. Deliberately minimal — this is not a frozen spec.
Evidence: `legacy-buyout-import-investigation.md`; review findings cited as
`R1-*` and `R2-*` from `docs/archive/legacy-buyout-import-reviews.md`.

## Goal

Let a user recover buyouts from a pre-0.16 database file into the current
`item_buyouts` / `location_buyouts` store, keyed on GGG ids.

## Shape: plan, then apply

The import splits into two phases around an editable plan file. The plan
file is simultaneously the dry run, the audit trail, and the control
surface — it replaces policy decisions we would otherwise have to
hardcode blind (R1-5, R1-7) with prefilled rows the user can flip.

**Plan**: read the legacy file, match buyouts to GGG ids, consult the
*current* userstore (existing buyouts, current tab names, current
characters), and produce a plan. The user sees one dialog:

> Matched N of M buyouts (X ambiguous, Y orphaned).
> [Import now]  [Save plan for review…]  [Cancel]

"Import now" runs apply immediately with the prefilled defaults and still
writes the plan file next to the logs (`buyout-import-<timestamp>.xlsx`)
as the audit record. "Save plan…" lets the user edit it in a spreadsheet
and later run "Import buyout plan…" to apply it.

Because native file pickers may not render their captions, each picker is
preceded by an Acquisition message: recovery asks for a pre-0.16 data file
containing legacy buyouts, while plan import asks for a previously exported or
edited workbook. After the user chooses "Import now" or selects a saved plan,
both paths present the same warning that the import will write to the current
account's data file. That final confirmation defaults to Cancel and is the last
reversible point before apply.

**Apply**: execute the plan's `action = import` rows. Apply validates
consistency only — known currency/type/source tags, numeric values,
non-empty ids, valid action — it does not re-match. An orphaned row
becomes importable if the user fills in an item id by hand.

## Plan file format

XLSX via QXlsx (MIT, fetched by CMake like other third-party code).
Chosen over CSV deliberately: typed cells (prices stay numbers, ids stay
strings), no locale decimal/delimiter/encoding hazards, dropdown
validation, and a metadata sheet. This format is the output of a legacy
import and an input to apply — it is **not** a public bulk-price-editing
interface, and we make no compatibility promises beyond the import flow.

- Sheet `plan`: one row per legacy buyout target. Columns are located by
  header name, never by position. Roughly: `action` (dropdown:
  import/skip), `reason` (matched / ambiguous-k-of-n / orphaned /
  inherited / existing-manual / needs-attention / …), the buyout fields
  (`value`, `currency`, `type`, `source`, `inherited`), the target
  (`item_id`, `location_id`, `location_type`), context for humans
  (`item_name`, `old_tab_label`, `current_tab_name`), and provenance
  (`legacy_hash`, `existing_value`, `existing_source`).
- Sheet `meta`: format version, source file path, export timestamp,
  source db_version. Apply checks the version stamp.

## Prefill defaults (replacing hardcoded policy)

- Inherited legacy buyouts → `skip` (reason `inherited`): they were
  derived from tab buyouts, which import separately; imported-as-is they
  are destroyed by the next `PropagateTabBuyouts` (R1-5).
- Target already has a MANUAL row → `skip` (reason `existing-manual`):
  never silently clobber user-authored current data (R1-7, following the
  `MigrateItem` precedent).
- Target has a non-MANUAL (auto-generated) row → `import`: the legacy
  price outranks a machine-derived one (R1-7).
- Ambiguous hash (identical items / duplicate labels) → `import` to all
  matching targets, flagged `ambiguous-k-of-n`; this replicates what old
  acquisition displayed.
- Orphaned → listed with empty `item_id`, `action = skip`.

## Matching rules

- **Stash ids**: legacy-API-era files carry 64-hex stash ids in the tabs
  blob; only the non-OAuth API ever returned those, so ids longer than
  10 chars are confidently truncated to their first 10 (the old
  `ItemLocation::FixUid` rule, which `items.loc` and master's
  `ItemLocation::id()` both already use). A truncated id that fails the
  cross-checks (present among the old file's `items.loc` keys; ideally in
  the current `stashes` table) flags that row `needs-attention` rather
  than aborting the import (R1-2).
- **Tab labels**: read from `n` with `name` as fallback (legacy-API tab
  JSON has no `name`; R1-3). The label is only the join key *within* the
  old file (legacy `tab_buyouts` are label-keyed); from there everything
  rides the unique id, which survives renames. The plan shows both
  `old_tab_label` and `current_tab_name` so the user can sanity-check.
- **Characters**, in fallback order (R1-9): (1) the old file's character
  `id` when present; (2) name match against current userstore
  characters; (3) equipped-item search — if a current character's
  `json_data` items include any of the old character's item ids, propose
  that character with reason `character-matched-by-items` for the user
  to confirm in the plan; (4) orphan. The search simply uses whatever
  the userstore currently holds — no fetch-state checks (see
  precondition below).

## Apply mechanics

- The save loops run inside a single `m_db.transaction()`/`commit()`
  (the `userstore.cpp` migrate pattern) with one hoisted prepared
  statement per loop, behind a wait cursor (R1-10).
- Write errors are counted separately from skips, recorded per row, and
  fail the report loudly — a failed import must be distinguishable from
  an idempotent re-run (R1-4).
- After a successful apply, the import is treated like any other buyout
  update: propagate tab buyouts, restore refresh-lock state, expire shop
  data — the same path `OnBuyoutChange` takes (R1-8).

## Parse leniency

Item granularity, not row granularity (R1-6): items rows are parsed
per element (e.g. `std::vector<glz::raw_json>`, then per-item
conversion), so one malformed 2014-era item flags that item, not an
entire stash tab. The same guard applies to the `buyouts`/`tab_buyouts`
blobs: a partial parse must not masquerade as a complete one.

## Precondition: a recent full refresh

Matching quality depends on the current userstore holding freshly
refreshed stashes and characters. This is deliberately the user's
responsibility, not the code's: the dialog carries a warning ("import
works best immediately after a full refresh of stashes and characters"),
and staleness is self-evident in the plan (empty current-name columns,
unmatched characters). No enforcement, no fetch-state gating. Optionally
the dialog may display the newest `json_fetched_at` as a nudge.

## Simplifying assumptions, on purpose

- **GGG ids are persistent.** Validated at 1170/1171 over 14 months.
- **db_version 4 and 5 only** (5 is master's harmless re-stamp of a v4
  file — R1-1, fixed). Files at `<4` are refused with a clear message
  (add the prefixed-hash variant later if anyone hits it).

## Deferred (iterate after this works)

- Automatic candidate discovery across `data/`, `data-backup-*/`,
  `m_data_save*/`, with league/count/mtime shown per candidate.
- db_version <4 hash variant.
- A startup nudge when unimported legacy buyouts are detected.
- Deleting the superseded live-hash migration machinery
  (`ItemsManager::MigrateBuyouts`, `Item::hash_v4`/`old_hash`,
  `ItemLocation::GetLegacyHash`) — closes F54, retires the F51
  constraint. Do this only once the importer has proven itself in a
  release.
