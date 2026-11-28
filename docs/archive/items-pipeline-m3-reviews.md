# Items Pipeline Milestone 3: Review Findings

Review-round findings for `items-pipeline-m3.md`, recorded from
round 1 per the M2 convention: each finding gets an `R<round>-<n>`
number, a verdict (accepted / rejected / amended, with reasons), and
a resolution naming the decision or criterion it changed. The spec
cites findings by number; this file is the record.

## Revision log

- **Revision 1 (July 30, 2026)** — initial draft. Written after the
  S1-M3 sort-profiling spike (evidence in `m3-sort-profile-result.md`)
  and the same-day lever hold point, whose decisions it records:
  levers A + B both committed (D1, D2), the intended
  `(name, uid, hash)` tie-break order (D5, fixing F67), and the
  key-memory measurement run pre-freeze as a spike extension
  (~286 B/item accounted, ~266 MB naive at 1m). No review rounds yet.
- **Revision 2 (July 30, 2026)** — round 1 incorporated (external
  review, eight findings R1-1…R1-8, all verified against the code and
  accepted; none challenged the hold-point decisions). Key changes:
  content deltas erase **source-scoped** within the stable-key display
  bucket (R1-1); the final snapshot performs authoritative **row**
  reconciliation, covering deletions and new unfetched tabs (R1-2);
  selection becomes an *intent* keyed by stable item id that survives
  mid-refresh removal and is re-adopted globally (R1-3); the
  intersection contract gains a metadata half, retiring M2 R7-2's
  metadata-only exception (R1-4); the key/sortedness state machine is
  specified with **transient** By-Tab keys and resident keys only for
  By-Item — a simplification that also shrinks the memory story
  (R1-5); buyout invalidation gains a batching/observability contract
  and the migration paths (R1-6); deltas the active search applies
  leave it clean, renegotiating M2 D9 rule 1 for the active search
  only (R1-7); the broad-filter default-expanded worst case is
  documented and budgeted (R1-8).

