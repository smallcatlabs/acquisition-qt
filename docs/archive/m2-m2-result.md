# M2-M2 measurement result (items-pipeline M2, stage 4 checkpoint)

Status: **MEASURED July 29, 2026; reviewed by Tom the same day at the
stage-4 hold point. Remedy selected: the named pair** — the required
manager source-keyed map (`FetchSourceKey → Items`, lazily rebuilt
flat vector) **plus the symmetric worker-side source-keyed store**
(D3 names it, gated on exactly this measurement; the gate fired).
Both whole-path budgets missed; the manager's marginal erase missed
at 1m. The dominant component is four O(all-items) erase passes per
reply, split evenly between worker and manager (attributed separately
per R3-3/R4-1) — a manager-only remedy would leave the path at
roughly half its measured cost, still far over budget at both scales,
which is why the pair was selected. **The pair was implemented the
same day (`SourceKeyedItems`, both sides) and validated by the R6-6
rerun appended at the bottom of this document: every budget now
passes with more than an order of magnitude of headroom** (whole path
0.234 ms @100k against 2 ms, 1.225 ms @1m against 16 ms).

This is the addendum the spec's open-items entry asks for (M2-M2,
first implementation checkpoint; D3, R2-3/R3-3/R4-1/R5-4/R6-6/R7-3):
the complete synchronous reply application measured on the real
production reply path, with per-component attribution, at the 100k and
1m scales. The spec's open-items entry (`items-pipeline-m2.md`,
"Open items requiring spike or measurement") records the resolution
and points here.

## What was measured

The measured unit is one accepted content reply's complete synchronous
application — worker handler → persistence → manager apply → scoped
pricing → UI intersection/fan-out — from the moment the facade future
resolves until the event loop is quiescent again. The harness
(`tests/m2m2_benchmark.cpp`, written fresh for this checkpoint) wires
the production objects exactly the way `Application` does: the real
`ItemsManagerWorker`, a real sqlite `UserStore` on the persistence
lane, the real `ItemsManager` (streaming apply + scoped pricing), and
the real `MainWindow` (D9 intersection and rule-1 fan-out) — driven
through the typed facade fake, which is the worker suite's standard
harness boundary.

Attribution method: timing probes are connected around the production
slots (direct connections run in connection order), segmenting the
live path into named buckets that sum to the whole-path time with the
residual reported explicitly. The erase, parse+append, and pricing
components are additionally measured as micro-benchmarks on identical
data outside the timed windows.

Excluded from the measured windows, deliberately:

- The D9 throttled reset (the tick): it is the separate coalesced
  refilter path, not part of the per-reply synchronous unit; the
  harness sets S to one hour so no tick lands inside a window. Reset
  cost at these scales is recorded in the M2 spec's D9 result
  (`s1-m2-spike-result.md` is retired to git history).
- Reply construction and serialization: the fake's payload (typed
  stash + wire bytes) is built before each window opens, standing in
  for work the network facade does below the boundary in production.

## Environment (recorded per D3)

- Hardware: Apple M4, 32 GB RAM (macMini-class), macOS 26.6
  (Darwin 25.6.0)
- Compiler: Apple clang 21.0.0 (clang-2100.1.1.101), arm64
- Qt 6.11.1 (release build), offscreen QPA platform
- Build: CMake `Release` (`-O3`), separate `build-release/` tree
- Allocator: system malloc (no substitution)
- Logging: spdlog level `info` (production default); the per-reply
  debug lines inside the measured path are gated off at this level
  exactly as in a production session

## Datasets and shapes (fixed, recorded)

`tests/spikedataset.h` (the deterministic generator ported from the
S1-M2 spike; same `(config, seed)` → same collection and churn
sequence, seed 20260729). July 30 note: the generator's stash ids
were shortened to the modern 10-character shape after every run
recorded here (they previously tripped the worker's legacy-id
warning once per tab) — reruns are therefore not byte-identical to
these runs' dataset; immaterial at the recorded margins:

