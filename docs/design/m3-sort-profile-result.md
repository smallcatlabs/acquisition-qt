# M3 sort-profile result (input to the M3 spec)

Status: **MEASURED July 30, 2026** — the profiling evidence the parent
plan's M3 inputs require before the M3 spec picks its central lever
("profile before choosing levers", S1-M2 amendment finding 5). This is
a spike result in the R3-4 tradition: produced on the throwaway branch
`spike/m3-sort-profile` (off master 3549b214, the M2 merge), whose
prototype code is never merged; this document is the deliverable. **No
lever is selected here** — bounds are established; selection belongs
to the M3 spec.

Headline: **the comparator claim is proven, and sharpened.** The
post-reset whole-model re-sort is comparator-bound, and the dominant
cost inside the comparator is not string comparison or allocation but
the **two `QRegularExpression` evaluations `Column::multivalue` runs
per side on every comparison** — roughly three quarters of every
comparator call, paid twice per comparison, ~10 million comparisons
per refilter at 1m. Columns with custom comparators that skip
`multivalue` entirely (Price, Date) sort the same buckets **7×
cheaper**; a precomputed-key prototype sorts them ~40× cheaper
(~10× including per-refilter key rebuild).

## What was measured

Three groups, at both scales (100k and 1m presets):

1. **The live full-refilter split** on the real production path:
   `MainWindow::OnItemsRefreshed` → `Search::FilterItems` (begin-reset
   / filter loop / bucket display ordering / end-reset) →
   `setSortingEnabled(true)` → `ItemsModel::sort` → `Search::Sort`
   over every bucket — reconfirming the S1-M2 whole numbers on
   current master, now with the sort's interior attributed.
2. **Comparator attribution**: live comparator invocation counts and
   multivalue-path counts (probes in `Column::lt`,
   `Column::multivalue`, `PriceColumn::lt`, `DateColumn::lt`,
   `Item::operator<`); micro-benchmarks on copies of the published
   collection — per-column bucket-sort cost, and per-call component
   costs of the NameColumn comparator (QVariant `value()`,
   `toString()`, the two regexes, `PrettyName()`, the tie-break).
3. **Lever bounds** (prototype quality, no decisions): a
   precomputed-sort-key variant and a sort-only-expanded-buckets
   variant, on the same bucket shapes the live path sorts, plus the
   By-Item single flat bucket both ways. **Born-sorted buckets
   (filtering from a pre-sorted master) was not prototyped** — its
   ceiling is implied by the keyed numbers but not measured.