- **Revision 3 (July 30, 2026)** — round 2 incorporated (external
  review of revision 2, six findings R2-1…R2-6, all verified and
  accepted; R1-2/R1-4/R1-7/R1-8's resolutions confirmed adequate).
  Key changes: the selection-intent window closes on **every**
  `RefreshFinished` outcome, failure included (R2-1); visible
  multi-source buckets get a remove-runs + sorted-merge-insert-runs
  contract, and D3's O(1) batch claim is corrected to O(runs)
  (R2-2); **By-Tab key residency is restored** for the active
  search's materialized buckets — round 1's transient-key resolution
  had silently altered the hold point's cached-key choice, and
  revision 3 specifies eviction/invalidation *within* the settled
  choice instead (R2-3); key residency is scoped to the active
  search, with lazy rebuild on reactivation (R2-4); user buyout
  commands batch at command scope (R2-5); three stale references
  fixed, including the nonexistent `MigrateTab` (R2-6).

- **Revision 4 (July 30, 2026)** — round 3 incorporated (external
  review of revision 3, four findings R3-1…R3-4, all verified and
  accepted, plus two review clarifications accepted into the
  resolutions; R2-1/R2-2/R2-5/R2-6 confirmed closed, the
  selection-terminal and interleaved-merge contracts confirmed
  adequate). Key changes: key residency becomes an axis independent
  of sorted validity, with one hydration rule for the
  sorted-but-keyless state and an eager-at-activation carve-out for
  clean By-Item searches — activation decides dirtiness first
  (R3-1); the invalidation contract drops revision 2's "By-Tab key
  caches don't exist" fossil and states each cause's effect on
  every resident key vector (R3-2); nested pass/command batches
  coalesce and only the outermost boundary emits (R3-3); migration
  is re-pinned to its real batch boundary and the snapshot's
  four-pass pricing sequence is **required** to emit one model
  batch (R3-4). The round was a consistency pass over revision 3's
  restored cache, not a re-litigation of any settled decision.
- **FROZEN (July 30, 2026, at revision 4)** — Tom froze the spec
  after round 3's reviewers judged it at diminishing returns and
  the focused consistency check they prescribed (key residency,
  activation ordering, nested batching — run as a scoped
  fresh-eyes pass over the full document) surfaced only
  wording-level findings, all folded into revision 4 pre-commit.
  The review series is closed. Post-freeze changes follow the M2
  convention: recorded amendments with reasons, never silent
  edits. Production implementation proceeds against revision 4,
  with M1-M3 as the completion gate.

## Round 1 (July 30, 2026 — external review of revision 1)

Eight findings, all **accepted** after claim-by-claim verification
(M2 D2/D6/D9/R6-3 texts; `buyoutmanager.cpp:369` (`MigrateItem`),
`search.cpp:261-290`, `column.cpp`). None challenges the settled
lever or tie-break decisions.

| # | Severity | Finding | Verdict and resolution |
|---|---|---|---|
| R1-1 | High | D3 confused fetch-source replacement with display-bucket replacement: a Map/Unique child delta, applied as "replace the bucket's rows", would erase sibling sources sharing the parent display bucket — contradicting M2 D2's accepted mixed-generation behavior. | Accepted. Verified against M2 D2 (atomic unit = fetch source) and D6/R5-1 (buckets key on stable `(type, id)`, aggregating sources). D3's replacement is now **source-scoped within the display bucket**: erase exactly the rows whose items were fetched from the delta's `FetchSourceKey`, insert the arrivals; sibling sources untouched. Source-scoped erase (over rebuild-from-published) chosen because it stays within the signal payload and mirrors the reconciliation's erase-by-predicate shape. New pin `childDeltaPreservesSiblingSourcesInParentBucket`. |
| R1-2 | High | The final reconciliation (metadata + moves only) omitted final-only additions and deletions: M2 D6 keeps deleted tabs (with items) and newly discovered unfetched tabs snapshot-boundary-only, so no delta removes/inserts them and the model would retain deleted content and miss new empty tabs. | Accepted. Verified against M2 D6's publication table. The final snapshot now performs **authoritative row reconciliation**: one pass diffing the model against the post-snapshot published state per stable key — deleted buckets/rows removed, newly listed tabs inserted (unfiltered searches), metadata refreshed, bucket order fixed by moves; row operations only, never a reset. New pins `finalReconciliationRemovesDeletedTabs`, `finalReconciliationInsertsNewlyListedEmptyTabs`. |
| R1-3 | High | Immediate remove-then-insert application loses M2 R6-3's global cross-tab selection fidelity: when the removal delta arrives before the insertion delta, naive clearing breaks `reselectionSurvivesCrossTabMove`; `byItemSelectionSurvivesMerge` as drafted said "removed clears" immediately. | Accepted. Verified against M2 R6-3 (global identity lookup, cross-tab pin). D3 gains a **selection-intent contract**: intent is keyed by stable item id, survives row removal during an active refresh, is re-adopted globally when any later delta inserts the id, and is cleared at the final reconciliation if the id is absent or by user action; outside an active refresh, removal clears immediately. New pin `selectionIntentSurvivesCrossTabMoveAcrossDeltas` (the M2 pin's successor under no-reset machinery); `byItemSelectionSurvivesMerge` reworded accordingly. |
| R1-4 | High | D3 promised immediate new-tab insertion and rename/move/color updates but inherited M2's item-based intersection gate, which deliberately excludes metadata-only (empty) deltas — post-M3 those would wait indefinitely, especially after terminal failure (no throttled tick, no guaranteed refilter). | Accepted. Verified against M2 D6 ("D9 deliberately has no metadata-only intersection trigger") and R7-2. The intersection contract gains a **metadata half**: every delta's location anchor lands in the canonical inventory immediately (existing M2 machinery), and a delta whose stable key owns a visible bucket — or would create one in an unfiltered search — applies metadata now (`dataChanged`, moves, empty-bucket insertion), item intersection notwithstanding. M2 R7-2's exception is explicitly renegotiated and retired. New pin `metadataDeltaAppliesWithoutItemIntersection` (empty rename/move/color/new-tab, no final snapshot, persisting after terminal failure). |
| R1-5 | Medium-high | The lazy bucket/key state machine was incomplete and internally inconsistent: D1 said collapsed buckets hold no keys, D2 said they retain arrival order; expand → sort → collapse left key eviction, order retention, and arrival-order reconstruction unspecified, and each candidate resolution broke a stated claim. | Accepted, resolved by simplification: **By-Tab keys are transient sorting scratch** (built for a sort, discarded after; what persists is the bucket's order and a sorted-validity flag), and **resident keys exist only for the By-Item flat bucket** (D4 needs them for merges). Collapse changes nothing (order and flag persist; arrival order is never reconstructed); invalidation acts on flags, not caches. D1/D2 now state the full transition table (expand, collapse, delta, column switch, direction flip, buyout invalidation). Strengthens the memory claim: resident key memory is By-Item only; By-Tab peak is one bucket (~0.2 MB). New pins `sortedOrderSurvivesCollapse`, `byTabKeysAreTransient`; `sortColumnSwitchRebuildsMaterializedKeysOnly` superseded by `sortColumnSwitchResortsVisibleBucketsOnly`. |
| R1-6 | Medium-high | The buyout invalidation contract lacked observability and batching: no statement of when a visible bucket reorders, how Price/Date **cells** repaint when another column is active, whether migration (`MigrateItem`/`MigrateTab`) counts as mutation, or how bulk pricing passes avoid one reorder per `Set` (quadratic risk on the flat bucket). | Accepted. Verified `MigrateItem` at `buyoutmanager.cpp:369` mutates the lookup result. D1 rule 4 now specifies: the choke-point inventory includes item/tab set-and-clear, **migration**, and the scoped and final pricing passes; pricing passes batch — one invalidation batch, hence at most one reorder/model update per pass; user edits apply immediately; and Price/Date cells repaint via `dataChanged` for affected visible rows regardless of the active sort column (reordering alone is gated on the active column). New pins `pricingPassYieldsSingleModelUpdate`, `priceCellsRepaintUnderAnySortColumn`; `priceKeysFollowBuyoutEdits` extended to migration. |
| R1-7 | Medium | Inheriting M2 D9 rule 1 unchanged makes the active search dirty on every delta it just applied, so switching away and back triggers a spurious full refilter; the final reconciliation didn't clear the flag either. | Accepted. Verified against M2 D9 rule 1's wording. Renegotiated **for the active search only**: a delta the active search applied — including one correctly adjudicated as no visible change — leaves it clean; any skipped application leaves it dirty (fail-safe); the final reconciliation clears it. Background searches keep rule 1 verbatim. New pin `appliedDeltasLeaveActiveSearchClean`. |
| R1-8 | Medium | "Filtered results are small by construction" is not a bound: a broad filter can match nearly the whole collection while default-expanding every bucket, approaching full keyed sorting — a case no budget covered. | Accepted. Verified `m_filtered` semantics (`search.cpp:261-290` — any excluded item sets it). D2 drops the claim and states the honest ceiling (key build + sort of ~everything atop the filter loop, ~0.9 s estimated at 1m; memory unaffected under R1-5's transient keys). M1-M3 gains a broad-filter default-expanded scenario with a ≤ 1.2 s budget at 1m. |

Round narrative: the round's through-line is that revision 1
specified the delta path's *happy* grain (one content delta, one
visible By-Tab bucket) and under-specified the boundaries — source
vs. display-bucket grain (R1-1), snapshot-boundary effects (R1-2),
cross-delta state (R1-3), metadata-only deltas (R1-4), state-machine
transitions (R1-5), batching (R1-6). The lesson mirrors M2 round 2's:
contracts that hold at the single-event grain must be re-checked at
the sequence grain (two deltas in flight, a pass of many mutations, a
whole refresh). One finding (R1-5) was resolved by simplifying the
design rather than adding state — transient keys retire the cache
lifetime questions instead of answering them. *(Round 2 note: that
resolution overstepped — see R2-3, which reversed the transient-key
choice for By-Tab while keeping the flag machinery it introduced.)*

## Round 2 (July 30, 2026 — external review of revision 2)

Six findings, all **accepted** after verification (`MigrateTab`'s
nonexistence and the `OnBuyoutChange` multi-row loop
(`ui/mainwindow.cpp:541-567`) confirmed in code; R2-1/R2-2 confirmed
against the revision-2 contracts; R2-3 confirmed against the
hold-point record). The round also confirmed R1-2, R1-4, R1-7, and
R1-8 as adequately resolved.

| # | Severity | Finding | Verdict and resolution |
|---|---|---|---|
| R2-1 | High | The selection-intent window ran from first delta to final reconciliation, but `FailedRefresh` emits no final snapshot (M2 D4) — after a failed refresh an absent item's intent could survive into a later refresh and unexpectedly reselect it. | Accepted. The intent window now closes on **every** `RefreshFinished` outcome: success closes it at the final reconciliation; failure performs the absence check against the visible result at the terminal event itself. New pin `selectionIntentClearsOnTerminalFailure`. |
| R2-2 | High | Source-scoped removal preserves siblings, but sorting the arrivals alone cannot establish a visible multi-source bucket's global order — arrivals must merge into the retained sibling rows; and D3's "O(1) row-op batches" claim contradicted the O(runs) reality D4 already acknowledged. | Accepted. D3 now specifies remove-runs plus sorted-merge insert-runs for visible multi-source buckets (O(runs) model operations, O(bucket) work) and the batch-count claim is corrected to O(runs), O(1) in the common single-source case. `childDeltaPreservesSiblingSourcesInParentBucket` extended with heavily interleaved keys and persistent-index assertions. |
| R2-3 | High | Revision 2's transient-By-Tab-keys resolution (R1-5) silently changed the hold point's **cached**-key lever: direction flips and invalidations now rebuilt keys instead of reusing them, altering a settled decision rather than specifying its lifecycle. | Accepted — the finding restores the settled choice. Key residency returns for the active search's materialized buckets (expanded By-Tab and the By-Item flat bucket): keys persist across re-sorts and direction flips; **collapse evicts keys while order and flag persist** (safe, because invalidation acts on flags independently of key residency — answering R1-5's original question within the cached design); column switches discard. The broad-filter fully-expanded memory ceiling (~a full-collection footprint) is now stated and budgeted. New pin `keyResidencyFollowsMaterialization`; `byTabKeysAreTransient` retired. |
| R2-4 | Medium | By-Item key residency across view/search switches was unspecified: "while that view is active" never defined active, and N background By-Item searches could each hold ~222–266 MB. | Accepted, generalized to all resident keys: **residency is scoped to the active search** — deactivation evicts every key vector (orders and flags persist); reactivation rebuilds lazily at the next event that needs keys, not eagerly. At most one search holds resident keys at any time. New pin `residentKeysScopedToActiveSearch` (aggregate-memory assertion across multiple searches, By-Item included). |
| R2-5 | Medium | "User edits apply immediately" still permitted one reorder per `Set`: `OnBuyoutChange` loops every selected row and then propagation runs, recreating in one command the quadratic behavior R1-6's pass-batching prevents. | Accepted. Verified the loop at `ui/mainwindow.cpp:541-567`. "Immediate" is redefined as **one batch at user-command end**; per-`Set` reordering is forbidden at every batch boundary (pass or command). New pin `multiSelectionBuyoutEditReordersOnce`. |
| R2-6 | Low | Three stale references: D3 still called the intersection machinery "unchanged" despite the metadata half; traceability still said "one-bucket replace chosen" despite R1-1; and `MigrateTab` was named and pinned but does not exist (only `MigrateItem` does). | Accepted. All three corrected; the round-1 table's `MigrateTab` mentions stand as historical record, corrected here. The choke-point inventory now names `MigrateItem` alone and notes there is no tab-level migration. |