| Preset | tabs | mean items/tab | quad share | published items |
|---|---|---|---|---|
| 100k | 2000 | 50 | 0.10 | 101,048 |
| 1m | 2600 | 400 | 0.80 | 975,711 |

The 1m preset is the S1-M2 spike's retuned preset. The collection is
populated through the real reply path (update 1 delivers every tab's
contents), then a second update streams the measured replies:

- **Replacement shape:** `ChurnTab(t, 0.3)` — ~15% of the tab's items
  removed, ~15% modified in place (same id), matched arrivals — for a
  deterministic stride of tab indices; 40 samples at 100k, 12 at 1m,
  after 3 unmeasured warmup replies.
- **Removal shape:** an emptied fetch source (a reply with an empty
  `items` array) for 10 further tabs at 100k, 6 at 1m.

The current search is a single unfiltered search (every reply
intersects through the removal half — the worst case for the visible
result and the intersection bookkeeping). Update 2 is terminated by a
terminal failure after the measured replies, which also exercises the
published copy's no-rollback survival on the way out.

Reported statistic: **median** over the samples, with max alongside.

## Results — 100k

Replacement replies (churn 0.3; 40 samples, median reply 48 items):

| Bucket | median | max |
|---|---|---|
| **whole path** | **7.382 ms** | 7.793 ms |
| pre (dispatch) | 0.027 ms | 0.038 ms |
| persistence | 0.088 ms | 0.131 ms |
| worker erase+parse | 1.843 ms | 2.080 ms |
| manager apply (erase+append+pricing) | 1.583 ms | 1.645 ms |
| UI intersection/fan-out (primary) | 0.001 ms | 0.005 ms |
| worker between (counters, status fan-out, ghost erase) | 1.796 ms | 1.924 ms |
| manager reconcile (expected-set erase) | 2.024 ms | 2.199 ms |
| UI intersection/fan-out (reconcile) | 0.001 ms | 0.004 ms |
| post (finish) | 0.034 ms | 0.042 ms |
| residual | 0.000 ms | 0.000 ms |

Micro-benchmarks (identical data, outside the windows):

| Component | median | max |
|---|---|---|
| worker erase | 1.840 ms | 1.993 ms |
| **manager marginal erase** | **1.828 ms** | 1.897 ms |
| parse+append | 0.075 ms | 0.233 ms |
| pricing | 0.002 ms | 0.004 ms |

Removal replies (emptied source; 10 samples): whole path median
7.169 ms — the same shape without parse/pricing; the erase passes
dominate identically.

## Results — 1m

Replacement replies (churn 0.3; 12 samples, median reply 512 items):

| Bucket | median | max |
|---|---|---|
| **whole path** | **74.928 ms** | 78.248 ms |
| pre (dispatch) | 0.085 ms | 0.110 ms |
| persistence | 0.240 ms | 0.257 ms |
| worker erase+parse | 19.106 ms | 19.435 ms |
| manager apply (erase+append+pricing) | 15.616 ms | 17.576 ms |
| UI intersection/fan-out (primary) | 0.003 ms | 0.004 ms |
| worker between (counters, status fan-out, ghost erase) | 20.252 ms | 21.188 ms |
| manager reconcile (expected-set erase) | 20.170 ms | 20.344 ms |
| UI intersection/fan-out (reconcile) | 0.001 ms | 0.002 ms |
| post (finish) | 0.068 ms | 0.075 ms |
| residual | 0.000 ms | 0.000 ms |

Micro-benchmarks (identical data, outside the windows):

| Component | median | max |
|---|---|---|
| worker erase | 18.845 ms | 19.420 ms |
| **manager marginal erase** | **18.830 ms** | 19.259 ms |
| parse+append | 0.789 ms | 0.927 ms |
| pricing | 0.017 ms | 0.022 ms |

