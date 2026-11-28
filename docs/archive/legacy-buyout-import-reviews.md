# Legacy Buyout Import — Review History

Findings from reviews of the legacy buyout import branch, following the
project's review-record convention: round-scoped IDs, cited from code and
commits as `R1-1` etc. Design: `legacy-buyout-import.md`; evidence:
`legacy-buyout-import-investigation.md`.

## Round 1 — August 14, 2026 (tracer bullet, commit c687409f)

High-effort multi-angle review with adversarial verification; ten findings
survived. Status values: **open** (not yet addressed), **agreed** (fix
shape settled in the August 14 design discussion, recorded in the design
doc's revision — see "Round 1 resolutions" below), **resolved by design
revision** (the plan/apply redesign dissolves the question), **fixed**
(commit noted), **decision needed** (policy question for Tom).

### R1-1. The db_version gate refuses files master itself stamps 5 — fixed

`importFile` refused any `db_version != "4"`. But master's
`ItemsManager::MigrateBuyouts` stamps `db_version` 5 into the legacy
`data/<md5>` file on the first refresh after an upgrade (a no-op v4→v5
pass; the `buyouts`/`tab_buyouts` keys are not deleted). So the *live*
file of the primary target user — an upgrade from ≤0.15 straight to
current master — was refused as "Unsupported legacy database version '5'"
despite holding every v4-generation hash-keyed buyout. The keys in a
5-stamped file are still v4-generation hashes, so the fix is to accept 4
and 5 and refuse only `<4` (matching the design doc, which always said
"refuse `<4`"). **Fixed** on this branch: gate accepts 4 and 5; test
`importsVersion5StampedFiles` pins it.

### R1-2. Tab buyouts keyed on the untruncated 64-char legacy stash id — agreed

`add_stash` writes `location_buyouts` rows keyed on the raw `stash.id`
from the old tabs blob. Pre-0.16 truncated legacy-API 64-hex stash ids to
their first 10 chars (`ItemLocation::FixUid`), which is what the old
`items.loc` keys and master's `ItemLocation::id()` both use — but the
serialized tab JSON keeps the untruncated id. Importing a legacy-API-era
file therefore writes tab buyouts under a 64-char key that
`BuyoutManager::GetTab()` (10-char) can never find: unreachable dead
rows, reported as imported, and idempotent against the wrong key so
re-running never heals it. Fix shape: apply the same first-10 truncation
when the id is longer than 10 hex chars.

### R1-3. `add_stash` ignores the legacy `n` label field — agreed

Old-format stash lists carry the tab label in `n` (injected by pre-0.16
`ItemLocation`); legacy-API tab JSON had no `name` at all. `add_stash`
reads only `LegacyStash::name`, so for such files every tab is skipped,
`location_targets` gets no `stash:<label>` keys, and all stash tab
buyouts report orphaned while item buyouts import — a plausible-looking
partial success. The validation script already does
`s.get("n", s.get("name", ""))`; the importer should match it.

### R1-4. Write failures are indistinguishable from a clean re-run — agreed

`report.success = true` is unconditional and `countSave` folds
`BuyoutSaveResult::Error` into `skipped`. A locked/read-only/full-disk
userstore produces "Imported: 0 … Skipped: N" — the same output as a
healthy second run. `skipped` also aggregates parse skips, id-less
items, unknown characters, unconvertible buyouts, and already-present
rows, so no single count is interpretable. Fix shape: count errors
separately, fail the report (or at least say so) when errors are
nonzero, and split "already present" from "skipped".

### R1-5. Imported inherited buyouts die on the next refresh — resolved by design revision

`convertBuyout` copies `legacy.inherited` through. Pre-0.16 persisted
inherited item rows (`IsSavable` filtered only on type). On the next
refresh `PropagateTabBuyouts` sees `IsInherited()` and either overwrites
the recovered price with the current tab price or — likely, given R1-2/
R1-3 orphan the tab buyouts — clears it via `Set(item, Buyout())`, which
deletes the row. Recovered prices vanish silently. Decision needed on
the fix: drop `inherited` on import, skip inherited rows, or import them
only when the owning tab buyout also imported.

### R1-6. One malformed item still discards a whole tab's items row — agreed

`LegacyDataStore`'s leniency stops at row granularity: `glz::read` of a
row's entire `std::vector<LegacyItem>` fails on a single malformed item
(non-optional `id`/`name`/`typeLine`; QString via std::string chokes on
a JSON null), and the row — an entire stash tab — is skipped. Every
buyout hashing into that tab then reports orphaned, which reads as "gone
forever" when the cause is one bad element. `getStruct` has the same
shape for the `buyouts`/`tab_buyouts` blobs: glaze returns a partially
populated map on error while the store stays valid. Fix shape: parse
items rows as `std::vector<glz::raw_json>` (or equivalent) and convert
per element, skipping only the bad ones.

### R1-7. Skip-existing discards manual prices behind auto-generated rows — resolved by design revision

`ApplyAutoItemBuyouts` and `PropagateTabBuyouts` persist rows for every
item with a priced note or under a priced tab, so by import time most
sellable items already have an `item_buyouts` row. The importer's
`ON CONFLICT DO NOTHING` then declines the legacy price — including a
MANUAL one, the only genuinely unrecoverable data — in favour of a
machine-generated row, and buries the fact in `skipped`. The
`MigrateItem` precedent points the other way: only a MANUAL *target* is
protected. Policy decision: e.g. legacy MANUAL overwrites non-MANUAL
existing rows; everything else keeps skip-existing.

### R1-8. The import result never reaches items, shop, or refresh state — agreed

`OnImportLegacyBuyouts` only calls `ReloadBuyouts()`. Unlike
`OnBuyoutChange` it never runs `PropagateTabBuyouts` (items under an
imported tab price stay blank — `BuyoutManager::Get` has no tab
fallback), never sets refresh locks, and never expires shop data (a shop
post right after import uses pre-import prices). The user's rational
read is that the import did nothing. Fix shape: after a successful
import, do what the existing buyout-change path does.

### R1-9. Character join hard-requires an id old files don't have — agreed

POESESSID-era character lists carry no `id`; pre-0.16 keyed characters
by name everywhere (`character:<name>`, empty unique id). With
`LegacyCharacter::id` empty, every character is skipped and all
character buyouts (item- and location-level) report orphaned. Fix
shape: fall back to the name as the location id when `id` is missing —
matching what master's character locations use when GGG provides no id.
No test covers a characters row lacking `id`.

### R1-10. Synchronous GUI import; no transaction; per-row prepare — agreed

The whole import runs in the GUI slot: full parse of a potentially
27 MB file, ~20k MD5 hashes, then 1,200+ autocommit INSERTs each
re-preparing constant SQL. The window stops repainting (no wait cursor,
unlike `OnExpandAll`/`OnCollapseAll`), and a force-quit leaves a partial
import with no rollback. Fix shape: wrap the save loops in
`m_db.transaction()`/`commit()` (the `userstore.cpp` migrate pattern),
hoist one prepared query per loop, and set a wait cursor.

### Round 1 resolutions — August 14, 2026

The design discussion (Tom + review follow-up) revised the design to a
plan/apply split around an editable XLSX plan file; full detail in
`legacy-buyout-import.md`. Dispositions:

- **R1-1**: fixed on-branch (8a4a4d14); gate accepts db_version 4 and 5.
- **R1-2**: truncate >10-char legacy-API stash ids to their first 10 —
  confident, not heuristic, since only the non-OAuth API returned long
  ids. Cross-check failures flag the row `needs-attention` per-row
  rather than aborting.
- **R1-3**: read the label from `n` (fallback `name`). The label is only
  the join key within the old file; the unique id bridges to the current
  store (labels are rename-prone). Plan shows old label and current name.
- **R1-4**: dissolved into the plan format — per-row outcomes, write
  errors counted separately from skips, failures fail loudly.
- **R1-5**: resolved by prefill default — inherited legacy rows default
  to `skip` (derived data; tab buyouts import separately); user can flip
  per row.
- **R1-6**: parse items per element (e.g. vector of raw JSON, convert
  individually) so one malformed item flags that item, not its tab; same
  guard for the buyouts blobs.
- **R1-7**: resolved by prefill defaults — legacy price imports over an
  existing non-MANUAL row; an existing MANUAL row defaults to `skip`
  (MigrateItem precedent); user can flip per row.
- **R1-8**: after apply, treat the import like any other buyout update
  (propagate tab buyouts, refresh locks, expire shop data).
- **R1-9**: character fallback chain id → name match → equipped-item
  search (propose, reason `character-matched-by-items`) → orphan. The
  search uses whatever the userstore holds; no fetch-state gating — the
  refresh precondition stays external (dialog warning only).
- **R1-10**: transaction-wrapped apply, hoisted prepared statements,
  wait cursor; errors reported.

### Cleanup notes (below the round's severity cap)

- R1-C1. The four `INSERT_*`/`UPSERT_*` SQL constants in `buyoutrepo.cpp`
  could collapse to one guarded upsert (`DO UPDATE … WHERE :overwrite`).
- R1-C2. `convertBuyout` re-implements `BuyoutManager::Deserialize` over
  a struct twin of `SerializedBuyout`.
- R1-C3. The item and location apply loops in the importer share ~20
  duplicated lines.
- R1-C4. `BuyoutManager::ReloadBuyouts()` duplicates the head of
  `Load()`.
- R1-C5. The `std::function` recursion in `add_stash` should be a plain
  helper (cf. `flattenStashList`, `stashrepo.cpp`).
- R1-C6. `item_targets` is built for every stored item (~19.6k hashes on
  the validated file) when only the hashes present in `data().buyouts`
  (~1.1k) can ever match.

### Conventions

- F54's reachability note ("`LegacyDataStore` has no callers outside
  `src/legacy/`") is falsified by this branch; `cleanup/findings.md`
  carries a dated update.
- Build and both new tests pass; clang-format clean. `tst_networkcapture`
  fails on this branch but the failure is pre-existing from master
  (timezone assertion introduced by f53d8cb1), unrelated to this work.

## Round 2 — August 14, 2026 (plan/apply revision, commits 6c25c427…b64f3e29)

Medium-effort review of the four plan/apply implementation commits;
eight findings confirmed by verification, two candidates refuted (the
`ambiguous` prefill matches documented intent; the `appendUnique`
equality narrowing reproduces the old semantics). All open.

### R2-1. Apply lost the never-clobber guarantee — fixed

`IMPORT_ITEM_BUYOUT`/`IMPORT_LOCATION_BUYOUT` use `ON CONFLICT DO
UPDATE` on every column where the tracer bullet used `DO NOTHING`; the
only protection is the prefill computed at *plan* time (`prefillAction`
skips only when the existing source is MANUAL), and `applyPlan` never
re-reads current buyouts. Consequences: existing AUTO/GAME rows are
overwritten even in the one-click path (that half is the agreed R1-7
default); but a *saved* plan applied later — after the user hand-priced
items, or after the auto-refresh timer fired inside the file dialog's
nested event loop — silently replaces manual prices set since the plan
was generated, reporting them "imported". Fix shape: apply must re-check
the target row's current source at write time (e.g. guard the upsert
with `WHERE source != 'manual'` unless the plan row's `existing_source`
was already manual, or re-read and demote conflicting rows to a
`skipped-existing-manual` outcome).