Round narrative: rounds 1 and 2 form a pair — round 1 pushed the
spec from event grain to sequence grain, and round 2 caught the
places where round 1's own resolutions were under-specified at their
boundaries (intent lifetime at the failure terminal, merge order in
multi-source buckets, residency across searches) or overstepped
(R2-3). The R2-3 lesson is a process one, worth keeping: a review
resolution may simplify *within* a settled decision, but changing
the settled decision itself — however defensible on the merits —
belongs to a hold point, not a revision.

## Round 3 (July 30, 2026 — external review of revision 3)

Four findings, all **accepted** after verification
(`OnBuyoutChange`'s trailing `PropagateTabBuyouts` call confirmed at
`ui/mainwindow.cpp:578`; `MigrateBuyouts` confirmed to run from
snapshot processing at `itemsmanager.cpp:152`, sequenced with the
auto-buyout and propagation passes at `itemsmanager.cpp:152-155`;
R3-1/R3-2 confirmed against the revision-3 texts). The round also
confirmed R2-1, R2-2, and R2-6 cleanly closed, R2-5's outcome
correct pending R3-3's nesting rule, and the selection-terminal and
interleaved multi-source merge contracts adequate. Review of the
proposed resolutions added two clarifications, both accepted and
folded in below.

| # | Severity | Finding | Verdict and resolution |
|---|---|---|---|
| R3-1 | High | Eviction creates a sorted-but-keyless state later rules don't handle: re-expanding a valid bucket builds no keys, yet a direction flip promised resident-key reuse; worse, a reactivated clean By-Item search is visible but keyless, so the first delta merge would absorb a ~368 ms key build at 1m — contradicting D4's "keys resident" premise and the ≤ 50 ms delta budget. | Accepted. Residency and sorted validity are now independent axes with sorted-but-keyless a named state. One **hydration rule**: any key-consuming operation (flip re-sort, delta merge, buyout reorder) hydrates missing keys first — By-Tab bounded by the 576 cap, lazy per bucket. **By-Item hydrates eagerly at activation** (deliberate carve-out from R2-4's lazy rule: the flat bucket is always visible and sorted, so hydration is never speculative, and the cost lands on the user action that activated the view, not on a background delta). Review clarification, accepted: **activation decides dirtiness first** — a dirty search refilters once and that rebuild supplies the keys; only a clean search hydrates, so stale keys are never hydrated and immediately rebuilt. New pins `reexpandedBucketFlipHydratesOnce`, `byItemActivationDecidesDirtinessFirst`; new budget line (clean By-Item reactivation ≤ 0.5 s at 1m); D1 rule 3, D2 rules 2/7, D4 rule 1, and two existing residency pins reworded. |
| R3-2 | High | The invalidation contract retained revision 2's "never on By-Tab key caches, which don't exist" after revision 3 restored resident By-Tab keys — an expanded Price/Date-sorted bucket could clear its flag and then re-sort on stale resident keys. | Accepted — a straight revision-2 fossil, now load-bearing and wrong. The contract preamble covers **every resident key vector, By-Tab and By-Item alike**, and each cause states its key effect: deltas discard the replaced source's entries and add arrivals' entries via the merge; column switches discard everything; buyout batches rebuild affected entries of Price/Date-sorted materialized buckets' vectors before the reorder (other columns: keys untouched, cells still repaint). `priceKeysFollowBuyoutEdits` extended to name the expanded By-Tab bucket explicitly. |
| R3-3 | Medium | Nested buyout batches were contradictory: a user command must emit one batch at command end, but its trailing `PropagateTabBuyouts` call is itself a pricing pass required to emit at pass end — permitting an inner propagation update plus an outer command update. | Accepted. Verified the call at `ui/mainwindow.cpp:578`. New batching rule: **nested pass/command batches coalesce; only the outermost boundary emits** — an inner boundary inside an open batch accumulates into it. `multiSelectionBuyoutEditReordersOnce` extended to include the propagation call in the exercised command. |
| R3-4 | Low | `priceKeysFollowBuyoutEdits` grouped migration with user edits "at command end," but `MigrateItem` runs from `ItemsManager::MigrateBuyouts` during snapshot processing — no user command exists there. | Accepted. Migration re-pinned inside the snapshot's outer batch (`itemsmanager.cpp:152`) — with that batch required (below), it is always migration's containing batch. Review clarification, accepted (upgrading the draft resolution's MAY to a requirement): the snapshot's `MigrateBuyouts` → `ApplyAutoTabBuyouts` → `ApplyAutoItemBuyouts` → `PropagateTabBuyouts` sequence **must** run as one outer model-invalidation batch — nothing observes UI state between the passes, and four separate batches could mean several full By-Item reorders; persistence writes unchanged. New pin `snapshotPricingSequenceEmitsOneModelBatch`. |

Round narrative: round 3 is the settling round — no new contracts,
no renegotiations, only the reconciliation of revision 3's restored
key cache with the rules written around its predecessor (R3-1,
R3-2) and the closing of the batching boundaries' composition
(R3-3, R3-4). Both High findings trace to the same root: R2-3
restored residency, and the surrounding text was only partially
re-derived against it. The round's two clarifications sharpen
resolutions rather than reopen them, and the reviewers judged the
spec at diminishing returns: revision 4 warrants a focused
consistency check of key residency, activation ordering, and nested
batching — not another broad exploratory round — and freezes if
that passes.