Removal replies (emptied source; 6 samples): whole path median
74.850 ms — indistinguishable from replacements; the erase passes are
the whole story.

Populate (whole first update through the real path, 2600 replies +
final snapshot with sort and full refilter): 93.9 s at 1m, 5.9 s at
100k — for context only; not part of the measured unit.

## Budget verdicts

| Budget (D3) | 100k (< 2 ms) | 1m (< 16 ms) |
|---|---|---|
| Manager marginal erase | **PASS** — 1.828 ms (slim: max 1.897) | **MISS** — 18.830 ms |
| Whole path | **MISS** — 7.382 ms | **MISS** — 74.928 ms |

Consequences under the spec's frozen conditional (D3, R5-4):

- **The manager remedy is required, not discretionary**: a
  source-keyed map (`FetchSourceKey → Items`) with a lazily rebuilt
  flat vector for `items()` consumers — the natural M3 representation.
- **The whole-path miss requires a real remedy for the dominant
  component.** The dominant component is not one bucket: it is **four
  structurally identical O(all-items) linear passes per reply**, each
  ~19–20 ms at 1m (~1.8–2.0 ms at 100k), together ~99% of the path:
  1. the worker's primary erase (`RemoveItemsFetchedBy`),
  2. the worker's expected-set ghost erase (inside "worker between"),
  3. the manager's primary erase,
  4. the manager's expected-set reconcile erase.
  Worker and manager are attributed separately as R3-3/R4-1 require:
  each side contributes two passes of near-identical cost, so a
  manager-only remedy would leave the path at roughly half its
  measured cost — still far above budget at both scales. The remedy
  the numbers point to is the spec's named pair: the required manager
  map **plus** the symmetric worker-side source-keyed store (D3 names
  it, measurement-gated — this measurement is the gate, and it fires).
  Everything else — persistence, parse+append, pricing, UI
  intersection/fan-out, dispatch — is collectively under 1.2 ms at 1m
  and needs nothing.
- A selected remedy must be validated by **rerunning this measurement**
  with the remedy in place before M2 is considered complete (R6-6).

## Attribution notes

- "worker erase+parse" is the live segment between the persistence
  slot returning and the primary delta emit; the micro rows split it:
  the erase dominates and parse+append is the small remainder,
  consistent with the live segment.
- "worker between" covers the counter increment, the status fan-out
  through `ItemsManager`/`MainWindow`, child-batch discovery, and the
  worker's own expected-set ghost erase — the third linear pass of a
  top-level reply.
- "manager apply" nests scoped pricing (D7); the pricing micro row
  shows it is negligible on these shapes (no notes in the dataset;
  tab-inheritance lookups only).
- "manager reconcile" is the manager's expected-set erase driven by
  `ChildrenReconciled` — emitted for every top-level stash reply, so
  each measured reply pays it.

## Remedy validation rerun (R6-6) — July 29, 2026

