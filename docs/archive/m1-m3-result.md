# M1-M3 measurement result (items-pipeline M3, S7 gate)

Status: **S7 GATE PASSED (July 31, 2026) — the complete budget table
ran authoritatively on the finished model and every budgeted row passes
at both presets.** The table and its attribution are in the "Budget
table (S7)" section at the end; conditional hold-point rows were
recorded per stage (S3-S5) as the sequence reached them. S7 review
round 1 (two findings, both remedied same day): the gate now fails
loudly (non-zero exit on any miss or unavailable binding measurement),
and the per-delta deferred column resize surfaced at the pause is now
a non-resetting debounce (pin `deltaResizeDebounceCoalescesBurst`,
three clauses after the queued-timeout follow-up). **Tom's formal go
for S8 recorded July 31, 2026.** S8 (wrap-up) remains.

## S0 — staleness-preamble anchor verification (July 30, 2026)

The M3 spec's staleness preamble was written against master `3549b214`
(the M2 merge). This verification ran against post-merge master
`a2ed4d96` (the spec merge, PR #186 — no model-layer or delta-path code
changed between the two), immediately before S0's first implementation
commit.

**Verdict: every load-bearing claim holds. One citation was wrong at
freeze; no code drifted.** The drift record:

- **`search.cpp:461-466` does not exist** — the file is 444 lines at
  both `3549b214` and `a2ed4d96` (451 after S0's own probe edits), so
  the range was invalid when the spec froze (the claim appears twice:
  the staleness preamble's "sort actually runs on activation" and D2's
  "existing pattern" reason). The *claim* is correct; its actual sites
  are:
  `Search::FilterItems` ends by invalidating the model's sorted flag
  (`m_model.SetSorted(false)`, `search.cpp:352`), and the sort runs on
  activation via `MainWindow::ModelViewRefresh`'s
  `setSortingEnabled(true)` (`ui/mainwindow.cpp:854`) → the header's
  sort-indicator path → `ItemsModel::sort` (`items_model.cpp:220`,
  lazy `m_sorted` gate at `:227`) → `Search::Sort` (`search.cpp:200`)
  → `Bucket::Sort` (`bucket.cpp:42`). `Search::SetViewMode`
  (`search.cpp:435-444`) is the other sort-on-activation site.

Every other anchor verified at (or within a line of) its citation:

| Preamble claim | Cited | Verified at `a2ed4d96` |
|---|---|---|
| `OnTabRefreshed` / `OnChildrenReconciled`: rule-1 dirty marking, intersection gate, `ScheduleThrottledRefilter()` | `ui/mainwindow.cpp:709-736` | `:708-737` |
| Non-resetting trailing throttle, S = 60 s, `m_delta_throttle` | `ui/mainwindow.cpp:756` | `ScheduleThrottledRefilter` `:755-765`; `DELTA_THROTTLE_INTERVAL_MS = 60 * 1000` `:71`, single-shot `:138` |
| `OnDeltaThrottleTimeout` → `ModelViewRefresh` (capture, refilter, reset, restore) | `ui/mainwindow.cpp:828` | `ModelViewRefresh` `:827` (timeout handler `:767`); capture `:842-844`, restore `:855/:876/:877` |
| `beginUpdate()` is literally `beginResetModel()` | `items_model.h:28` | exact (moved by S0's own probe edit, behavior identical) |
| One sort column/order per model; pinned default column 0 = Name, descending; lazy `m_sorted` | — | `items_model.cpp:21-23` |
| 26 columns | `search.cpp:33-59` | `:32-58`, 26 entries |
| `Column::lt`/`multivalue`, two static regexes, string fallback | `column.cpp:28-67` | `multivalue` `:27-57`, `lt` `:59-62` |
| `PriceColumn::lt` / `DateColumn::lt` read `BuyoutManager` per comparison | — | `:422-436` / `:453-458` |
| `Item::operator<` hash term dead (F67) | `item.cpp` | `:667-672`; `:671` compares `m_hash` on both sides |
| Search state: `m_bucket_by_tab`/`m_bucket_by_item`, `m_expanded_keys`, `m_visible_by_id`, `m_visible_sources`(`_by_tab`), scroll anchor | `search.h` | members present (`:155-160`, `ScrollAnchor` `:59`) |
| `defaultExpanded()` is `m_filtered \|\| ByItem` | `search.h:86` | exact |
| M2 signal surface: `TabRefreshed`, `ChildrenReconciled`, typed `RefreshFinished`, final `ItemsRefreshed`, `SourceKeyedItems` both sides | — | `itemsmanager.h:74-79/:98`, `itemsmanagerworker.h:115-151/:328` |
| Scoped per-delta pricing in `ItemsManager` (M2 D7) | — | `OnTabRefreshed` `itemsmanager.cpp:160` calls `ApplyScopedPricing` `:176` (defined `:202`) |
| Buyout mutation surface: `OnBuyoutChange` `SetTab`/`Set` per selected row, trailing `PropagateTabBuyouts`; `MigrateItem`; snapshot pricing sequence | `ui/mainwindow.cpp:541-567/:578`, `buyoutmanager.cpp:370`, `itemsmanager.cpp:152-155` | `:541-577` (`Set` `:567`) / `:578`; `:370` exact; `:152-155` exact |
| Per-tab caps bound a display bucket at 576 items (quad tab) | — | structural, not an enforced cap in model code: a display bucket aggregates one tab, and a quad stash is 24×24 = 576 slots (`QuadStash` handling `itemlocation.cpp:102-106`; the dataset generator encodes the same cap, `tests/spikedataset.h`) |
| Initial cached load: the parse thread publishes one snapshot (`ItemsRefreshed(..., true)`) | — | `StartParseThread` runs `ParseCachedItems` on a dedicated `QThread` (`itemsmanagerworker.cpp:119-140/:142`); completion emits `ItemsRefreshed(..., true)` `:274`; `ItemsManager` re-emits `itemsmanager.cpp:125/:157` |

## S0 — probe surface and datasets

- **Probes** (`src/modelprobes.h`): comparator-call (all three `lt`
  implementations), bucket-sort (attributed by the stable `(type, id)`
  display key), model-reset (attributed per model), refilter,
  index-rebuild, and capture/restore entry counters, wired at their
  production sites; key-build, keyed-compare, model-update (batch),
  and live-resident-key-bytes fields declared with their landing
  stages (S1/S2/S3) so pins assert them from zero. **Disabled by
  default**: production takes one predicted branch per site and
  accumulates no state, and measurement windows stay unperturbed
  unless a row needs attribution; tests enable explicitly.
  `reset()` clears counters but preserves the `live_key_bytes` gauge
  (it tracks live ownership; zeroing it mid-residency would go
  negative at the next eviction). Nothing logs, nothing in production
  reads them. Pinned by `probeCountersTrackRefilterAndSort`
  (tst_search) and `probeCountersTrackCaptureRestore`
  (tst_mainwindow).
- **Datasets** (`tests/spikedataset.h`): the recorded M2-M2/spike
  shapes are now named presets — `smoke` (the S1-M2 harness's
  recorded 50 tabs / mean 20, ~1k items, functional runs), `100k`
  (2000 tabs / mean 50 / quad 0.10, ~101k published), `1m`
  (2600 / 400 / 0.80, ~976k published), all seed 20260729. Pinned by
  `namedPresetsMatchRecordedShapes`; `m2m2_benchmark` consumes the
  named presets. The M2-M2 caveat carries: reruns against numbers
  recorded before the July 30 10-char stash-id change are not
  byte-identical in dataset.

## S3 — conditional hold-point rows (July 30, 2026): **PASS, all rows**

Run at the end of S3 (D2 flags + D1 residency landed; branch
`items-pipeline-m3`) with the new `m3_holdpoint_benchmark` harness
(`tests/m3_holdpoint_benchmark.cpp`, Release, offscreen, spike presets,
seed 20260729). Environment matches the M2-M2 record: Apple M4, 32 GB
(macMini-class), macOS 26.6, Apple clang 21, Qt 6.11.1 release. The
sequence continues to S4 — no remedy decision needed.

| Row | 100k | 1m | Budget (100k / 1m) | Verdict |
|---|---|---|---|---|
| Worst-case unfiltered By-Tab refilter, default collapsed (median of 5, end-to-end `OnSearchFormChange`) | 39.1 ms | 418.3 ms | ≤ 60 ms / ≤ 500 ms | **PASS** |
| — sort share of the above | 0 ms | 0 ms | ≤ 5 ms | **PASS** (probe-attributed: 0 bucket sorts, 0 key builds, 0 keyed compares — the toll disappears, not merely cheapens) |
| Single-bucket expand, cold keys (576-item quad, median of 5) | 0.55 ms | 0.59 ms | ≤ 10 ms | **PASS** |
| Broad-filter default-expanded refilter (ilvl ≥ 2, ~99% match, every bucket visible — R1-8) | 94.9 ms | 980.8 ms | ≤ 150 ms / ≤ 1.2 s | **PASS** |
| Collapsed-default resident key memory | 0 B | 0 B | ≈ 0 | **PASS** (gauge exactly 0) |
| Background-search resident key memory after deactivation | 0 B | 0 B | exactly 0 | **PASS** (gauge exactly 0; was > 0 while active) |

Per-component attribution (M2-M2 discipline, micros on identical data
outside the timed windows):

- Unfiltered refilter is filter-loop-bound as designed: bare
  `FilterItems` micro 35.5 ms @100k / 464.8 ms @1m of the 39.1 / 418.3
  end-to-end; the remainder is the model reset + restore machinery.
- Broad refilter @1m: 471.4 ms filter loop + the eager key build/sort
  of ~everything (982.0 ms measured as one flat-bucket build+sort micro,
  an upper bound on the per-bucket work, which the live window's
  980.8 ms total confirms is heavily amortized across 2600 small
  sorts — 2600 key builds, 9.7M keyed compares, zero comparator calls).
- Broad-filter resident key footprint by the gauge: 37.0 MB @100k,
  356.7 MB @1m. The gauge deliberately over-estimates (fixed 24-byte
  header per string; the `uid`/`hash` CoW copies counted although they
  cost ~nothing while their items live), so this is not comparable to
  the spec's ~222-266 MB process-level figure; the authoritative
  ≤ 300 MB aggregate row is measured at process level when By-Item
  lands (S5) and at S7.

**S3 review round 1 rerun (July 30, 2026).** The round's fixes changed
`Bucket::Sort`'s mechanics (the permutation now applies in place —
sorting no longer transiently duplicates the ~144 B/item key vector,
which at a one-million-item By-Item sort would have added ~144 MB of
untracked peak on top of the resident estimate) and scoped the buyout
batch's layout operation to the affected materialized set. All rows
rerun and PASS, slightly faster: unfiltered refilter 35.3 / 374.3 ms,
sort share 0, cold expand 0.53 / 0.55 ms, broad-filter 85.1 / 912.6 ms
(100k / 1m), memory rows exactly 0.

## S4 — conditional hold-point row (July 31, 2026): **PASS**

Run at the end of S4 (D3 bucket-scoped delta operations + selection
intent landed; By-Tab throttle retired) with the same harness, build,
environment, and presets as the S3 rows. The sequence continues to S5 —
no remedy decision needed.

| Row | 100k | 1m | Budget (1m) | Verdict |
|---|---|---|---|---|
| Delta application on the current search, By-Tab visible bucket (576-item full source replacement into the expanded quad bucket — removal runs plus a maximal merge; median of 5, end-to-end `OnTabRefreshed`) | 0.344 ms | 0.352 ms | ≤ 5 ms | **PASS** (100k informational — the spec states the budget at 1m) |

Attribution (probe-attributed, same run): exactly one bucket sort — the
R2-2 merge itself, counted as the bucket's order-refresh event — zero
key builds (the visible bucket's resident vector is reused; arrival
keys are built per item inside the merge), ~5.9k keyed compares, zero
index rebuilds, zero refilters, zero model resets. The cost is flat
across presets, as D3 predicts: the operation is O(delta + affected
bucket) and never sees collection scale.

The S3 rows were incidentally rerun on the S4 code and all still pass
(unfiltered refilter 32.1 / 361.0 ms, sort share 0, cold expand
0.52 / 0.56 ms, broad-filter 83.3 / 878.8 ms, memory rows exactly 0).

**S4 review round 1 correction and rerun (July 31, 2026).** The
review found the row above did not measure the shape it claimed: the
harness replaced an ordinary single-source tab's entire source, so the
retained vector was empty — one contiguous removal run and no merge
against retained sibling-source rows. That run validated arrival
sorting and simple replacement, not the R2-2 interleaved path. The
harness now aggregates a synthetic child fetch source (a same-sized
donor tab's items, fresh ids, re-homed under the largest quad tab)
into the expanded bucket and replaces the child per delta: removal
runs scatter through 576 retained parent rows and the merge's 576
arrivals interleave against them. Rerun after the round's fixes,
same build and environment:

| Row | 100k | 1m | Budget (1m) | Verdict |
|---|---|---|---|---|
| Delta application, By-Tab visible bucket — **R2-2 interleaved child-source replacement** (576 retained + 576 arrivals, median of 5) | 0.887 ms | 1.079 ms | ≤ 5 ms | **PASS** |
| — single-source full replacement (the previously recorded shape, kept informational) | 0.380 ms | 0.459 ms | — | informational |

The interleaved shape costs ~2.3× the simple replacement — the run
count is what scales, exactly why the review flagged the original row
— and stays flat across presets (O(delta + bucket), never collection
scale). Attribution: one bucket sort (the merge), ~11.5k keyed
compares, zero key builds (resident vector reused), zero index
rebuilds, zero refilters, zero resets. The S3 rows rerun with the
round's fixes and still pass (unfiltered refilter 36.0 / 374.6 ms,
sort share 0, cold expand 0.53 / 0.59 ms, broad-filter
87.1 / 917.9 ms, memory rows exactly 0).

*(Addendum, July 31, 2026, out of the S8 external review: this
round's finding 6 — recorded at the time only in commit
`7ddd08db`'s message and the code comment at `search.h`
`m_row_by_key`, now recorded here so the spec's third post-freeze
amendment has a durable citation — added the ordered stable-key map
beside the serial map, retiring the per-delta linear bucket scan.
The accepted consequences: an ordinary delta's bucket-row lookup is
O(log tab-count), and structural ops (bucket insert/remove/move)
pay an O(tab-count) row-position remap, which any row-position
scheme would.)*

## S5 — conditional hold-point rows (July 31, 2026): **MISS — sequence PAUSED** *(resolved the same day — the remedy subsection at the end of this section records the final PASS)*

Run at the end of S5 (D4 By-Item merge + eager activation landed; D9
throttle fully retired) with the same harness, build, environment, and
presets as the S3/S4 rows. Three of four budgeted rows pass; the
By-Item merge row misses by ~28× and, per working rule 3, **the
sequence is paused here** pending a remedy decision.

| Row | 100k | 1m | Budget (100k / 1m) | Verdict |
|---|---|---|---|---|
| By-Item full refilter (median of 3, end-to-end `OnSearchFormChange`) | 116.7 ms | 1471.5 ms | ≤ 250 ms / ≤ 1.5 s | **PASS** |
| Clean By-Item reactivation — eager key hydration, R3-1 (end-to-end tab switch; probe-attributed: 0 refilters, 1 key build, 0 resets) | 46.3 ms | 446.6 ms | ≤ 100 ms / ≤ 0.5 s | **PASS** |
| Worst-shape resident key memory (process footprint delta across entering By-Item; gauge 35.7 MB / 344.2 MB is the known over-estimate) | 21.8 MB | 223.9 MB | ≤ 300 MB aggregate at 1m | **PASS** |
| **By-Item merge** (D4 rule 2: interleaved child-source replacement, 576 retained-source rows scattered through the resident order + 576 arrivals, median of 5) | 168.0 ms | **1397.9 ms** | ≤ 50 ms at 1m | **MISS (~28×)** |

Mode switch into By-Item (flat rebuild + keyed flat sort, D6 boundary,
informational): 81.0 ms / 1059.9 ms.

**Attribution (M2-M2 discipline).** Probes on the live merge: exactly
one bucket sort (the merge itself), zero key builds (the eager-hydrated
resident vector is reused), ~17.7k keyed compares, zero index rebuilds,
zero refilters, zero resets — all of which is microseconds. The micro
on identical data settles where the time goes: the same flat delta
against a bare `Search` with **no view attached costs 1395.5 ms of the
1397.9 ms live row**. The cost is the bucket-vector shuffle itself, not
view-side batch handling:

- The child source's ~576 rows scatter through the collection-sized
  sorted order, so the erase is ~576 contiguous runs, and the merge's
  arrivals land as ~360 more insert runs.
- Qt's `begin/endRemoveRows` contract requires the model to be
  consistent after **every** run, so each run's `vector::erase`/
  `insert` must complete before the next begins — and each one moves
  the tail of a ~1m-element vector, twice over (the item vector and
  the index-aligned resident key vector, whose `ItemSortKey` elements
  are string-heavy composites). Total work is O(runs · n), ~10⁸-scale
  element moves per delta at 1m.

*(The analysis and remedy options below are superseded in part by the
S5 review round 1 subsection that follows — the "mutually
unsatisfiable" claim was too strong, and remedy A as stated violates
Qt's model contract.)*

**The two halves of D4 rule 2 are mutually unsatisfiable at collection
scale.** The rule's mechanism ("contiguous-run `removeRows` batches …
inserted as contiguous-run `insertRows` batches") forces O(runs · n);
the rule's complexity claim and budget ("a single merge pass … O(n + d)
per delta … at 1m that is tens of milliseconds", ≤ 50 ms) describe a
single compaction+merge pass, which Qt permits only under **one** model
operation per delta (the bucket-scoped layout-change protocol
`ApplySort` already uses, with persistent-index remapping) — not under
per-run row-op batches. R2-2 corrected the batch count from O(1) to
O(runs) without anyone noticing that per-run consistency multiplies the
run count into the vector length. The S4 By-Tab row never sees this
because its buckets are capped at 576 rows; the flat bucket is the one
structure where runs and n are both large.

Remedy options (Tom decides; nothing is built on top of the miss):

- **A (recommended): post-freeze amendment to D4 rule 2's mechanism.**
  The flat bucket applies a delta under a single bucket-scoped layout
  operation — snapshot persistent indexes, one O(n + d) erase+merge
  pass over the item and key vectors, remap (removed rows map to
  invalid), `layoutChanged` scoped to the flat bucket. Model ops become
  O(1) per delta (stronger than the stated O(runs)); the complexity
  claim, the ≤ 50 ms budget, selection/intent behavior, and
  `byItemMergeMatchesFullSort` are unchanged.
  `byItemRemovalOnlyDeltaErasesInPlace`'s row-op signal assertions
  would be renegotiated to layout-op assertions (same observable
  contract: no reset, no full re-sort, order preserved).
- **B: keep the row-op mechanism and renegotiate the budget** to the
  measured ~1.4 s per delta at 1m — rejected on its face by D3's
  immediacy rationale (a delta arrives every ~20 s per tab; 1.4 s of
  synchronous UI work per delta makes By-Item unusable during a
  refresh at scale).
- **C: hybrid** — row ops below a flat-bucket size threshold, layout op
  above it. Adds a second code path and still needs A's amendment; only
  worth it if the row-op signals carry value at small scale that the
  layout op lacks.

The S3/S4 rows were rerun on the S5 code and all still pass (unfiltered
refilter 33.3 / 426.1 ms, sort share 0, cold expand 0.52 / 0.59 ms,
broad-filter 88.7 / 981.7 ms, S4 interleaved delta 0.85 / 0.92 ms,
memory rows exactly 0).

**S5 review round 1 (July 31, 2026 — Tom's review of the pause
record, four findings, all verified and accepted).** The findings and
what each changed:

1. *S5 misses its performance contract* — accepted; this is the
   recorded miss and the pause above. No change.
2. *Remedy A is invalid* — accepted, verified against the Qt 6.11.1
   docs: `VerticalSortHint` "carr[ies] the meaning that items are …
   not filtered out or in", and rows appearing or disappearing outside
   `begin/endInsertRows`/`begin/endRemoveRows` is outside the model
   contract; a pure layout operation covers equal-cardinality
   permutation only. The recommendation above is withdrawn (marker
   added). The round also corrects this section's **"mutually
   unsatisfiable" claim, which was too strong**: Qt's contract binds
   the model's *observable answers* at each notification boundary, not
   its physical representation — the O(runs · n) cost came from this
   implementation equating per-run notification with per-run
   `vector::erase`/`insert`. A mutate-once/notify-per-run
   implementation (option A′ below) satisfies D4's stated mechanism
   *and* its complexity claim; the spec is not internally
   inconsistent, the implementation was naive.
3. *The benchmark omits production-path pricing* — accepted; the
   harness gained manager-path rows (`ItemsManager::OnTabRefreshed`
   end to end: source-keyed replacement, inventory ingest, scoped
   pricing, then the window's merge), with priced arrivals carrying a
   fresh `~b/o N chaos` note per rep so every rep's 576 `Set`s are
   real state changes. Measured (same build/environment):

   | Shape | 100k | 1m |
   |---|---|---|
   | Manager path, unpriced arrivals, Name active | 167.1 ms | 1414.9 ms |
   | — same, with the view's row list laid out first | 167.3 ms | 1410.6 ms |
   | Manager path, priced arrivals, Name active | 4634.7 ms | **43,982.3 ms** |
   | Manager path, priced arrivals, Price active | 98.1 ms | 532.5 ms |

   Attribution: the unpriced manager path costs the same as the
   window-direct row (the pricing pass no-ops when no buyout state
   changes), so the recorded 1397.9 ms was the honest number for
   unpriced deltas — but a **lower bound** overall, exactly as the
   finding said. The priced-Name blowup is NOT the pricing writes
   (576 sqlite upserts are milliseconds) and NOT the merge: it is the
   **rule-5 repaint's single spanning `dataChanged` rectangle**.
   `RepaintBuyoutCells` emits one first-to-last rectangle per bucket;
   in By-Item under a non-buyout sort the affected ids scatter, so the
   span approaches the whole flat bucket, and Qt's per-row handling
   costs ~44 µs/row — a constant that reproduces across all three
   scales (smoke ~53 ms/1.2k rows, 100k ~4.4 s, 1m ~43 s). A new
   sub-finding, S2-era in origin: the one-rectangle strategy is
   O(span), invisible in By-Tab where buckets cap at 576 rows.
   Priced-Price at 532 ms is the R3-2 batch re-sort (~2.0M keyed
   compares) plus a *narrow* repaint band and a nearly run-free merge
   (same-price arrivals cluster under the Price order).
4. *Retired throttle leaves source-index machinery unconsumed* —
   accepted, and stronger than stated: `HasVisibleSource` and
   `HasVisibleGhostUnder` now have **zero callers anywhere**, tests
   included; `m_visible_sources`/`m_visible_sources_by_tab` are
   write-only production state maintained per delta. S6's
   reconciliation does not need them as specced. Disposition is Tom's
   call with the remedy: delete under an explicit amendment (the
   acceptance text's D9-intersection references are M2 history), or
   carry to S8's design-review pass.

**Corrected remedy options** (superseding the list above; Tom
decides):

- **A′ — mutate once, notify per run (translation shim).** Compute
  the final item and key vectors in one O(n + d) pass; emit the same
  contiguous-run `removeRows`/`insertRows` batches D4 rule 2 states,
  answering row queries during the notification window through a
  prefix-offset translation over the old/final vectors, and swap the
  final vectors in after the last batch. Keeps the frozen mechanism
  verbatim — no amendment — with standard signals
  (`modelTesterPassesUnderDeltaStorm`-safe). Work
  O(n + d + runs·log runs). Caveat: the batch count stays O(runs), so
  any per-batch view-side cost survives; measured nil offscreen, and
  the "laid out" probe row above was inconclusive because the harness
  window is never shown — if chosen, an on-screen spot-check is part
  of the remedy's definition of done.
- **A″ — cardinality adjustment + layout remap** (the review's
  sketch): one tail `insertRows`/`removeRows` to establish the new
  row count, then a bucket-scoped layout operation remapping
  persistent indexes. O(1) model operations per delta. Needs a
  post-freeze amendment to D4 rule 2's mechanism, and it is
  contract-gray: the layout step still replaces content in and out,
  which the sort hints explicitly exclude and `NoLayoutChangeHint`
  leaves undocumented; `QAbstractItemModelTester` under the S6 storm
  would adjudicate it in practice.
- **B — renegotiate the budget** to the measured cost: unchanged from
  above, rejected on its face by D3's immediacy rationale.
- **Regardless of the merge remedy, the repaint span needs its own
  fix**: rule 5 mandates that affected cells repaint, not that one
  rectangle cover them — emitting one rectangle per affected
  contiguous run (O(runs), like the row ops) removes the O(span)
  term. Without it, even a 50 ms merge leaves a priced delta at ~43 s
  at 1m under a non-buyout sort. Implementation-level, no amendment
  required; folded into the remedy stage if approved.

**S5 remedy (July 31, 2026): implemented, all rows PASS — the pause
is lifted and the sequence resumes at S6.** Tom's decision on the
round-1 options: **A′** for the merge (the mechanism stays; A″ remains
a fallback experiment only if on-screen signal handling had missed),
the repaint-run fix approved, and the dead source indexes deleted now
under an explicit amendment. Three commits, in the ordered sequence
Tom prescribed:

1. `594b2b1a` — the spec's second post-freeze amendment: the D9
   intersection sets (`m_visible_sources`, `m_visible_sources_by_tab`,
   `HasVisibleSource`, `HasVisibleGhostUnder`) deleted; their final
   consumer disappeared with the throttle, and the intersection
   contract's semantics are executed by application itself.
   `deltaUpdatesVisibleIndexesIncrementally` narrowed.
2. `b6442903` — rule-5 repaint scoped to affected runs: O(affected
   runs) rectangles, By-Item tab-level changes resolve to the tab's
   rows by item location, `everything` keeps the single full
   rectangle. New pin `buyoutRepaintScopesToAffectedRuns`.
3. `a737ad3a` — **A′**: `Bucket::ReplaceSourceRows` — one O(n + d)
   rebuild (removal-run scan; arrival sort + insertion runs against
   the compacted retained keys; removals notified back-to-front on
   the intact old vector, the final vector then built by MOVING
   retained rows, insertions notified forward on it; final keys
   realized in place by a backward run-replay merge — no second
   collection-sized key buffer). D4 rule 2's stated row-op batches
   are emitted unchanged; a row translation keeps the model's
   observable answers consistent at every notification boundary.
   Gate pin `byItemReplaceSatisfiesModelTester` (shrink / equal /
   grow / empty / removal-only under `QAbstractItemModelTester`,
   selection + persistent index surviving, end state equal to a
   from-scratch refilter).

Rerun of the full S5 hold point (same build discipline, environment,
presets):

| Row | 100k | 1m | Budget (1m) | Verdict |
|---|---|---|---|---|
| **By-Item merge** (child-source replacement) | 4.0 ms | **32.4 ms** | ≤ 50 ms | **PASS** (was 168.0 / 1397.9 ms — 43× at 1m) |
| — window shown and laid out (A′ view-overhead gate) | 4.2 ms | 32.2 ms | ≤ 50 ms | **PASS** (fully on-screen variant, `QT_QPA_PLATFORM=cocoa` on the live desktop: 34.8 ms — PASS; per-batch view overhead is ~2.5 ms over the bare rebuild) |
| — bare micro, no view attached | 4.4 ms | 32.9 ms | — | attribution: the cost IS the single-pass rebuild |
| Manager path, unpriced arrivals, Name | 5.1 ms | 33.0 ms | — | informational (pricing pass no-ops) |
| Manager path, priced arrivals, Name | 74.7 ms | 310.4 ms | — | informational (was 4634.7 / 43,982.3 ms); residual = 576 buyout upserts + the O(n) affected-row scan + per-run rectangles |
| Manager path, priced arrivals, Price | 103.3 ms | 545.0 ms | — | informational; dominated by the **mandated R3-2 batch re-sort** (~2.0M keyed compares) plus a near-run-free merge — NOT closed by the repaint fix, recorded separately |

Peak-memory gate (Tom's A′ definition of done): lifetime peak
(`ru_maxrss`) across the shown-window merge reps +3.9 MB at 100k (the
final-vector scale) and **+0.0 MB at 1m** — no key-vector-sized
transient; the resident-key gauge and process-footprint rows are
unchanged from the round-1 record. Attribution probes unchanged in
shape: one bucket sort (the merge), zero key builds, zero index
rebuilds, zero refilters, zero resets. All S3/S4/S5 budgeted rows
rerun and PASS (unfiltered refilter 25.3 / 248.6 ms, broad-filter
79.1 / 754.1 ms, S4 interleaved delta 0.88 / 0.86 ms, By-Item
refilter 107.4 / 1267.0 ms, reactivation 44.8 / 455.2 ms — the
across-the-board improvement over the round-1 numbers is partly the
repaint fix and partly run-to-run environment variance; the budget
verdicts are what is load-bearing).

## S6 — final reconciliation; last reset seam deleted (July 31, 2026)

No hold point (per the sequence); this entry records what landed and
the renegotiations reviewers must see.

**Landed.** `Search::ReconcileFinalSnapshot` — the R1-2 authoritative
row reconciliation on `ItemsRefreshed`: one O(collection) filter pass
deciding target membership, then deleted buckets/rows removed (one
top-level removal takes the subtree), metadata refreshed against the
rebased locations, bucket order corrected by a selection pass of move
ops (zero moves on a clean refresh), newly listed tabs inserted at
their display positions (unfiltered searches), and a row-level diff
by item identity (the worker publishes the Item objects the deltas
delivered — worker comment at `FinishUpdate` — so a clean search's
surviving buckets diff to nothing, and a skipped delta's stale
content is authoritatively replaced, licensing the R1-7 dirty-flag
clear). By-Item runs the same diff as one A′ flat replace and marks
the By-Tab side stale. The final-snapshot reset seam in
`MainWindow::OnItemsRefreshed` is deleted: resets now exist only on
D6's enumerated user-initiated paths and initial population
(`initial_refresh` now reaches the slot). Background searches keep
rule 1 at the snapshot boundary — flagged items-dirty, refiltered by
their own next activation, never eagerly.

**Pins closed.** `noModelResetDuringRefresh` (complete refresh: zero
resets window-wide, zero refilters, exactly one reconciliation),
`finalReconciliationRemovesDeletedTabs`,
`finalReconciliationInsertsNewlyListedEmptyTabs`,
`modelTesterPassesUnderDeltaStorm` (240 seeded operations over the
full input set — content/empty/metadata deltas, child
reconciliations, new-tab discoveries, final reconciliations with
snapshot-only mutations — interleaved with expansion changes, sort
clicks, and view-mode switches; probe floors prove the boundary was
exercised and that the storm never fell back to a refilter; end-state
membership equals a from-scratch refilter).

**Fallback-insensitive revalidation (review round 1's tightened S6
verification).** The final-reconciliation clauses of
`selectionIntentSurvivesCrossTabMoveAcrossDeltas` and
`byItemSelectionSurvivesMerge` now assert, at the snapshot: reset
spy zero, `final_reconciliations == 1`, `refilters == 0` — proving
the reconciliation, not a reset, performed the intent clearing.
`appliedDeltasLeaveActiveSearchClean` gained its post-snapshot
no-refilter tail; its R1-7 clause (skipped delta leaves dirty, the
reconciliation clears it) has no reachable skip path in the
MainWindow fixture since S5, so it closed on the direct Search
harness as `reconciliationDischargesFailSafeDirtiness`.

**Renegotiated tests (outside the M2-pin supersession map, which
covers D9-era pins only; recorded here loudly instead).**

- `itemsRefreshRefiltersBackgroundSearches` (pre-M2, F33) encoded the
  seam's eager background refilter at the snapshot. Renamed
  `itemsRefreshDefersBackgroundSearchesToActivation`: the background
  caption stays last-rendered at the snapshot and activation
  refilters — F33's freshness guarantee holds exactly where the user
  can observe it. This is the spec's stated background contract
  (R1-2's "flag-and-refilter-on-activation", R1-7's "rule 1
  verbatim"), not a new decision.
- `probeCountersTrackCaptureRestore` (S0) drove the capture/restore
  counters through the non-initial snapshot; that path no longer runs
  them. The driver is now initial population (the surviving reset
  path), and the test additionally pins the S6 probe site: a
  non-initial snapshot enters `final_reconciliations` and none of the
  reset machinery.

New probe: `final_reconciliations` (site:
`Search::ReconcileFinalSnapshot`).

### S6 review round 1 (Tom, July 31, 2026): four findings, all verified and remedied

Verification notes first, then remedies (commits in Tom's approved
order; focused tests after each, full suite once at the end).

- **P2 — By-Item reconciliation over-allocated/over-scanned.**
  Confirmed, and the By-Tab row diff shared the shape: its per-bucket
  `have` sets summed to O(collection) hash nodes across buckets, so
  both modes paid roughly two collection-sized set builds beyond the
  accepted filter pass. Remedy: ONE reserved pointer→state table
  (kAccepted from the filter pass; the removal pass marks kRetained;
  "missing" = accepted-but-never-retained) serves both modes — the
  second million allocations are gone.
- **P2 — unconditional O(tabs²) selection pass.** Confirmed; the
  in-code "accepted like the rest of the O(collection) pass" comment
  was bad arithmetic (~3.4M `bucketDisplayLess` calls per clean
  snapshot at the 2600-tab preset, against the 2000+-tab driving use
  case). Remedy: `bucketDisplayLess` is a TOTAL order (stable-id
  tiebreak), so zero-moves-needed ⇔ `std::is_sorted` — the clean
  path is one O(tabs) scan; the selection pass survives as the rare
  displaced-path mechanism. (Tom's O(t log t) desired-order walk
  remains the upgrade if S7 ever shows real reorders mattering.)
  Pinned by the `rowsMoved == 0` spy in `noModelResetDuringRefresh`.
- **P3 — `rows_changed` conflated grains.** Confirmed both ways:
  a deleted empty bucket marked the flat bucket stale (wasted rebuild
  at the next mode switch); a final-only rename or new empty tab
  scheduled no column resize. Remedy: `content_changed` (item rows
  removed/inserted → staleness marks) vs `model_changed` (any model
  operation → resize) in BOTH `SnapshotReconciliation` and
  `DeltaApplication` — the S4 delta path shared the resize half of
  the conflation and is aligned in the same commit as an explicitly
  recorded correction discovered during this review (metadata-only
  deltas and empty-bucket structural changes now schedule the resize
  they visually implied; no other S4 semantics move).
- **P3 — diff was global-by-pointer, storm couldn't see it.**
  Confirmed, including reachability sharper than first assessed: the
  wrong-bucket state is fabricable through the PUBLIC API —
  `ApplyTabDelta(tabA, {itemLocatedInTabB})` parks the arrival under
  the delta anchor's bucket (insertions target the anchor). Remedy:
  the removal predicate is per stable key (accepted AND the item's
  (type, id) matches the bucket's key), which re-homes a parked item
  — removed there, inserted under its own key, never duplicated —
  and is also what makes the kRetained mark sound. Pinned by the
  direct-harness `reconciliationRehomesWrongBucketRow`, verified RED
  under a deliberately weakened global-membership predicate before
  landing green. The storm's closing check became a structural
  signature (top-level order + bucket header text + sorted
  per-bucket membership vs a from-scratch refilter, settled in
  By-Tab) — the old globally sorted name list could not see wrong
  ownership, stale headers, or misordered tabs.

**New informational rows (Tom's adjustment 3 — no budget yet; the
path runs on every refresh and the modes allocate differently).**
Release, offscreen, recorded presets, window-direct clean snapshot,
median of 5:

| Row | 100k | 1m | Attribution |
|---|---|---|---|
| S6 clean final snapshot, By-Tab | 23.8 ms | 334.8 ms | 1 reconciliation, 0 refilters/resets/sorts/key builds; peak +30.9 MB @1m (the state table — the one remaining collection-scale transient) |
| S6 clean final snapshot, By-Item | 12.6 ms | 204.8 ms | same shape; peak +0.0 MB (table already in the lifetime peak) |

All budgeted S3-S5 rows re-ran PASS in the same runs (unfiltered
refilter 29.5/255.4 ms, broad filter 81.7/834.6 ms, merge 33.3 ms
@1m, reactivation 478.6 ms @1m). The harness's initial population is
now `initial_refresh=true` (production-faithful: cached load takes
the reset path; the reconciliation is measured by its own rows). S7
carry-over note: `m2m2_benchmark` still populates through the
non-initial path — check its setup labels before recording S7
numbers.

## Budget table (S7, July 31, 2026): **PASS — every budgeted row, both presets**

The formal complete-table M1-M3 gate: the acceptance-criteria table
from `items-pipeline-m3.md`, run authoritatively on the finished model
(S0-S6 landed, through commit `f7fdebbf` plus this stage's harness
changes), regardless of the earlier conditional passes. Harness
`tests/m3_holdpoint_benchmark.cpp`, Release `build-release/`,
offscreen, spike presets, seed 20260729. Environment matches every
prior record: Apple M4, 32 GB (macMini-class), macOS 26.6, Apple
clang 21, Qt 6.11.1 release. Harness changes for this run: the
broad-filter row now records a process-level footprint delta across
the FIRST entry into the shape, so BOTH candidate worst materialized
shapes of the ≤ 300 MB row (By-Item, and broad-filter fully-expanded
By-Tab — R2-3) are judged at process level rather than by the
over-estimating gauge.

| Row | 100k | 1m | Budget (100k / 1m) | Verdict |
|---|---|---|---|---|
| Worst-case unfiltered By-Tab refilter, default collapsed (median of 5) | 23.6 ms | 282.3 ms | ≤ 60 ms / ≤ 500 ms | **PASS** |
| — sort share of the above | 0 ms | 0 ms | ≤ 5 ms | **PASS** (probe-attributed: 0 bucket sorts, 0 key builds, 0 keyed compares) |
| Single-bucket expand, cold keys (576-item quad, median of 5) | 0.53 ms | 0.59 ms | ≤ 10 ms | **PASS** |
| Clean By-Item reactivation (eager hydration, R3-1; end-to-end tab switch) | 46.7 ms | 475.3 ms | ≤ 100 ms / ≤ 0.5 s | **PASS** |
| By-Item full refilter (median of 3) | 106.6 ms | 1331.7 ms | ≤ 250 ms / ≤ 1.5 s | **PASS** |
| Broad-filter default-expanded refilter (ilvl ≥ 2, every bucket visible — R1-8; median of 3) | 76.6 ms | 825.9 ms | ≤ 150 ms / ≤ 1.2 s | **PASS** |
| Delta application, By-Tab visible bucket (R2-2 interleaved child-source replacement, 576 retained + 576 arrivals, median of 5) | 0.87 ms | 0.86 ms | ≤ 5 ms at 1m | **PASS** (100k informational) |
| By-Item merge (same interleaved child source, median of 5) | 3.9 ms | 32.7 ms | ≤ 50 ms at 1m | **PASS** (100k informational) |
| — same, window shown and laid out (A′ gate) | 4.1 ms | 32.5 ms | ≤ 50 ms at 1m | **PASS** (peak-footprint gate: ru_maxrss +3.9 MB @100k / +0.0 MB @1m across reps — no key-vector-sized transient) |
| Resident key memory, worst materialized shape, process-level footprint delta: By-Item | 21.8 MB | 223.7 MB | ≤ 300 MB at 1m | **PASS** |
| — broad-filter fully-expanded By-Tab (the other R2-3 candidate) | 22.4 MB | 210.0 MB | ≤ 300 MB at 1m | **PASS** |
| Collapsed-default resident key memory | 0 B | 0 B | ≈ 0 | **PASS** (gauge exactly 0) |
| Background-search resident key memory after deactivation | 0 B | 0 B | exactly 0 | **PASS** (gauge exactly 0; was > 0 while active) |

The gauge readings for the two worst shapes are 35.3 / 340.2 MB
(broad) and 35.7 / 344.2 MB (By-Item) at 100k / 1m — the known
deliberate over-estimate (S3 record); the budget is judged at process
level, where both shapes sit comfortably inside the spec's ~222-266 MB
expectation.

Per-component attribution (M2-M2 discipline — probes on the live
windows, micros on identical data outside them):

- Unfiltered refilter is filter-loop-bound as designed: bare
  `FilterItems` micro 20.2 / 328.8 ms of the 23.6 / 282.3 end-to-end;
  probes prove zero sort or key work.
- Broad refilter: 2000 / 2600 bucket sorts and key builds, 717k / 9.7M
  keyed compares, zero comparator calls; broad `FilterItems` micro
  19.0 / 329.3 ms; one-flat-bucket build+sort of the same visible
  result 74.2 / 1009.9 ms (upper bound the amortized per-bucket work
  stays under).
- By-Tab delta: exactly 1 bucket sort (the merge itself), 0 key
  builds, ~11.5k keyed compares, 0 index rebuilds, 0 refilters,
  0 resets — flat across presets (O(delta + bucket)).
- By-Item merge: 1 bucket sort, 0 key builds, ~20.6k keyed compares,
  0 rebuilds / refilters / resets; the bare no-view micro (33.4 ms
  @1m vs the live 32.7 ms) shows the row is essentially all model-side
  A′ work, view-side batch handling ~0.
- Clean reactivation: 0 refilters, 1 key build (the R3-1 eager
  hydration), 0 resets.
- Informational rows: initial publish + refilter 54.4 ms / 621.1 ms
  (the D6 reset path); mode switch into By-Item 82.2 / 1052.3 ms;
  single-source simple replacement 0.36 / 0.37 ms; manager-path merge
  (unpriced, Name) 4.2 / 33.3 ms, laid out 4.4 / 32.8 ms, priced-Name
  73.3 / 319.8 ms, priced-Price 97.0 / 583.8 ms (the mandated R3-2
  batch re-sort: model_updates=1, 2 bucket sorts, ~1.96M keyed
  compares @1m); S6 clean final snapshot By-Tab 23.2 / 312.9 ms and
  By-Item 12.3 / 212.4 ms (probes: 1 final reconciliation, 0
  refilters, 0 resets, 0 sorts; lifetime peak +3.7 / +30.7 MB By-Tab —
  the known state-table transient — and +0.0 MB By-Item).

### M2-M2 addendum: rerun on the finished model, and one surfaced observation

The S6 carry-over note asked that `m2m2_benchmark`'s setup labels be
checked before recording S7 numbers. Checked and rerun (same build and
environment):

- The harness populates through the production non-initial path (the
  worker's `FinishUpdate` emits `initial_refresh=false`), which since
  S6 means its "populate update" includes the row reconciliation
  rather than a reset — the labels are accurate, the number's meaning
  changed. It also no longer compiled: it still called
  `SetDeltaThrottleInterval`, deleted with the throttle in S5
  (EXCLUDE_FROM_ALL, so no build had caught it). The call is removed;
  the measured window now contains the synchronous per-delta model
  application, which is exactly the production path.
- Whole-path medians (resolve → quiescent): replacement 10.4 ms @100k
  / 11.7 ms @1m; removal 10.2 / 10.5 ms. **~10.1-10.2 ms of every
  sample is a flat `post (finish)` tail**; the synchronous reply
  application proper (whole − post) is **0.26 ms @100k / 1.53 ms @1m**
  (removal 0.14 / 0.31 ms) — still inside the long-discharged M2-M2
  budgets (< 2 ms / < 16 ms) with the M3 model application now
  included (ui primary 0.02 / 0.27 ms).
- **The surfaced observation:** the flat tail was profiled (macOS
  `sample`) to `MainWindow::ResizeTreeColumns` — the 0 ms single-shot
  `m_delayed_resize_columns` timer, armed by the S4/S6 handlers on
  every `model_changed` delta and firing on the next event-loop pass.
  Its ~10 ms cost is scale-independent (Qt bounds
  `sizeHintForColumn`'s scan) but **newly per-delta**: under M2 the
  throttle coalesced column resizing to at most one per tick, under
  M3 every applied delta pays it. No M3 budget row binds it (it is
  deferred, outside every timed window in this table), and at the
  production delta cadence (~1 per 20 s under rate limits) it is
  invisible; a burst catch-up of ~15 replies costs ~150 ms total in
  ≤ 10 ms event-loop slices. Surfaced at the S7 pause and decided in
  S7 review round 1 (below): the delta-path arming is debounced.

**Gate verdict: PASS.** Every budgeted row passes at both presets with
headroom; both candidate worst memory shapes pass at process level.
Per the implementation sequence, the sequence pauses here for Tom
before S8 (wrap-up).

### S7 review round 1 (Tom, July 31, 2026): two findings, both accepted

**(1) The gate could not fail.** `printRows` computed and discarded its
miss flag, `main` returned 0 unconditionally, and the memory rows were
print-only `PASS`/`MISS` strings — on a platform without the footprint
API the two *binding* 1m memory rows silently degraded to gauge-only
prints with no verdict. Fixed: budget rows and the four memory checks
(collapsed-default ≈ 0, background exactly 0, both ≤ 300 MB footprint
rows) accumulate into one verdict, a required measurement being
unavailable at a preset where its budget binds counts as a miss, and
the harness prints `S7 gate: PASS`/`FAIL` and exits non-zero on any
miss. The two `ru_maxrss` peak observations stay informational by
Tom's decision — they are S5-remedy verification evidence, not rows of
the frozen acceptance-criteria table, and binding them would have
required inventing a threshold the spec never stated. Both presets
rerun after the change: every row still passes and the process exit
code is 0 (verified directly, not through a pipeline). The recorded
table above is unchanged — the runs it records were adjudicated by
inspection and every row passed.

**(2) The per-delta deferred column resize (the observation the
addendum above parked for Tom's judgment): decided.** Tom's call —
the delta-path arming becomes a **non-resetting single-shot debounce**
(the D9 pattern at presentation scale); width-relevance gating was
considered and deliberately not added. Remedy, pin, and measured
effect below.

**Step 2 (the remedy).** `MainWindow::ScheduleDeltaResizeTreeColumns`
arms a new non-resetting 250 ms single-shot
(`DELTA_RESIZE_DEBOUNCE_MS`); the one delta-path site is
`FinishDeltaApplication`, where both per-reply delta forms (content
delta and child reconciliation) funnel. Non-resetting means an armed
timer keeps its deadline, so a sustained burst cannot starve the
resize — it pays at most one ~10 ms pass per interval (bounded
staleness AND bounded work) instead of one per applied delta. Every
user-initiated site (mode switch, expand/collapse signals and
commands, buyout command, refilter) and the once-per-refresh final
reconciliation keep the immediate 0 ms same-turn coalescer, and —
Tom's refinement — `ResizeTreeColumns` itself stops the pending
debounce, so an immediate resize supersedes a queued delta resize
rather than being followed by a redundant second pass. New probe
`column_resizes` counts actual resize passes.

Pin `deltaResizeDebounceCoalescesBurst` (tst_mainwindow, both
clauses): deltas applied in separate event-loop turns yield exactly
one resize pass per interval and none inside it; an immediate resize
while a delta resize is pending supersedes it (no third pass after
the debounce deadline). Both clauses were verified RED against their
defect shapes before landing (clause 1 against the old per-delta
immediate arming, clause 2 with the supersede stop removed). Pin
lesson from the red check: the first draft applied the burst in ONE
event-loop turn, which even the old 0 ms coalescer collapses to one
pass — the defect quantifies over event-loop turns (one per
production reply), so the pin must too.

Measured effect (`m2m2_benchmark`, 100k, same build/environment):
whole-path median 10.4 ms → **0.216 ms** (the `post (finish)` tail
10.14 ms → 0.021 ms) — back at the M2-era 0.234 ms with the M3
synchronous model application now included in the unit. Suite green,
34/34, 57 mainwindow scenarios.

**Step 2 follow-up (Tom's review of the remedy, July 31).** One
supersession race: stopping the debounce only inside
`ResizeTreeColumns` is too late when the debounce deadline has
already expired and its timeout sits queued ahead of the freshly
started 0 ms timer — the queued timeout fires first, two passes.
`ScheduleResizeTreeColumns` now stops the debounce at scheduling
time; the stop inside `ResizeTreeColumns` is retained defensively.
Pin clause 3 reproduces the window deterministically (`qSleep` lets
the deadline expire unprocessed, so the timeout is queued when the
immediate schedule happens) and was verified RED with the
scheduling-time stop removed. Recorded benchmarks unaffected (Tom:
no rerun needed). **With this follow-up, S7 carries Tom's formal go
for S8.**

## S8 — wrap-up: design review, supersession-map verification, turnover (July 31, 2026)

### Design-review criteria (spec, "Design-review criteria"): all six HOLD

Verified on the finished branch by independent evidence sweep, judged
here. Key evidence per criterion:

1. **No refresh-driven reset.** The only reset primitive is
   `ItemsModel::beginUpdate` with exactly two call sites:
   `Search::FilterItems` (sole caller `ModelViewRefresh`, whose five
   call chains are all user-initiated or the `initial_refresh` branch
   of `OnItemsRefreshed`) and `Search::SetViewMode` (user mode
   switch). Both delta handlers and the non-initial snapshot route
   through row operations only.
2. **Delta path O(delta + affected bucket + log tab-count).** The
   per-delta path touches one bucket, resolved through the ordered
   stable-key map (`m_row_by_key`, `std::map` — O(log tabs), the
   deliberate S4 round 1 finding-6 design; the serial map is the
   hashed sibling); the only O(n) pass is D4's stated
   `Bucket::ReplaceSourceRows`. The bounded terms outside the
   criterion's original phrasing — the log-factor lookup, the
   O(tab-count) row-lookup remap on structural ops (both accepted
   at S4 review round 1, addendum above), and the By-Item
   buyout-repaint flat-bucket scan (accepted at S2, batch path not
   delta path) — are registered as the spec's third post-freeze
   amendment (S8 external review).
3. **Single ordering truth.** Every column family's `lt` and `key`
   derive from one private computation (`Column::parts`,
   `PriceColumn::head`, `DateColumn::head`); the shared suffix equals
   `Item::operator<`'s `(PrettyName, uid, hash)` exactly;
   equivalence pinned by `keyedOrderMatchesComparatorOrder`.
4. **Choke point complete; batching holds.** Every public mutation of
   the buyout lookup state reports through `RecordChange` (Set,
   SetTab, both Compress passes, Clear, MigrateItem — the latter
   under its own internal batch); nested batches emit only at the
   outermost boundary; the snapshot pricing sequence is one batch;
   `RepaintBuyoutCells` runs unconditionally of the sort column. One
   latent hole found and closed this stage: `BuyoutManager::Load()`
   rewrote the lookup state wholesale, without recording, as a
   PUBLIC method (benign — sole caller is the constructor,
   pre-model) — now private with the constraint documented.
5. **Lane separation.** Zero `poe::*` in `sortkey.h`, `search.h`,
   `bucket.h`, `locationinventory.h`; the only wire types in the key
   surface's transitive closure are `ItemLocation`'s ingestion
   constructors, which the criterion permits.
6. **Exactly three stated renegotiations, none silent.** R1-4
   (metadata half, retiring M2 R7-2's exception), R1-7 (rule 1
   relaxed for the active search), and the July 31 intersection-set
   deletion amendment. The deleted sets have zero occurrences in
   `src/` — the amendment was executed, not just recorded. Three
   spec passages still described the sets as live (the amendment had
   promised the pin-text narrowing but it had not been applied);
   completed this stage: D3's index bullet and the
   `deltaUpdatesVisibleIndexesIncrementally` criterion narrowed to
   `m_visible_by_id`, the staleness-preamble mention annotated
   as-at-freeze, and the amendment labeled as the third
   renegotiation so the count greps true.

### Supersession map: executed exactly

All three categories verified against the current tree and git
history: every category-1/2 pin ABSENT (each removed in its named
stage — S4 deletions in `2a572e5d`, S5 deletions in `f51b7906`),
every named successor PRESENT (including the two same-commit
landings the no-green-gap traps required:
`selectionIntentSurvivesCrossTabMoveAcrossDeltas` in `2a572e5d`,
`scrollAndCaptureSurviveUserRefilter` in `f51b7906`), every
category-3 survivor PRESENT as a live test slot. No stray throttle
machinery: `m_delta_throttle`, `ScheduleDeltaTick`,
`OnDeltaThrottleTimeout` have zero occurrences in `src/` and
`tests/`; the only delta-adjacent timer is the S7 resize debounce.
Two stale present-tense comments describing retired machinery
(`mainwindow_bridge.cpp` "throttled, intersection-gated refilter
machinery"; `mainwindow.cpp` "when the throttled reset needs it")
reworded to the current design. Evidence note for future audits:
`git log -S` misses `reselectionSurvivesCrossTabMove`'s removal
because the S4 commit added as many comment mentions as it removed
occurrences — use `-G`.

### Turnover

- Spec: M1-M3 open item marked RESOLVED with the S5 miss/remedy and
  S7 gate summary; the three intersection-set passages completed
  (above); the complexity criterion's dated clarification.
- Sequencing doc: status IMPLEMENTED (July 31, 2026).
- Parent plan: M2 heading marked SHIPPED (PR #185), M3 heading
  marked IMPLEMENTED with a result summary; M1-M2 named as the one
  remaining follow-up (blocks nothing).
- `docs/README.md`: pipeline entries updated (M2-M2 resolved, M1-M3
  passed, sequencing docs implemented, this result doc cited).
- Findings register: F67 was moved to the resolved ledger at S1;
  F66's entry now records that M3 completed without reworking the
  legacy stores (D7's deliberate non-exercise) and the hook stands.

### S8 review round 1 (external, July 31, 2026): two findings, both accepted — documentation-only remedies

(1) Medium: the complexity audit above originally claimed "hash
lookups" — the By-Tab bucket-row index is the ordered
`std::map` `m_row_by_key` (O(log tabs), the deliberate S4 round 1
finding-6 design), so ordinary deltas did not literally meet the
criterion as written, and the S8 clarification cited an S4
acceptance this document did not contain (it lived only in commit
`7ddd08db`'s message). Tom's call: keep the reviewed ordered-map
design (the log factor is ~11 key comparisons against a 0.86 ms
row; a hash index would be wording-driven churn) and make the
record honest — the bound is registered as the spec's **third
post-freeze amendment**, the finding-6 acceptance is now durably
recorded in §S4 review round 1 above, and criterion 2's audit text
is corrected. (2) Low: `docs/README.md` still labeled the M2
implementation sequence "accepted" against its own IMPLEMENTED
header, contradicting the map's header-wins rule — fixed. No code
changes, no benchmark reruns (reviewer concurred both).
