# Items Pipeline Milestone 3: Delta-Native Items Model

Status: **FROZEN at revision 4** (July 30, 2026); **IMPLEMENTED
July 30–31, 2026** (stages S0–S8 on branch `items-pipeline-m3`).
M1-M3 — the performance/memory checkpoint in the acceptance
criteria, gating M3 completion — **PASSED at the S7 formal gate**
with Tom's go, after S5's By-Item merge miss was remedied (the A′
translate-and-notify rebuild); full tables and the miss/remedy
record: `m1-m3-result.md`. Frozen on Tom's decision after the round-3 reviewers
judged the spec at diminishing returns and the prescribed focused
consistency check (key residency, activation ordering, nested
batching) passed with only wording-level findings, folded into
revision 4 before it was committed. Post-freeze changes follow the
M2 convention: recorded amendments with reasons, never silent
edits. Three amendments are recorded (all July 31, 2026): the
definition of a *filtered* search (out of S4's implementation review
round 2 — see the markers at D2 rule 5 and in D3's metadata half),
the deletion of the D9 intersection sets after S5's throttle
retirement removed their final consumer (out of S5's review round 1
— see the marker in D3), and the complexity criterion's stated
bound (out of the S8 external review — see the marker at the
design-review criteria: ordinary deltas pay an O(log tab-count)
ordered-map bucket-row lookup and structural ops an O(tab-count)
remap, both chosen and accepted at S4 implementation review
round 1). A fourth is recorded November 12, 2026: R1-3's selection
intent is extended to the other selected rows within their tab (#282 —
see the marker after R1-3).
Review rounds 1–3 (external; R1-1…R1-8, R2-1…R2-6, and
R3-1…R3-4, all eighteen verified and accepted) are incorporated
throughout. Round 1's largest changes: D3's source-scoped
replacement grain (R1-1), the final snapshot's row reconciliation
(R1-2), the selection-intent contract (R1-3), the metadata half of
the intersection contract (R1-4). Round 2 closed the boundaries
round 1 opened — the intent window now ends at every terminal
outcome (R2-1), multi-source buckets merge rather than merely sort
arrivals (R2-2), and **key residency was restored to the hold
point's cached-key choice** after round 1's transient-key
resolution overstepped it (R2-3, with residency scoped to the
active search per R2-4). Round 3 reconciled the restored cache's
remaining inconsistencies: residency became an explicit axis with a
single hydration rule and an eager-at-activation carve-out for
By-Item (R3-1), invalidation now covers every resident key vector
(R3-2), nested buyout batches emit only at the outermost boundary
(R3-3), and migration is pinned at its true batch boundary, with
the snapshot's pricing sequence required to emit one batch (R3-4). Unlike M2, the pre-spec evidence is already in hand: the
parent plan's "profile before choosing levers" obligation was
discharged by the S1-M3 sort-profiling spike (July 30, 2026, branch
`spike/m3-sort-profile`, evidence in `m3-sort-profile-result.md`
beside this spec), and the lever hold point ran the same day. **At
that hold point Tom selected: levers A and B both committed (D1, D2),
the intended `(name, uid, hash)` tie-break order (D5, fixing F67 as a
side effect), and the key-memory measurement run pre-freeze rather
than deferred** — it ran the same day as a spike extension (~286
B/item accounted; ~266 MB naive at 1m; section 4 of the spike
result). This spec consumes the M3 inbox in `items-pipeline.md`
("Inputs accumulated for the M3 spec") and the spike's evidence; the
traceability table at the end maps every input to the decision,
deferral, or acceptance criterion that consumed it.

Citation convention: bare D-numbers (D1, D2, …) in this document are
this document's decisions; M2's decisions are cited qualified ("M2
D9"), as are the network redesign's. F-numbers are the findings
register (`docs/cleanup/findings.md`); `R1-*` (and later `R2-*`, …)
are this spec's review-round findings, recorded with verdicts and
resolutions in `items-pipeline-m3-reviews.md` from round 1; pinned
test names are quoted in `camelCase`. "The spike" unqualified means
S1-M3.

## Staleness preamble

Written against commit `3549b214` (master, July 30, 2026 — the M2
merge, PR #185). The load-bearing code assumptions; re-verify these
anchors before implementing against any section, and re-verify the
whole list if the model layer or the delta path has been touched
since:

- **Delta application (M2 D9)**: `MainWindow::OnTabRefreshed` /
  `OnChildrenReconciled` (`ui/mainwindow.cpp:709-736`) mark every
  search items-dirty (rule 1), intersection-gate against the current
  search, and call `ScheduleThrottledRefilter()`
  (`ui/mainwindow.cpp:756`) — a non-resetting trailing throttle with
  period S = 60 s (`m_delta_throttle`). `OnDeltaThrottleTimeout` →
  `ModelViewRefresh` (`ui/mainwindow.cpp:828`), which captures
  expansion and scroll, refilters, resets the model, and restores
  (`RestoreViewExpansion`, `ReselectCurrentItem`, the scroll anchor).
- **The reset**: `ItemsModel::beginUpdate()` is literally
  `beginResetModel()` (`items_model.h:28`). One sort column and order
  per model (`m_sort_column`/`m_sort_order`; the pinned default is
  column 0 = Name, descending) with a lazy `m_sorted` flag:
  `Search::FilterItems` ends with `SetSorted(false)` and the sort
  actually runs on activation (`search.cpp:461-466`) via
  `ItemsModel::sort` → `Search::Sort` (`search.cpp:201`) →
  `Bucket::Sort` (`bucket.cpp:41`), a per-bucket `std::sort` calling
  `Column::lt` per comparison.
- **Comparator surface**: 26 columns (`search.cpp:33-59`), three
  comparator implementations — the `Column::lt`/`Column::multivalue`
  base (`column.cpp:28-67`; two static `QRegularExpression`s, string
  fallback path), and the `PriceColumn::lt`/`DateColumn::lt`
  overrides, which read `BuyoutManager` per comparison. Ties fall
  through to `Item::operator<` (`item.cpp`), whose hash term is dead
  code (F67).
- **Search state rebuilt by every refilter** (`search.h`):
  `m_bucket_by_tab`/`m_bucket_by_item` (`ViewMode::ByTab`/`ByItem`),
  expansion keyed by stable `(type, id)` (M2 R6-3,
  `m_expanded_keys`), the reselection index `m_visible_by_id`, the
  intersection sets `m_visible_sources`(`_by_tab`) (as-at-freeze
  state; deleted mid-implementation by the July 31 amendment in D3),
  and the scroll anchor. `defaultExpanded()` is `m_filtered || ByItem`
  (`search.h:86`). Buckets hold `std::shared_ptr<Item>` vectors;
  per-tab caps bound a display bucket at 576 items (quad tab).
- **The M2 signal surface is settled ground**: worker
  `TabRefreshed(location, items)` and
  `ChildrenReconciled(parent, expected)` deltas, the typed
  `RefreshFinished` terminal, the unchanged final `ItemsRefreshed`
  snapshot (M2 D8), scoped per-delta pricing in `ItemsManager` (M2
  D7), and `SourceKeyedItems` on both worker and manager (the M2-M2
  remedy pair). M3 changes none of it; this spec touches only Layer 3
  (search/model/view).
- **Initial cached load**: the parse thread publishes one snapshot
  (`ItemsRefreshed(..., true)`); the first population of a search's
  model has no view state to preserve.

## Scope

M3 makes Layer 3 consume deltas natively and retires the two costs
the evidence localized: the destructive whole-model reset on the
refresh path, and the comparator-bound re-sort. Concretely: a tab
delta becomes bucket-scoped model operations instead of a throttled
reset (D3); sorting runs on precomputed cached keys (D1, hold-point
lever A) and only for buckets whose order is visible (D2, hold-point
lever B); the By-Item flat bucket gets a per-delta sorted merge (D4);
the tie-break becomes the intended `(name, uid, hash)` order, fixing
F67 (D5). The user-initiated full refilter survives as a
non-refresh-driven path and gets cheap by the same levers (D6).

Out of scope, each with reasons: the filter loop, born-sorted
masters, regex micro-optimization as a lever, F66's legacy-store
rekeying, key persistence (D7); pricing semantics (settled, M2 D7 —
M3's concern is model operations, not pricing).

## Decisions

### D1. The central lever: precomputed, cached sort keys (hold point, lever A)

Sorting never calls `Column::lt` per comparison. When a bucket sorts,
a key vector is built for its items and `Bucket::Sort` orders
`(key, item)` pairs with plain tuple comparison; the bucket adopts
the resulting item order.

**Key shape.** The key is the comparator's own tuple, materialized
once per item — the comparator remains the single source of ordering
truth, and keys are derived from it, never a second definition:

- Base columns: the `multivalue` tuple `(d1, s1, d2, s2)` exactly as
  `Column::multivalue` computes it (`column.cpp:28-61`), plus the
  tie-break suffix.
- `PriceColumn`: `(currency rank, value)` plus the suffix.
- `DateColumn`: `(last_update)` plus the suffix.
- The tie-break suffix is `(PrettyName, uid, hash)` — the intended
  order per D5.

The two identical `PrettyName` strings on the base string path (`s2`
and the suffix's first element) share one buffer; `uid` and `hash`
are CoW copies of the `Item`'s own members and cost nothing while the
item lives. Both facts are measured (spike section 4).

**Residency: keys are cached for the active search's materialized
buckets (R2-3, R2-4 — the hold point's cached-key choice, with its
lifecycle specified).** A *materialized* bucket is an expanded
By-Tab bucket or the By-Item flat bucket. **Key residency and
sorted validity are independent axes (R3-1)**: eviction removes
keys while order and flag persist, so *sorted-but-keyless* is a
legitimate, explicitly handled state, not a gap. The rules:

- Within the **active search**, a materialized bucket keeps its key
  vector resident from its first sort until evicted — re-sorts and
  direction flips reuse it without rebuilding (the spike's 40×
  cached path), and a bucket whose sorted flag is valid skips
  sorting entirely (D2's flags — stronger still).
- **Hydration rule (R3-1): any operation that consumes keys — a
  direction flip's re-sort, a delta's merge, a buyout batch's
  reorder — hydrates missing keys first**, an O(bucket) build. For
  a By-Tab bucket this is bounded by the 576-item cap
  (microseconds keyed). There is no other hydration path: a
  sorted-but-keyless bucket stays keyless until one of these
  events actually needs its keys; nothing pre-builds
  speculatively.
- **Collapse evicts the bucket's keys; its order and sorted flag
  persist.** This is safe against later Price/Date staleness because
  invalidation acts on flags independently of key residency — the
  question R1-5 raised, now answered *within* the cached design.
  Re-expanding while the flag is valid does no work (D2 rule 2),
  leaving the bucket sorted-but-keyless; the next key-consuming
  event hydrates it by the rule above.
- **Residency is scoped to the active search (R2-4).** Deactivating
  a search evicts every one of its key vectors (orders and flags
  persist). **Reactivation decides dirtiness first (R3-1)**: a
  dirty search refilters once, and that rebuild supplies its keys —
  never a hydration of stale keys immediately before rebuilding
  them. A clean search's By-Tab buckets rehydrate lazily,
  per bucket, by the hydration rule; but **a clean By-Item search
  hydrates its flat bucket's keys eagerly at activation**, before
  any delta can need them. The eager carve-out is deliberate: the
  flat bucket is always visible and always sorted (D4), so its
  hydration is never speculative, and paying it at activation
  (~368 ms at 1m, budgeted below) attributes the unavoidable
  rebuild to the user action that made the view active — instead
  of letting the first background delta absorb it and blow the
  ≤ 50 ms merge budget. At most one search holds resident keys at
  any time — N background By-Item searches hold none, not ~250 MB
  each.
- Build cost is O(bucket) — measured 33 ms per 100k items, 368 ms
  per 1m, so a 576-item bucket keys in well under a millisecond.

**Invalidation contract.** An order — and every resident key
vector, By-Tab and By-Item alike (R3-2) — is a cache of
`(item content, active sort column, buyout state for Price/Date)`.
Invalidation therefore acts on sorted flags **and on whichever key
vectors are resident**; each cause states its key effect:

1. A delta that changes a bucket's items clears that bucket's sorted
   flag. Key effect: a *visible* (D2) bucket re-establishes order as
   part of the delta's application — hydrating its vector first if
   evicted (R3-1) — discarding the replaced source's entries and
   adding the arrivals' via the merge/sort, still
   O(delta + bucket); a collapsed bucket's keys stay absent — no
   build for a bucket nobody sees. By-Item: the merge itself
   maintains order and keys (D4).
2. Switching the active sort column discards every resident key
   vector and clears every sorted flag; materialized buckets rebuild
   and re-sort, collapsed ones simply stay flagged unsorted. The
   whole-collection worst case (By-Item, or everything expanded) is
   ~0.5 s at 1m, user-initiated, once per switch.
3. Flipping only the direction re-sorts materialized buckets **on
   their resident keys — hydrating an evicted vector first (R3-1),
   otherwise no rebuild (R2-3)** — and clears collapsed buckets'
   flags.
4. `BuyoutManager` mutations invalidate Price/Date-dependent order.
   The implementation must route every mutation path through a single
   choke point, and the inventory is exhaustive (R1-6): item
   set-and-clear, tab set-and-clear, **migration** (`MigrateItem`,
   `buyoutmanager.cpp:370` — it changes the lookup result; there is
   no tab-level migration, R2-6), and the scoped and final pricing
   passes. **Key effect (R3-2)**: with Price or Date active, the
   affected items' entries in every resident key vector — an
   expanded By-Tab bucket's as much as By-Item's — are rebuilt
   before the batch's reorder, so a re-sort never runs on stale
   resident keys; with any other column active, resident keys
   encode no buyout state and are untouched (cells still repaint —
   the batching rules below). A design-review criterion checks the
   enumeration.

**Buyout observability and batching (R1-6).** Five rules make the
invalidation observable and bounded:

- **User commands batch at command scope (R2-5)**: a single UI
  command can loop over every selected row and then trigger
  propagation (`OnBuyoutChange`, `ui/mainwindow.cpp:541-567` —
  `SetTab`/`Set` per selected row), so "immediate" means **one batch
  at command end** — with Price or Date active, one reorder of the
  affected visible scope per command, never per `Set`
  (`multiSelectionBuyoutEditReordersOnce`). Per-`Set` reordering is
  forbidden at every batch boundary, pass or command.
- **Pricing passes batch**: the scoped per-delta pass and the final
  passes accumulate invalidations and emit **one** batch at pass
  end when the pass is outermost (the nesting and snapshot rules
  below take over when it is not) — at most one reorder/model
  update per pass, never one per `Set`
  (`pricingPassYieldsSingleModelUpdate` pins it; the quadratic
  per-`Set` reorder of the flat bucket is the failure this rule
  exists to prevent).
- **Nested batches coalesce; only the outermost boundary emits
  (R3-3)**: the two rules above compose, because a command can
  contain a pass — `OnBuyoutChange` ends by calling
  `PropagateTabBuyouts` (`ui/mainwindow.cpp:578`), a pricing pass
  in its own right. A pass or command boundary reached while an
  enclosing batch is open emits nothing; its invalidations
  accumulate into the enclosing batch, which emits once at its own
  end. One user command is therefore one model update even when it
  triggers propagation.
- **The snapshot's pricing sequence is one batch (R3-4)**:
  `OnItemsRefreshed` runs `MigrateBuyouts` → `ApplyAutoTabBuyouts`
  → `ApplyAutoItemBuyouts` → `PropagateTabBuyouts` back-to-back
  (`itemsmanager.cpp:152-155`), and nothing observes UI state
  between them. The sequence **must** run inside one outer
  model-invalidation batch — a single model update per snapshot,
  never up to four, each a potential full By-Item reorder.
  Persistence writes (`BuyoutManager::Save`) are outside batching
  and unchanged; only model invalidation coalesces
  (`snapshotPricingSequenceEmitsOneModelBatch`).
- **Cell repaint is independent of the active sort column**:
  Price/Date *cells* render buyout state unconditionally
  (`PriceColumn::value` reads the manager), so affected visible rows
  get `dataChanged` on any buyout batch regardless of what column
  sorts the view; only *reordering* is gated on the active column
  (`priceCellsRepaintUnderAnySortColumn`).

**Memory.** Measured (spike section 4): ~286 B/item accounted for
the string-heavy worst case. Under the residency rules, resident key
memory is proportional to the **active search's materialized set**
and nothing else: the collapsed-default unfiltered view holds ≈ 0;
background searches hold exactly 0 (R2-4). The worst case is a
materialized whole-collection result — the By-Item flat bucket, or a
broad-filter fully-expanded By-Tab result (R2-3) — at ~266 MB naive
/ ~222 MB with the required `s2`/suffix sharing at 1m. Accepted and
budgeted (≤ 300 MB aggregate, acceptance criteria); the user paying
it is holding a million-item sorted view either way.

Reasons:

- The spike proved the sort comparator-bound and the comparator
  regex-bound (~3/4 of every call); keys retire the whole per-
  comparison toll: 40× on cached keys, 10× even rebuilding every
  refilter, and `std::sort` machinery is ~12–13 ns/compare.
- Keys are the only measured lever that helps every structure,
  including the By-Item flat bucket (12.9 s → ~0.77 s at 1m even
  rebuilding; D4 does better by merging).
- The cache contract fits the delta shape: deltas arrive per fetch
  source, buckets key per display tab, so invalidation is naturally
  bucket-scoped and O(delta).

### D2. Deferred sorting: only visible order is paid for (hold point, lever B)

A bucket's item order is established only when that order can be
seen. Each bucket carries a sorted-validity flag (per search, per
model), and the flag-and-order lifecycle is a complete state machine
(R1-5) — every transition is listed here; there are no others:

1. A **never-sorted** collapsed By-Tab bucket keeps arrival order.
   Unsorted is not unstable: the order is deterministic until a delta
   or refilter changes the bucket's contents, so persistent indexes
   and model invariants hold.
2. **Expanding** a bucket whose flag is invalid sorts it first
   (building its keys, which then stay resident — D1), then the
   expansion proceeds. Cost is bounded
   by the 576-item bucket cap: ~3 ms even with the *current*
   comparator, microseconds keyed — imperceptible against the expand
   paint itself. The same rule serves `RestoreViewExpansion`:
   restoring N expanded buckets sorts exactly those whose flags are
   invalid. Expanding a bucket whose flag is valid does no sort
   work — and builds no keys: the bucket is sorted-but-keyless
   until a key-consuming event hydrates it (D1's hydration rule,
   R3-1).
3. **Collapsing evicts the bucket's keys and nothing else** (R1-5,
   R2-3): the sorted order and the flag persist — collapse is a view
   event, not a model event. Arrival order is never reconstructed; a
   bucket that has sorted once simply stays sorted until
   invalidated.
4. **Invalidation** (content delta, column switch, direction flip,
   buyout batch — D1's contract) clears flags; whether re-sorting
   happens *now* is decided by visibility: visible buckets (expanded,
   or By-Item) re-establish order as part of the invalidating event's
   application; collapsed buckets defer to their next expansion.
5. **Filtered searches are default-expanded** (`search.h:86`), so
   every visible bucket of a filtered result sorts eagerly.
   *(Post-freeze amendment, July 31, 2026 — S4 implementation review
   round 2: "filtered" means any filter is ACTIVE, superseding
   R1-8's original "set by any single excluded item" refilter
   snapshot. The snapshot definition could be flipped by a single
   delta — the last excluded item disappearing, or a first rejected
   arrival — and a flip is a whole-view membership change (every
   empty bucket hides or shows, default expansion changes) that no
   bucket-scoped operation can express, so delta-native operation
   requires the stable definition; it flips only at filter edits,
   which are full refilters by construction (D6). The observable
   change is confined to the degenerate case of an active filter
   that excludes nothing: such a search now hides empty tabs and
   default-expands. Pinned by the round-2 clause of
   `filteredSearchDropsEmptiedBucket`.)* The
   honest worst case is a **broad filter** that matches nearly the
   whole collection while expanding every bucket (R1-8): the
   ceiling is the key build + sort of ~everything on top of the
   filter loop, ~0.9 s estimated at 1m, **and the resident key
   footprint approaches the full-collection measurement (~266 MB
   naive at 1m) while that result stays materialized (R2-3)** — the
   same ceiling By-Item already accepts (D1 memory rules). Accepted
   and budgeted in M1-M3 (≤ 1.2 s, ≤ 300 MB at 1m); the
   collapsed-default win applies to the unfiltered case only.
6. The By-Item flat bucket is always visible and therefore always
   sorted (D4); lever B deliberately does not apply to it.
7. Sort-column header clicks and direction flips re-sort
   **materialized** buckets only (D1 rules 2–3; flips reuse
   resident keys, hydrating evicted ones first — R3-1 — and
   switches rebuild) via the existing `layoutChanged` path;
   collapsed buckets' flags clear and their sort defers to
   expansion.

Reasons:

- Measured: sorting only the expanded set costs 15–63 ms independent
  of collection size, and the default unfiltered view starts fully
  collapsed — the whole-model sort at first paint disappears rather
  than being merely cheapened.
- The hold point weighed B's bounded marginal value post-A (~0.13 s
  of a ~0.55 s filter-bound refilter at 1m) against its bookkeeping
  and committed it anyway, buying the collection-size-independent
  first paint — and B's flag caching composes with D1's key cache:
  a valid flag skips the sort entirely, a resident key vector makes
  an unavoidable re-sort cheap (R2-3).
- The lazy machinery extends an existing pattern (`m_sorted` +
  sort-on-activation, `search.cpp:461-466`) one level down, from
  model to bucket, rather than inventing a new one.

### D3. Refresh-driven model changes are bucket-scoped operations; the reset is retired from the refresh path

The delta path stops resetting the model. When a delta intersects the
current search (the M2 D9 intersection test — item half inherited
as-is, metadata half added below, R2-6), it is applied as
fine-grained operations scoped to the affected bucket:

- **Content replacement** (`TabRefreshed`): a **source-scoped**
  replace within the display bucket (R1-1) — remove exactly the rows
  whose items were fetched from the delta's `FetchSourceKey`, insert
  the arriving items that pass the active filters; arrival-ordered
  if the bucket is collapsed. **If the bucket is visible, the
  arrivals are sorted among themselves and then merged into the
  retained rows' order (R2-2)** — removal as contiguous-run
  `removeRows` batches, insertion as the merge's contiguous
  `insertRows` runs, O(runs) model operations and O(bucket) work.
  Sorting the arrivals alone cannot establish the bucket's global
  order when sibling sources' rows interleave under the sort; the
  merge is what does. The source-scoped distinction matters because the display bucket keys on the stable
  `(type, id)` and *aggregates* fetch sources: a Map/Unique child
  shares its parent's bucket with its siblings, and M2 D2's accepted
  mixed-generation behavior requires a child's delta to leave
  sibling sources untouched
  (`childDeltaPreservesSiblingSourcesInParentBucket`). For an
  ordinary tab the source and the bucket coincide and this reduces
  to the whole-bucket replace. Item-level diffing *within* a source
  is still deliberately rejected: M2 D2/D3 define the delta as a
  whole-fetch-source replacement, so source-scoped replace is the
  delta's native grain.
- **Child reconciliation** (`ChildrenReconciled`): the erase becomes
  row removals scoped to the parent's bucket — already
  source-predicate-shaped (keys outside the expected set), matching
  the replacement's grain.
- **New tab discovered**: the bucket row is inserted at its display
  position (`begin/endInsertRows` at top level). **Deletion stays a
  snapshot-boundary effect** (M2 D6): an empty delta empties a
  bucket, it never removes one — removal happens at the final
  reconciliation below.
- **Metadata changes** (rename, move, color): the bucket row updates
  in place (`dataChanged`) and repositions with `beginMoveRows` when
  display ordering changes; expansion and selection follow the
  stable `(type, id)` key exactly as in M2 R6-3 — the machinery the
  parent plan carries forward as M3's bucket keying.
- **The intersection contract gains a metadata half (R1-4).** M2's
  item-based intersection deliberately had no metadata-only trigger
  (M2 D6/R7-2): an empty delta carrying a rename, move, color, or a
  newly discovered empty tab waited for the next refilter or the
  final snapshot — tolerable when application meant an expensive
  reset, wrong once application is a `dataChanged`. Rule: every
  delta's location anchor lands in the canonical inventory
  immediately (existing M2 machinery), and a delta whose stable key
  owns a visible bucket — or would create one in an unfiltered
  search (new empty tab) — applies its metadata **now**, item
  intersection notwithstanding. M2 R7-2's exception is explicitly
  renegotiated and retired: metadata-only deltas are no longer
  outside any freshness statement, and the "invisible until user
  action after terminal failure" caveat disappears
  *(Post-freeze amendment, July 31, 2026 — S5 review round 1,
  finding 4: the D9 intersection SETS — `m_visible_sources`,
  `m_visible_sources_by_tab`, and their query surface
  `HasVisibleSource`/`HasVisibleGhostUnder` — are deleted. They were
  M2 D9 gating machinery: their final consumer was the throttled
  fallback's intersection gate, retired with the seam in S5, which
  left them write-only state maintained per delta with no reader
  anywhere, tests included. The intersection CONTRACT is not
  relaxed — its semantics are executed by application itself:
  removal is source-scoped by each item's fetch key, arrivals are
  filter-tested, and the metadata half applies unconditionally
  (R1-4) — so "does this delta intersect?" is no longer a question
  any consumer asks ahead of applying.
  `deltaUpdatesVisibleIndexesIncrementally`'s source-index wording
  narrows to the indexes that retain consumers. This amendment is
  the third of the design-review criteria's three stated
  renegotiations of inherited M2 machinery.)*
  (`metadataDeltaAppliesWithoutItemIntersection`). Filtered searches
  still hide empty buckets (`search.cpp:277-286`) — and the delta
  path converges to that state: a bucket a delta empties leaves a
  filtered view as a top-level row removal and reappears when
  arrivals match again, with "filtered" following D2 rule 5's
  amended any-filter-active definition (post-freeze amendment,
  July 31, 2026; `filteredSearchDropsEmptiedBucket`).
- **Search-side indexes are maintained incrementally**: the delta
  updates `m_visible_by_id` and the bucket in O(delta); no
  whole-collection rebuild rides along. (The intersection sets this
  bullet originally also named are deleted — the July 31 amendment
  above.) The full rebuild remains the refilter's job (D6).

**The D9 throttle is retired for the current search.** Its reason to
exist — reset-plus-restore too expensive to pay per delta — is gone:
a bucket-scoped application is O(delta) work (filter the arriving
items with `MatchesActiveFilters`, keyed sort of ≤ 576 items, row
ops). Deltas apply as they arrive; the freshness bound M2 D9 stated
becomes trivially "immediate", and M2's hard constraint ("no
per-delta uncoalesced model reset") is honored by having no reset at
all.

**Dirty flags: renegotiated for the active search only (R1-7).** M2
D9 rule 1 marked *every* search items-dirty per delta, the current
one included, because the throttled reset was the only application
mechanism. Post-M3 the active search processes every delta — either
applying operations or correctly adjudicating "no visible change"
(fails intersection, matches no filter) — so a delta it has
processed leaves it **clean**; marking it dirty anyway would buy a
spurious full refilter on the next switch-away-and-back
(`appliedDeltasLeaveActiveSearchClean`). Fail-safe direction: any
delta whose application was *skipped* for any reason leaves the flag
dirty, and the final reconciliation clears it. Background searches
keep rule 1 and dirty-on-activation verbatim.

**The final snapshot stops being a destructive event for the open
window — but it is an authoritative row reconciliation, not just a
metadata pass (R1-2).** M2 D6 keeps three worker mutations
snapshot-boundary-only that no delta expresses: deleted tabs dropped
with their items, newly discovered unfetched tabs, and the location
rebase (`RebaseItemLocations`, success only). A metadata-and-moves
pass would therefore retain deleted content and omit new empty tabs
indefinitely. On `ItemsRefreshed` after a refresh, the active search
performs **one reconciliation pass diffing its model against the
post-snapshot published state per stable key**: buckets (and rows)
for deleted tabs removed, newly listed tabs inserted at their
display positions (unfiltered searches — filtered ones still hide
empty buckets), metadata refreshed against the rebased locations,
bucket order corrected via move ops. Row operations only — no
`beginResetModel`; O(collection) once per refresh is accepted
(`finalReconciliationRemovesDeletedTabs`,
`finalReconciliationInsertsNewlyListedEmptyTabs`). Background
searches keep the existing flag-and-refilter-on-activation path. A
full refresh with the window open is then, as the parent plan
requires, just N deltas plus one reconciliation — no special
destructive path left.

**Selection is an intent, not a row (R1-3).** Immediate application
creates a window the throttled world never had: an item moving
between tabs arrives as a removal delta and, later, an insertion
delta — and M2 R6-3's cross-tab pin
(`reselectionSurvivesCrossTabMove`) requires selection to survive
that. The contract: the selection *intent* is the stable item id,
held independently of row existence. During an active refresh
(first delta to the terminal outcome), removing the selected item's
row keeps the intent alive (the visual selection may lapse); any
later delta inserting an item with that id — any bucket, any view —
re-adopts the selection through the global identity index.
**Every `RefreshFinished` outcome closes the intent window (R2-1)**:
on success, the final reconciliation clears the intent if the id is
absent; on failure — which emits no final snapshot (M2 D4) — the
terminal event itself performs the same absence check against the
visible result, so a stale intent can never survive one refresh and
unexpectedly reselect an item in a later one. A user selection wins
at any time. Outside an active refresh, a removal clears selection
immediately, as today. Pinned by
`selectionIntentSurvivesCrossTabMoveAcrossDeltas` — the successor of
M2's pin under no-reset machinery — and
`selectionIntentClearsOnTerminalFailure`.
*(Post-freeze amendment, November 12, 2026 — #282: this extends
R1-3 and withdraws nothing. A delta that replaced the rows of a tab
holding several selected items left only the current item selected —
the intent pass cleared the view's selection and re-adopted the one id
— so the next price edit applied to one row. **The current item keeps
everything above** (its stable id is the intent for the refresh window,
a removal keeps it alive, any delta showing it again re-adopts it as
the current row, a user selection overwrites it, R2-1 closes the
window), with one change: lapsing and re-adoption no longer clear or
replace the rest of the selection, and the re-adopted row is selected
only if it was selected when it last left (whether the current row is
selected is recorded before every pass that can remove it — a delta,
the final snapshot, a refilter — so the record describes the latest
lapse; switching search tabs records it as selected). **Every other
selected row stays selected across a refresh while its item stays
where it is**: immediately
before a delta, the items selected in the bucket it touches are read
from the selection as (stable id, tab) pairs (every bucket for the final
snapshot); immediately after, one walk of that bucket selects again
those it still holds from the same tab. Nothing is kept for those rows
from one delta to the next, so **they do not follow a move**: an item
that leaves its tab (sold, or moved to another tab) loses its selection,
whichever tab's delta arrives first. The grain is the delta's: a
Map/Unique child source is its own delta, so an item moved between
child sources of one tab loses its selection too when the removal
arrives first. Cost: the delta reads no row
selected in another bucket and walks only its own bucket, but finding
that bucket's rows scans the selection's range list once (and each row
re-selected asks Qt once), so it grows with the number of selection
ranges elsewhere — QItemSelectionModel keeps nothing else. Recorded:
input that changes neither the selection nor the current row (a click
on empty space while the tree has focus and the current item is away)
cannot be told from no input, so the intent survives it; focus entering
the tree makes its first row current, which overwrites the intent.
Known limitation: every content delta removes and inserts its rows
(D3's source-scoped replace), so the restore calls select() without the
Current flag. A Shift+click, Shift+arrow or Ctrl+drag range in progress
when such a delta lands is frozen as it stood: the frozen range stays
selected, and the user's next Shift gesture replaces the rows the
restore had just re-selected instead of the range. The price editor
acts on the selection as it then is, which may include rows in a
collapsed or scrolled-away tab. Removing this needs an in-place replacement for a
delta that leaves a tab's items unchanged, an item-level diff within a
source that D3's "source-scoped replace is the delta's native grain"
rejects; it would need its own decision to amend D3. Pinned by
`multiSelectionSurvivesDeltaToItsTab`,
`multiSelectionAcrossTabsSurvivesDeltas`,
`multiSelectionKeepsSelectedHeaders`,
`byItemMultiSelectionSurvivesDelta`,
`currentItemLeavingKeepsOtherSelectedRows`,
`moverLosesSelectionWhicheverDeltaComesFirst`,
`currentMoverFollowsWithoutCollapsingSelection`,
`emptySpaceClickIsNotUndoneByALaterDelta`,
`userInputWhileCurrentItemIsAway`,
`readoptionUsesSelectionAtTheLatestLapse`,
`byItemMoverArrivingFirstLosesSelection`,
`deltaElsewhereLeavesUnselectedCurrentRowUnselected` and
`deltaWorkIgnoresSelectionInOtherTabs` (rows read and walked).)*

**Where resets remain legitimate**: initial population (nothing to
preserve) and user-initiated structural changes — filter edits, view-
mode switches, search switches — where the R6-3 capture/restore
machinery already covers fidelity (D6). Never on the refresh path;
`noModelResetDuringRefresh` pins this.

Reasons:

- The spike killed the alternative framing: begin/endResetModel are
  microseconds, so the reset was never the cost — but the *restore*
  obligations it creates (recapture, refilter, re-sort, reselect) are
  the whole cascade. Scoped ops make the untouched state simply never
  move, which is the milestone's success criterion.
- Per-delta cost is bounded and small (O(delta) + a ≤ 576-item keyed
  sort), so no coalescing layer is needed; deleting the throttle
  removes a whole class of staleness/starvation reasoning (M2 D9
  spent most of its length on it).
- Rejecting item-level diffs keeps the operation count per delta at
  O(runs) row-op batches — O(1) in the common single-source-tab
  case, the merge's run count for multi-source buckets (R2-2, the
  same bound D4 states for the flat bucket) — which is what keeps
  persistent-index handling tractable
  (`modelTesterPassesUnderDeltaStorm`).

### D4. The By-Item flat bucket: per-delta sorted merge

The By-Item view (single flat bucket holding the whole filtered
result) is the one structure that fights the delta shape, and lever B
cannot help it. It gets its own contract:

1. The flat bucket is always materialized: order maintained, and
   keys resident **whenever the search is active** — activation
   supplies them (a dirty search's refilter builds them as part of
   its sort; a clean search hydrates eagerly at activation,
   R3-1/D1), so no delta ever meets a keyless flat bucket and the
   merge budget in rule 2 holds unconditionally.
2. A delta applies as: remove the source's rows (erase by
   `FetchSourceKey`, contiguous-run `removeRows` batches), then merge
   the arriving filtered items — sort the ≤ 576 arrivals by key,
   then a single merge pass against the resident order, inserted as
   contiguous-run `insertRows` batches. Work is O(n + d) per delta
   (the vector shuffle dominates; at 1m that is tens of
   milliseconds), model ops are O(runs), and the result must equal a
   full keyed re-sort (`byItemMergeMatchesFullSort`).
3. The memory worst case is accepted and documented: ~222–266 MB
   resident keys at 1m on a base column (D1). The user paying it
   today pays 12.9 s per refilter for the same view; the trade is
   deliberate. The budget is pinned (≤ 300 MB at 1m, acceptance
   criteria).
4. A full By-Item refilter (user-initiated) is the keyed build + flat
   sort: measured ~0.77 s at 1m against 12.9 s today.

Reason: the parent plan names this structure explicitly ("the one
structure that fights the delta shape") and the spike quantified both
the threat (12.9 s, immune to lever B) and the remedy's ceiling. An
O(n) merge pass per delta is the honest cost of a single sorted
container; pretending a flat bucket can take O(d) updates would be
designing against arithmetic.

### D5. The tie-break becomes the intended `(name, uid, hash)` order (hold point; fixes F67)

`Item::operator<`'s third tuple element becomes `rhs.m_hash` (the
one-token F67 fix), restoring the intended hash-level determinism for
id-less items, and D1's key suffix carries the hash so keyed and
comparator order agree everywhere —
`keyedOrderMatchesComparatorOrder` is the equivalence pin and
`intendedTieBreakRestored` the determinism pin. The only visible
change is that items with no server id that tie on name now order
deterministically instead of arbitrarily. F67's register entry is
moved to the resolved ledger when this lands.

Reason: the hold point chose intended over actual order — determinism
for the price of one CoW QString per key (measured free while the
item lives), versus preserving an ordering accident nobody depends
on.

### D6. The user-initiated refilter: kept, and cheap by the same levers

Filter edits, view-mode switches, and search switches keep today's
shape — capture, `FilterItems`, reset, restore (R6-3 machinery
unchanged) — because a changed filter genuinely invalidates the whole
derived view. What changes is the price: the post-reset sort
disappears into D1/D2 (collapsed buckets sort nothing; restored
expansions sort their N buckets keyed; By-Item pays the keyed flat
sort). The refilter's residual cost is then the filter loop itself —
~34 ms at 100k, ~391 ms at 1m, now the dominant term. **Optimizing
the filter loop is explicitly out of scope** (D7) but the fact is
recorded: post-M3, "refilter cost" means "filter-loop cost".

Reason: the refresh path (D3) and the user-refilter path have
different invalidation semantics — a delta invalidates one bucket, a
filter edit invalidates everything — and forcing them through one
mechanism would either reintroduce whole-view work per delta or
incremental-filter complexity M3 does not need. The levers make the
honest full rebuild affordable.

### D7. Deferrals, each with its reason

- **Filter-loop optimization.** Post-M3 the dominant refilter term
  (~0.4 s at 1m). Deferred: it is orthogonal to the delta-native
  model and the sort levers, and it needs its own profile-first pass
  (per-filter attribution) before choosing a lever. Routed to the
  parent plan as a follow-up input, not silently dropped.
- **Born-sorted master buckets.** Never prototyped; its marginal win
  over cached keys is bounded by the numbers at ~0.13 s per 1m
  refilter, against pipeline-wide sortedness maintenance and a
  master-per-sort-order problem. Reopen only if D1's invalidation
  proves intractable in practice.
- **Regex micro-optimization as a lever.** Bounded strictly below A
  (the toString/PrettyName floor leaves ~1.5–2.5 s at 1m) and
  unprototyped. Permitted silently as an implementation detail
  *inside* the key build (D1 rule 2's 0.37 s column-switch rebuild is
  ~90% multivalue machinery), forbidden as a substitute for keys.
- **F66 legacy-store rekeying.** The register names M3 "the natural
  opportunity" if the stores are reworked — M3 reworks the model
  layer, not the legacy persisted stores, so the hook stays parked in
  the register. Nothing in this spec touches location-keyed
  persistence.
- **Key persistence across sessions.** Keys rebuild from live items
  in well under a second at any realistic scale; persisting a cache
  of derived strings buys nothing and adds an invalidation surface to
  the datastore. Rejected, not deferred.
- **`QueueUpdated` coalescing (M1-M2)** stays a post-M2 follow-up
  where M2 left it; nothing here changes its standing. **Discharged
  July 31, 2026, after M3's implementation:** the measurement ran,
  the gate fired (~23 ms of synchronous status-widget handler time
  per 2,000-entry burst), and the prescribed D10 coalesce was built
  and validated (`m1-m2-result.md`; resolution recorded at M2's
  D10 and open-items entry).

## Acceptance criteria

Model-level (Qt Test, against the `MainWindow` fixture and a direct
`Search`/`ItemsModel` harness):

- `unrelatedDeltaLeavesOtherBucketsUntouched` — the milestone's
  success criterion as a test: refresh one tab; every other bucket's
  expansion, selection, scroll anchor, and persistent indexes are
  bit-identical, and no restore machinery runs (probe: capture/
  restore entry counts stay zero).
- `noModelResetDuringRefresh` — a complete refresh (N deltas, child
  reconciliations, terminal, final snapshot) with the window open
  emits zero `begin`/`endResetModel` on the current search's model;
  the final snapshot performs only the D3 row reconciliation.
- `deltaReplacesExactlyItsSourceRows` — a content delta's row
  operations touch exactly the rows fetched from its
  `FetchSourceKey` within the affected bucket; row accounting,
  filtered membership, and (for a visible bucket) keyed order all
  correct after application (R1-1).
- `childDeltaPreservesSiblingSourcesInParentBucket` (R1-1, R2-2) —
  with an expanded Map/Unique parent bucket holding parent items
  plus two children's items whose sort keys interleave heavily, one
  child's delta replaces only that child's rows **and the resulting
  bucket order is globally sorted** (arrivals merged into the
  retained rows, not appended or sorted separately); the sibling's
  and parent's rows and their persistent indexes are untouched
  outside the merge's moves (M2 D2's mixed-generation behavior at
  the model layer).
- `finalReconciliationRemovesDeletedTabs` (R1-2) — a refresh whose
  list reconciliation deleted a tab: no delta removes its bucket;
  the final snapshot's reconciliation removes the bucket and its
  rows via row operations, and its items leave the visible indexes.
- `finalReconciliationInsertsNewlyListedEmptyTabs` (R1-2) — a newly
  discovered, unfetched (empty) tab appears as a bucket in an
  unfiltered search at the final reconciliation, at its display
  position; a filtered search continues to hide it.
- `metadataDeltaAppliesWithoutItemIntersection` (R1-4) — empty
  deltas carrying a rename, a move, a color change, and a
  new-empty-tab discovery each apply immediately (`dataChanged` /
  move / bucket insertion in an unfiltered search) with **no final
  snapshot and no refilter**, and the applied state persists after a
  terminal failure.
- `appliedDeltasLeaveActiveSearchClean` (R1-7) — after the active
  search applies (or correctly adjudicates) a series of deltas,
  switching away and back triggers no refilter; a delta arriving
  while application is impossible leaves the flag dirty, and the
  final reconciliation clears it.
- `emptyDeltaEmptiesBucketWithoutRemovingIt` — the M2 D6 boundary at
  the model layer: an empty replacement leaves an empty bucket row
  (unfiltered search), never a bucket removal.
- `deltaUpdatesVisibleIndexesIncrementally` — after a delta,
  `m_visible_by_id` answers as if freshly refiltered, with no
  whole-collection rebuild (probe: rebuild counters). (Originally
  also named the intersection sets and their query surface; narrowed
  by the July 31 amendment in D3, which deleted them.)
- `bucketRepositionsByMoveOnMetadataDelta` — a rename/move delta
  repositions the bucket via move operations; expansion and selection
  follow the stable `(type, id)` key (extends M2 R6-3's pins from
  reset-restore to move).
- `modelTesterPassesUnderDeltaStorm` — `QAbstractItemModelTester`
  attached through a randomized storm of deltas (content, empty,
  reconciliation, metadata, new-tab) interleaved with expansion
  changes, sort clicks, and view-mode switches.

Sort-correctness:

- `keyedOrderMatchesComparatorOrder` — for every column: the keyed
  sort of a mixed dataset (double path, range paths, string path,
  heavy ties, id-less items) is identical to a direct `Column::lt`
  sort with the D5-fixed comparator, both directions.
- `intendedTieBreakRestored` — id-less items tying on `PrettyName`
  order deterministically by hash across repeated sorts and merges
  (F67 resolved).
- `collapsedBucketsDeferSorting` — an unfiltered By-Tab refilter
  sorts no collapsed bucket and builds no keys for one (probe:
  key-build and compare counters at zero); expanding one bucket sorts
  exactly that bucket, correctly.
- `restoredExpansionSortsRestoredBucketsOnly` — a user refilter with
  N saved expansions sorts exactly those N buckets on restore.
- `filteredSearchSortsAllVisibleBuckets` — a filtered
  (default-expanded) search presents every bucket sorted.
- `sortedOrderSurvivesCollapse` (R1-5) — expand (sort), collapse,
  re-expand with no intervening invalidation: no key build and no
  sort runs the second time (probe counters), and the order is the
  sorted one; arrival order is never reconstructed.
- `collapsedInvalidBucketResortsOnReexpand` (R1-5) — expand,
  collapse, replace the bucket's contents by delta, re-expand: the
  bucket re-sorts exactly once, correctly.
- `keyResidencyFollowsMaterialization` (R2-3) — an expanded bucket's
  keys persist across re-sorts and direction flips (probe: no key
  rebuild on a flip); collapsing it evicts its keys (probe: live key
  bytes drop) while its order and flag persist.
- `residentKeysScopedToActiveSearch` (R2-4) — with several searches
  including multiple By-Item ones, aggregate resident key memory
  never exceeds one search's worth; deactivating a search evicts its
  keys, and no refilter runs when the search is not dirty.
  Reactivation rehydrates a clean By-Tab search lazily, per bucket
  (a dirty one refilters, the sort supplying its keys); a clean
  By-Item search hydrates eagerly at activation (the R3-1
  carve-out — the one deliberate exception to lazy).
- `reexpandedBucketFlipHydratesOnce` (R3-1) — expand (keys built),
  collapse (keys evicted), re-expand (valid flag: no sort, no key
  build): a direction flip then hydrates the bucket's keys exactly
  once and re-sorts correctly; a second flip rebuilds nothing
  (probe: key-build counter).
- `byItemActivationDecidesDirtinessFirst` (R3-1) — deactivate a
  **clean** By-Item search, reactivate: activation hydrates the
  flat bucket's keys with no refilter, and the first delta merges
  with a zero key-build count during application, inside the delta
  budget. Deactivate a **dirty** one, reactivate: exactly one
  refilter runs and its sort supplies the keys — no separate
  hydration before or after (probe: one key build total).
- `staleOrderNeverSurvivesDelta` — a delta replacing a visible
  bucket's items yields fresh keyed order as part of application;
  stale order never persists on a visible bucket (probe: sort
  counted for that bucket alone).
- `sortColumnSwitchResortsVisibleBucketsOnly` (R1-5, R2-3) —
  switching the active column discards resident keys, clears every
  sorted flag, and re-sorts materialized buckets only; a direction
  flip re-sorts materialized buckets **on their resident keys with
  no rebuild** (an evicted vector hydrates once — R3-1,
  `reexpandedBucketFlipHydratesOnce`); collapsed buckets sort at
  their next expansion.
- `priceKeysFollowBuyoutEdits` — with Price active, a user buyout
  edit (item and tab level, set and clear) reorders affected rows
  at command end — **in an expanded By-Tab bucket as well as
  By-Item (R3-2: the bucket's resident key entries rebuild before
  the reorder; no re-sort on stale keys)**. Migration
  (`MigrateItem`, R1-6/R2-6) reorders within the snapshot's outer
  batch, not at a command end — it runs from
  `ItemsManager::MigrateBuyouts` during snapshot processing
  (`itemsmanager.cpp:152`), where there is no user command and the
  required snapshot batch (R3-4) is always the containing batch.
  Date symmetric; with Name active the same edits reorder nothing.
- `multiSelectionBuyoutEditReordersOnce` (R2-5) — with Price active
  in By-Item, one buyout command over a many-row selection produces
  exactly one reorder / model update at command end (probe:
  model-update count), never one per selected row — **the command's
  trailing `PropagateTabBuyouts` call included: the propagation
  pass's batch nests inside the command batch and emits nothing of
  its own (R3-3)**.
- `pricingPassYieldsSingleModelUpdate` (R1-6) — a scoped or final
  pricing pass touching many items produces at most one reorder /
  model update per pass on the active search (probe: model-update
  count), never one per `Set`.
- `snapshotPricingSequenceEmitsOneModelBatch` (R3-4) — one final
  snapshot's `MigrateBuyouts` → `ApplyAutoTabBuyouts` →
  `ApplyAutoItemBuyouts` → `PropagateTabBuyouts` sequence produces
  at most one reorder / model update on the active search (probe:
  model-update count), never one per pass; buyout persistence
  writes still occur.
- `priceCellsRepaintUnderAnySortColumn` (R1-6) — with Name active, a
  buyout batch emits `dataChanged` for the affected visible
  Price/Date cells and performs no reordering.

By-Item:

- `byItemMergeMatchesFullSort` — after any sequence of deltas, the
  flat bucket's order equals a from-scratch keyed sort of the same
  filtered collection.
- `byItemRemovalOnlyDeltaErasesInPlace` — an empty replacement
  removes exactly the source's rows via row operations; no reset, no
  full re-sort.
- `byItemSelectionSurvivesMerge` — a selected item retains selection
  through a merge that moves its row; an item absent at the final
  reconciliation clears it, while mid-refresh absence retains the
  intent (R1-3).

Selection intent (R1-3):

- `selectionIntentSurvivesCrossTabMoveAcrossDeltas` — with an item
  selected, one delta removes it from its tab; several deltas later
  another inserts it in a different tab. The selection re-adopts the
  item by stable id through the global index (M2
  `reselectionSurvivesCrossTabMove`'s successor). If the refresh
  ends without the id reappearing, the final reconciliation clears
  the selection; a user selection made in between wins outright.
- `selectionIntentClearsOnTerminalFailure` (R2-1) — deltas remove
  the selected item, then the refresh fails terminally (no final
  snapshot): the intent is cleared at the terminal event, and a
  later refresh reinserting the same id does **not** reselect it.

Performance and memory (the M1-M3 checkpoint, Release, recorded
environment, spike presets; budgets are the spike's measured ceilings
with headroom, misses gate completion the way M2-M2's did):

- Worst-case unfiltered By-Tab refilter (user-initiated, default
  collapsed): ≤ 60 ms at 100k, ≤ 500 ms at 1m end-to-end
  (filter-loop-bound; the sort share ≤ 5 ms).
- Single-bucket expand (cold keys): ≤ 10 ms at both scales.
- Clean By-Item search reactivation (eager key hydration, R3-1):
  ≤ 100 ms at 100k, ≤ 0.5 s at 1m — the cold-reactivation
  boundary; the By-Item delta budget below then holds
  unconditionally, no By-Item delta ever paying a flat-bucket key
  build.
- By-Item full refilter: ≤ 250 ms at 100k, ≤ 1.5 s at 1m.
- Broad-filter default-expanded refilter (a filter matching ~all
  items, every bucket visible — R1-8's worst case): ≤ 150 ms at
  100k, ≤ 1.2 s at 1m.
- Delta application on the current search, By-Tab visible bucket:
  ≤ 5 ms at 1m; By-Item merge: ≤ 50 ms at 1m.
- Resident key memory, active column, **aggregate across all
  searches** (R2-4): ≤ 300 MB at 1m in the worst materialized shape
  (By-Item, or broad-filter fully-expanded By-Tab — R2-3);
  collapsed-default unfiltered view ≈ 0; background searches
  exactly 0.

Design-review criteria (checked in review, not runnable):

- No refresh-driven path reaches `beginResetModel`; resets exist only
  on initial population and user-initiated structural changes (D3's
  enumerated list).
- The delta path is O(delta + affected bucket) everywhere except
  D4's stated O(n + d) By-Item merge pass; no whole-collection scan
  or rebuild rides on any delta (M2 hard constraint, extended to the
  model layer). *(Post-freeze amendment, July 31, 2026 — the third,
  out of the S8 external review; registered in the status header.
  The implemented bound for an ordinary delta is
  **O(delta + affected bucket + log tab-count)**: bucket-row
  resolution goes through the ordered stable-key map
  (`std::map`, `search.h` `m_row_by_key`), the deliberate S4 review
  round 1 (finding 6) design that retired the per-delta linear scan
  — acceptance recorded in `m1-m3-result.md` §S4 review round 1.
  Structural delta ops — bucket insertion, removal, and metadata
  repositioning — additionally pay an O(tab-count) row-position
  remap, accepted in the same round. A buyout batch's By-Item cell
  repaint scans the flat bucket once, the D4-shaped exception
  accepted at S2 (D1 rule 4's repaint path, not the delta path).
  None of these terms is collection-of-items scale; the criterion's
  intent — no collection-scale work rides a delta — stands
  unchanged.)*
- Keys are derived from the comparators, which remain the single
  source of ordering truth; no code path defines order twice.
- D1 rule 4's `BuyoutManager` mutation enumeration is complete —
  every mutation path, **migration included** (R1-6), flows through
  the single choke point — and the batching rules hold: passes and
  commands emit one batch at their own end, nested boundaries emit
  only at the outermost (R3-3), the snapshot's pricing sequence
  emits one batch (R3-4), and cell repaint is independent of the
  active sort column.
- M2's intersection and background-search machinery is inherited
  with exactly three stated renegotiations, none silent: the
  metadata half added to the intersection contract (R1-4, retiring
  M2 R7-2's exception), rule 1 relaxed for the active search only
  (R1-7), and the intersection sets deleted once S5's throttle
  retirement removed their final consumer (the July 31, 2026
  post-freeze amendment in D3). The persistence and presentation lanes still share no
  payload types, and keys carry no wire or `poe::*` types.
- The retirement of the D9 throttle deletes its machinery and its
  pinned timer tests by renegotiation, not silent breakage — the M2
  pins that encode "throttled reset" behavior are explicitly
  superseded here, each mapped to its D3-era replacement in the
  implementation plan.

## Open items requiring spike or measurement (not argument)

- **S1-M3 (spike, pre-spec — RESOLVED before this draft):** the
  sort-profiling spike, run July 30, 2026 on `spike/m3-sort-profile`
  (never merged; off master `3549b214`). Proved the sort
  comparator-bound and the comparator regex-bound; quantified levers
  A (~10–40×), B (proportional to expanded set), and the By-Item
  worst case; measured the key-memory extension at the hold point
  (~286 B/item; ~266 MB naive at 1m). Evidence:
  `m3-sort-profile-result.md`. Consumed by D1–D6 and the hold-point
  record.
- **M1-M3 (measurement, implementation checkpoint — blocks M3
  completion) — RESOLVED July 31, 2026:** the performance/memory
  budget table above, run on the implemented model with the spike
  presets and a recorded environment, plus per-component attribution
  (filter loop, key build, sort, model ops, merge) so a miss names
  its component before a remedy is chosen — the M2-M2 discipline.
  Delta-application scenarios use fixed recorded reply shapes as
  M2-M2's did. Conditional hold-point rows ran per stage: S3 and S4
  passed; **S5's By-Item merge row MISSED ~28×** (per-run row
  operations on collection-sized vectors), paused the sequence, and
  Tom selected the A′ translate-and-notify remedy (one O(n+d)
  rebuild emitting the same O(runs) batches) plus the per-run
  repaint-rectangle fix — rerun passed every row. The formal
  complete-table S7 gate then **PASSED every budgeted row at both
  presets**, with both candidate worst memory shapes judged at
  process level (≤ 300 MB row: By-Item 223.7 MB, broad-filter
  fully-expanded By-Tab 210.0 MB at 1m). S7 review round 1 made the
  gate harness fail loudly and debounced the per-delta deferred
  column resize it surfaced. Full tables, attribution, environment,
  and the S5 miss/remedy record: `m1-m3-result.md` (beside this
  spec).

## Input traceability

| Input (parent plan M3 section, spike, register, hold point) | Consumed by |
|---|---|
| Bucket architecture matches the delta shape; tab delta → fine-grained ops or one-bucket replace | D3 (source-scoped replace within the display bucket, with sorted merge — R1-1/R2-2) |
| By-Item single flat bucket needs a sorted merge per delta | D4 |
| Pricing semantics settled by M2 D7; M3 inherits | Scope (out), staleness preamble |
| Success criterion: refreshing one tab leaves everything else untouched; full refresh = N deltas, no destructive path | D3, `unrelatedDeltaLeavesOtherBucketsUntouched`, `noModelResetDuringRefresh` |
| Inbox: reset cost is the re-sort; user refilter still pays at scale; levers to weigh (keys / expanded-only / born-sorted); profile before choosing | S1-M3 ran (profile obligation); D1, D2 (chosen levers), D7 (born-sorted deferred) |
| Inbox: R6-3 fidelity machinery negligible; carries forward as M3's bucket keying | D3 (metadata/move rule), D6 (restore path kept) |
| Spike 1: comparator regex-bound; any lever skipping `multivalue` per comparison captures the win; regex-only variant unprototyped | D1, D7 (regex micro-opt bounded and demoted) |
| Spike 2: levers compose (cost per comparison × number of comparisons), ceilings 10–40× and proportional-to-expanded | D1 + D2 committed together (hold point) |
| Spike 3: By-Item flat bucket worst by far, immune to lever B; must be answered | D4 |
| Spike 4: begin/end reset ~free; filter loop is the next term once sort stops dominating | D3 (reset retired as *path*, not cost), D6, D7 (filter loop routed as follow-up) |
| Spike section 4: key memory ~286 B/item, ~266 MB naive at 1m; uid/hash CoW free; s2 dedupe saves ~44 MB | D1 (memory rules), D4 rule 3, memory budget |
| F67: dead hash tie-break; keyed design must choose intended vs actual order | D5 (intended; hold point) |
| F66: M3 named as natural opportunity if stores reworked | D7 (explicitly not exercised; hook stays) |
| Hold point (Tom, July 30, 2026): levers A+B committed; intended order; memory measured pre-freeze | D1, D2, D5; spike section 4 |
| M2 hard constraint: no per-delta uncoalesced model reset; freshness bound | D3 (no reset at all; throttle retired, bound trivially met) |

Every named input is consumed. Review rounds 1–3 (R1-1…R1-8,
R2-1…R2-6, R3-1…R3-4, July 30, 2026, all eighteen accepted) are
incorporated throughout — verdicts and resolutions in
`items-pipeline-m3-reviews.md`, which also carries the revision
log. The rounds' structural theme: revision 1 specified the
single-event grain; round 1 forced the sequence grain (source vs.
display bucket, cross-delta selection state, snapshot-boundary
rows, pricing-pass batching); round 2 closed the boundaries round
1's own resolutions opened (intent lifetime at the failure
terminal, merge order in multi-source buckets, residency across
searches) and returned one of them — key residency — to the hold
point's settled cached-key choice (R2-3); round 3 reconciled that
restoration's consequences — residency as an explicit axis with a
single hydration rule (R3-1), invalidation over every resident
cache (R3-2), and the batch boundaries nested (R3-3) and pinned to
their true call sites (R3-4).