Instrumentation: `src/util/spikeprofile.h` (spike-only globals) with
probes in `search.cpp`, `items_model.cpp`, `column.cpp`, `item.cpp`,
and `mainwindow.cpp`; harness `tests/m3_sort_benchmark.cpp` (target
`m3_sort_benchmark`, `EXCLUDE_FROM_ALL`). All of it lives only on the
spike branch. Reproduce with:

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/Users/tom/Qt/6.11.1/macos
cmake --build build-release --target m3_sort_benchmark -j 8
./build-release/tests/m3_sort_benchmark --preset 100k   # and --preset 1m
```

## Environment (per the M2-M2 conventions)

- Hardware: Apple M4, 32 GB RAM (macMini-class), macOS 26.6
  (Darwin 25.6.0)
- Compiler: Apple clang 21.0.0 (clang-2100.1.1.101), arm64
- Qt 6.11.1 (release build), offscreen QPA platform
- Build: CMake `Release` (`-O3`), separate `build-release/` tree
- Allocator: system malloc (no substitution)
- Logging: spdlog level `info` (production default)

## Datasets and shapes

`tests/spikedataset.h`, seed 20260729 — the same presets as S1-M2 and
M2-M2 (note: the generator's stash ids were shortened July 30, after
the recorded M2-M2 runs; irrelevant to sort cost):

| Preset | tabs | mean items/tab | quad share | published items |
|---|---|---|---|---|
| 100k | 2000 | 50 | 0.10 | 101,048 |
| 1m | 2600 | 400 | 0.80 | 975,711 |

The collection is published once through the production snapshot path
(`ItemsManager::OnItemsRefreshed`), then the measured unit is
`MainWindow::OnItemsRefreshed()` — the worst-case unfiltered
refilter-plus-resort of the whole collection on the single unfiltered
default search, By-Tab view, default sort (column 0 = Name,
**descending** — the model's pinned default). 1 warmup + 5 measured
reps at 100k, + 3 at 1m; reported statistic is the **median** (spread
across reps and across a full rerun was within ±3%).

## 1. Live full-refilter split

All values ms, median. "—" marks components below 0.01 ms.

| Component | 100k | 1m |
|---|---|---|
| **whole `OnItemsRefreshed`** | **422.5** | **5,561.9** |
| `FilterItems` total | 35.2 | 398.1 |
| — begin reset (`beginResetModel`) | — | — |
| — filter loop + bucketing | 34.4 | 391.3 |
| — bucket display ordering | 0.8 | 6.8 |
| — end reset (view rebuild) | 0.01 | 0.01 |
| `setSortingEnabled(true)` → `ItemsModel::sort` | 386.8 | 5,163.0 |
| — `Search::Sort` (per-bucket `std::sort`) | 386.8 | 5,162.9 |
| — everything else in `ItemsModel::sort`¹ | — | — |
| `RestoreViewExpansion` | 0.04 | 0.06 |
| Reselect + scroll restore | 0.6 | 0.7 |

¹ `layoutAboutToBeChanged`/`layoutChanged` emits, the persistent-index
snapshot, and the remap are all ≤ 0.005 ms — there were no persistent
indexes below bucket level in the harness run (no expansion, no
selection). With expanded tabs and a selection they cost more, but
S1-M2 already bounded the whole restore side collectively at a few ms.

This reconfirms the S1-M2 whole numbers on current master: their
~455 ms / ~5,370 ms resets (sort ~390 / ~5,040–5,076) against our
422 / 5,562 (sort 387 / 5,163) — same shape, within ~5–10% across a
somewhat different measured boundary (S1-M2's window was the throttled
tick, capture → scroll restored; ours is the plain
`OnItemsRefreshed` call).

**The model reset itself is ~free; the "reset cost" is the re-sort.**
`beginResetModel`/`endResetModel` are microseconds. The filter loop is
the only other real cost (~7% of the total at both scales), and the
sort is ~92–93%, all of it inside `Bucket::Sort`'s `std::sort` calls.

Live comparator counts (Name column, descending):

| | 100k | 1m |
|---|---|---|
| `Column::lt` calls | 728,360 | 9,842,111 |
| live ns per comparison | ~531 | ~525 |
| `multivalue` calls (2 per lt) | 1,456,720 | 19,684,222 |
| multivalue path | 100% string (both regexes fail) | same |
| tie-break `Item::operator<` reached | 16.6% | 34.9% |

## 2. Inside the comparator — the claim proven

**Per-column bucket sort** (micro, fresh copies of the same buckets,
descending; live-equivalent — the Name row matches the live sort
within 1%):

| Column | 100k ms | ns/compare | 1m ms | ns/compare |
|---|---|---|---|---|
| Name (default sort) | 385.2 | 529 | 5,116.4 | 520 |
| Property "Q" (Quality) | 340.2 | 466 | 4,603.0 | 468 |
| ilvl | 351.2 | 483 | 4,849.3 | 493 |
| **Price (custom `lt`, no multivalue)** | **52.4** | **72** | **719.5** | **73** |
| **Date (custom `lt`, no multivalue)** | **53.1** | **73** | **728.4** | **74** |

Every column that routes through `Column::multivalue` costs
~470–530 ns/compare; the two columns with bespoke comparators that
never touch it cost ~72–74 ns — **a 7× spread on identical buckets**.
Crucially, **ilvl is numeric and still costs 483–493 ns**: its value
matches the first regex immediately, so even the regex *fast path*
pays the full toll. The multivalue machinery, not the data type, is
the cost.

**Component costs** (NameColumn, fixed random-pair sample; each row is
the cost of doing that work for both sides of one comparison):

| Component (both sides) | 100k ns/call | 1m ns/call |
|---|---|---|
| **full `Column::lt`** | **773** | **911** |
| `value()` (QVariant of `PrettyName`) | 96 | 134 |
| `value().toString()` | 119 | 166 |
| toString + **both regexes** | 721 | 862 |
| `PrettyName()` alone | 80 | 116 |
| replicated multivalue (full key build) | 910–994 | 1,030 |
| `Item::operator<` tie-break alone | 95 | 124 |

Attribution reading (ratios, not exact addends — see caveats): of a
~773–911 ns comparator call, the toString-plus-regex stage accounts
for ~93–95%, and subtracting the toString cost leaves **the two
`QRegularExpression::contains` evaluations at roughly 600–700 ns per
comparison — about three quarters of the whole comparator**. The
QVariant/QString conversions and the `PrettyName()` allocations
(1 QString concatenation per call for named items) are the next terms
at roughly 10–20% combined; the tie-break, when reached, adds ~100 ns
(two more `PrettyName` allocations plus QString compares). The
`std::sort` machinery itself is ~12–13 ns/compare (measured by the
keyed sort below).

For the default Name sort both regexes *always* fail (item names never
look like `12`, `+16%`, or `12-14`), so the string path runs
every time: 4 regex evaluations + 4 `PrettyName()`-class allocations +
2 QString tuple copies per comparison, at ~10M comparisons per 1m
refilter.

## 3. Lever bounds (ceilings, not decisions)

**A. Precomputed sort keys** (build the multivalue tuple + tie-break
key once per item; sort with plain tuple compares — QString relational
compares remain, no QVariant/regex/allocation per comparison):

| | 100k | 1m |
|---|---|---|
| key build (once per item) | 33.1 ms | 367.5 ms |
| sort with cheap compare | 8.5 ms (11.6 ns/cmp) | 129.9 ms (13.2 ns/cmp) |
| **build + sort vs live sort** | **41.6 vs 386.8 ms (9.3×)** | **497.4 vs 5,163.0 ms (10.4×)** |
| sort alone (keys already valid) | 8.5 ms (45×) | 129.9 ms (40×) |

Even rebuilding every key on every refilter beats the current sort by
~10×; if keys survive across refilters (they depend only on item
content and, for Price/Date, buyout state), the re-sort itself drops
to ~40×. A 1m worst-case refilter's sort would go from ~5.2 s to
~0.5 s (rebuild) or ~0.13 s (cached keys).

**B. Sort only expanded/visible buckets** (the K largest buckets stand
in for a worst-case expanded set; per-tab caps bound each bucket at
576 items):

| | 100k | 1m |
|---|---|---|
| top 5 buckets | 14.7 ms (2,792 items) | 15.9 ms (2,880 items) |
| top 20 buckets | 34.4 ms (6,919 items) | 62.8 ms (11,520 items) |

The cost becomes proportional to the *expanded set*, independent of
collection size. Note the default view starts fully collapsed (an
unfiltered By-Tab search), so the deferred work at first paint is the
entire sort.

**By-Item flat bucket** — the structure that fights both levers
(single bucket holding everything; lever B cannot help it at all):

| | 100k | 1m |
|---|---|---|
| real comparator | 1,017.0 ms | 12,874.5 ms |
| precomputed keys (build + sort) | 33.4 + 27.2 ms | 371.3 + 393.4 ms |

The flat sort is ~2.5× worse than the By-Tab sort of the same items
(larger n per `std::sort`, higher tie rate) — **~12.9 s at 1m today**
— and the keyed variant collapses it to ~0.77 s.

## Attribution notes and honest caveats

- **Probe overhead is negligible and common-mode.** The live and micro
  Name sorts agree within 1% and both include the same counters (a
  few increments per comparison against a ~520 ns comparison). The
  per-bucket timer pairs add ~2 × 2,600 clock reads per sort.
- **The random-pair component sample is not the in-sort access
  pattern.** Micro per-call costs (773–911 ns) exceed the live
  per-comparison cost (~520–530 ns) because random pairs touch the
  whole collection cache-cold while `std::sort` works one
  cache-warm bucket at a time. Component rows are therefore reliable
  as *ratios* (what fraction of a comparator call each stage is), not
  as absolute addends to the live number.
- **Dataset name diversity is artificially low** (16 prefixes × 12
  suffixes × 18 bases), which inflates the tie-break rate (16.6% at
  100k, 34.9% at 1m — real accounts should tie less). The tie-break
  is a ~100–124 ns term; the regex cost per comparison does not
  depend on name diversity, so the headline attribution stands.
- **Price/Date buyout lookups ran against an all-default
  `BuyoutManager`** (the dataset carries no notes and no buyouts were
  set), so their ~72–74 ns may understate a priced account's cost;
  they cannot approach multivalue's cost regardless of the lookup.
- **No paint**: offscreen numbers, as in S1-M2. The restore-side
  machinery ran with no expansion/selection state, so the
  persistent-index snapshot/remap costs in `ItemsModel::sort` were
  measured at their floor (S1-M2's collective few-ms bound covers the
  realistic case).
- **The lever prototypes take no positions**: key invalidation (item
  churn, buyout edits invalidating Price/Date keys), key memory
  (~4 QStrings + 2 doubles per item — not measured), per-bucket
  sortedness bookkeeping and sort-on-expand latency for lever B, and
  the born-sorted-master alternative are all M3 spec work. The
  numbers here are achievable ceilings under the current comparator
  semantics, nothing more.
- **The By-Item flat sort's comparison count was not separately
  counted** (time only); the compare counts in the per-column table
  are exact.
- Measured on the spike branch off master 3549b214; the full ctest
  suite passes on the branch with the instrumentation in place.

## New findings routed to the register

- **F67** — `Item::operator<` compares `m_hash` against itself (the
  tie-break tuple's third element is the lhs hash on both sides), so
  the hash tie-break is dead code. Harmless today; recorded in
  `docs/cleanup/findings.md` with the M3-relevant consequence: a
  precomputed-key design must choose between reproducing the
  *intended* `(name, uid, hash)` order and the *actual* `(name, uid)`
  one.

## What this gives the M3 spec (evidence, not decisions)

1. **The suspected driver is confirmed and localized.** The sort is
   comparator-bound; the comparator is regex-bound (~3/4 of each
   call), with QVariant/QString conversion and `PrettyName`
   allocation as the second-order terms. Any lever that stops paying
   `multivalue` per comparison captures most of the available win —
   and conversely, micro-optimizing the regexes alone (e.g. cheaper
   parsing) could plausibly get a large fraction of lever A's win
   without precomputation, though that variant was not prototyped.
2. **Both named levers have large, now-quantified ceilings** —
   precomputed keys ~10× (rebuild-per-refilter) to ~40× (cached),
   expanded-only proportional-to-expanded — and they compose: they
   attack independent factors (cost per comparison vs number of
   comparisons).
3. **The By-Item flat bucket is the worst structure by far**
   (~12.9 s at 1m) and lever B cannot help it; whatever M3 chooses
   must have an answer for it (lever A alone brings it to ~0.77 s).
4. The model-reset side of the old "reset cost" story is dead: begin/
   end reset are microseconds, and the filter loop (~0.4 s at 1m) is
   the second-largest component after the sort — worth keeping in
   frame once the sort stops dominating.