The selected pair implemented: `SourceKeyedItems`
(`src/sourcekeyeditems.h`) replaces the flat vector on BOTH sides of
the presentation lane. Buckets are keyed by `FetchSourceKey`; the
per-reply replacement is one bucket swap (O(replaced + delta)); the
reconcile and list-deletion erases walk the bucket index (O(sources)
plus the erased items), testing one representative location per
bucket — valid because every bucket is non-empty and homogeneous (one
fetch source). Whole-collection consumers read a lazily rebuilt flat
vector; nothing on the per-reply path calls it (the D9 fan-out was
verified to touch only the delta and the searches' dirty flags).

Same harness, datasets, shapes, seed, statistic, and environment as
the original measurement above; same commit for everything except the
remedy itself. One harness change: the erase micro-benchmarks became
replace micro-benchmarks — they time the production bucket op
(`ReplaceSource`) on store mirrors seeded from each side's
post-populate state, since the flat erase they used to time no longer
exists on the path.

### Rerun — 100k

Replacement replies (churn 0.3; 40 samples, median reply 48 items):

| Bucket | median | max |
|---|---|---|
| **whole path** | **0.234 ms** | 0.400 ms |
| pre (dispatch) | 0.013 ms | 0.026 ms |
| persistence | 0.054 ms | 0.089 ms |
| worker replace+parse | 0.067 ms | 0.208 ms |
| manager apply (replace+append+pricing) | 0.002 ms | 0.005 ms |
| UI intersection/fan-out (primary) | 0.001 ms | 0.001 ms |
| worker between (counters, status fan-out, ghost erase) | 0.043 ms | 0.063 ms |
| manager reconcile (expected-set erase) | 0.017 ms | 0.031 ms |
| UI intersection/fan-out (reconcile) | 0.001 ms | 0.001 ms |
| post (finish) | 0.021 ms | 0.026 ms |
| residual | 0.000 ms | 0.000 ms |

Micro-benchmarks (identical data, outside the windows):

| Component | median | max |
|---|---|---|
| worker replace (bucket op) | 0.001 ms | 0.005 ms |
| **manager replace (bucket op)** | **0.001 ms** | 0.002 ms |
| parse+append | 0.067 ms | 0.213 ms |
| pricing | 0.001 ms | 0.003 ms |

Removal replies (emptied source; 10 samples): whole path median
0.129 ms.

### Rerun — 1m

Replacement replies (churn 0.3; 12 samples, median reply 512 items):

| Bucket | median | max |
|---|---|---|
| **whole path** | **1.225 ms** | 1.474 ms |
| pre (dispatch) | 0.043 ms | 0.055 ms |
| persistence | 0.164 ms | 0.188 ms |
| worker replace+parse | 0.745 ms | 0.857 ms |
| manager apply (replace+append+pricing) | 0.014 ms | 0.021 ms |
| UI intersection/fan-out (primary) | 0.002 ms | 0.005 ms |
| worker between (counters, status fan-out, ghost erase) | 0.135 ms | 0.228 ms |
| manager reconcile (expected-set erase) | 0.060 ms | 0.076 ms |
| UI intersection/fan-out (reconcile) | 0.001 ms | 0.001 ms |
| post (finish) | 0.039 ms | 0.045 ms |
| residual | 0.000 ms | 0.000 ms |

Micro-benchmarks (identical data, outside the windows):

| Component | median | max |
|---|---|---|
| worker replace (bucket op) | 0.004 ms | 0.005 ms |
| **manager replace (bucket op)** | **0.002 ms** | 0.002 ms |
| parse+append | 0.739 ms | 0.841 ms |
| pricing | 0.011 ms | 0.017 ms |

Removal replies (emptied source; 6 samples): whole path median
0.209 ms.

Populate, for context only (same definition as above): 0.9 s at 100k
(was 5.9 s), 6.9 s at 1m (was 93.9 s) — the populate path paid the
same erase passes per reply, so the remedy collapses it too.

### Rerun budget verdicts

| Budget (D3) | 100k (< 2 ms) | 1m (< 16 ms) |
|---|---|---|
| Manager marginal replace | **PASS** — 0.001 ms (was 1.828) | **PASS** — 0.002 ms (was 18.830) |
| Whole path | **PASS** — 0.234 ms (was 7.382) | **PASS** — 1.225 ms (was 74.928) |

Every budget passes with more than an order of magnitude of headroom;
the max column stays inside budget everywhere. The dominant remaining
component at 1m is parse+append (~0.74 ms on a 512-item reply) —
genuine O(delta) work on the delta's own items, exactly what D3
permits the path to cost. The four erase passes are gone from the
attribution: worker between and manager reconcile are now bucket-index
walks (~0.04–0.14 ms at 1m), and the primary replaces are microseconds.
No further remedy is required; the M2-M2 conditional is discharged.