### R2-2. Post-apply propagation destroys imported inherited rows — fixed

`OnLegacyBuyoutsImported()` runs `PropagateTabBuyouts()` (per R1-8),
which deletes or overwrites any imported item buyout whose `inherited`
flag survived — the prefill only *defaults* inherited rows to skip; a
user can flip one to import and applyPlan writes the flag through. The
workbook and dialog then claim success for a row propagation just
removed. Fix shape: strip `inherited` on write (an explicitly imported
row is by definition no longer derived), or refuse `import` +
`inherited: true` at validation with a clear per-row error.

### R2-3. Workbook save failure after commit leaves caches stale — fixed

`applyPlan` commits the DB transaction, then saves the annotated
workbook; if the save fails (file open in Excel on Windows, network
share, disk full) `success` stays false and both callers skip
`OnLegacyBuyoutsImported()`, leaving BuyoutManager/model/shop stale
against an already-mutated database — and later edits write back from
the stale cache. Fix shape: after a successful commit, always reload and
propagate; report the workbook-save failure as a warning, not a failed
import.

### R2-4. Skip rows are validated before the skip short-circuit — fixed

Buyout-field validation (value/type/currency/`convertBuyout`) runs
before the `action == "skip"` test, so editing the value cell of a row
the user opted out of (e.g. clearing an orphaned row's price) errors and
— via the all-or-nothing gate — stamps every other row `not-applied`.
The id-validation block already sits after the skip test, showing the
intended split. Fix: hoist the skip check above the buyout validation.

### R2-5. A blank row aborts the whole plan — fixed

An empty row inside the sheet dimension (contents cleared rather than
row deleted, or a blank separator row — Excel keeps both in the
dimension) yields an empty `action`, which is treated as a hard error
and aborts the import. The header scan in the same function skips empty
cells; the row loop should skip fully blank rows the same way.

### R2-6. Character lookup ignores the league — fixed

`createPlan` calls `getCharacterList(m_realm)` without `m_league`
(which is in scope and used by the stash lookup two lines up).
Characters from other leagues — including Standard heirs of dead-league
characters, which keep their ids and item ids — enter the equipped-item
match and produce `character-matched-by-items` proposals for the wrong
league. One-word fix: pass `m_league`.

### R2-7. Labels starting with '=' become Excel formulas — fixed

Cells are written as raw QStrings and QXlsx dispatches any string
beginning with `=` to `writeFormula` — verified empirically: a tab named
`=== SELL ===` renders as `#NAME?` in the plan. Legibility, not
integrity (those columns are not read back), but separator-named tabs
are common and are exactly what a reviewer wants to see.
`strings_to_hyperlinks_enabled` also defaults true. Fix shape: write
text cells with an explicit string type / disable formula and hyperlink
interpretation for data cells.

### R2-8. The v5 test can no longer catch a matching regression — fixed

`plansVersion5StampedFiles` (the only db_version-5 test) asserts
`success`, `total == 5`, and plan-file existence — but `total` counts
rows before any hash lookup, so it passes identically when every row
orphans. A regression in `LegacyItem::hash()` or the version gate stays
green for the exact file shape most ≤0.15 upgraders have. The R1-1
rationale comment was also dropped from test and source. Fix: assert
matched/orphaned counts (or apply the plan and check the repo), and
restore the comment.

### Round 2 resolutions — August 14, 2026

All eight findings fixed on-branch in three commits:

- **194745eb** — R2-4 (skip short-circuit hoisted above buyout
  validation), R2-5 (fully blank rows ignored), R2-6 (character lookup
  league-filtered), R2-7 (string cells written verbatim via
  `writeString`; string-to-hyperlink conversion disabled). Pinned by
  `skipRowEditsDoNotAbortTheImport`, `blankPlanRowsAreIgnored`,
  `characterMatchingIsLeagueScoped`, `formulaLikeLabelsStayText`.
- **8b257ce6** — R2-1 (write-time manual guard inside the import
  transaction: a manual row is only overwritten when the plan row's
  `existing_source` was already manual; a manual row byte-identical to
  the incoming write is the plan's own earlier import and still reports
  already-present, preserving idempotence reporting; new
  `skipped-existing-manual` outcome and `Protected manual` count), R2-2
  (inherited flag stripped from imported rows), R2-3 (`success` reflects
  the committed database; a post-commit workbook-save failure becomes
  `report.warning`, both UI callers still reload/propagate and surface
  it — no failure-injection test for the save path; the ordering is the
  fix). Pinned by `manualPricesSetAfterPlanningAreProtected`,
  `importedInheritedBuyoutsLoseTheFlag`.
- R2-8 — `importsVersion5StampedFiles` restored to end-to-end parity
  (matched/orphaned counts, apply, repo contents) and the R1-1 rationale
  comment restored in test and source.
