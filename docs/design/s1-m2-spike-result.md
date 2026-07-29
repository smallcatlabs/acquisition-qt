# S1-M2 spike result (input to items-pipeline-m2 revision 9)

Status: **FINAL** — measured July 29, 2026; hands-on judgment made by
Tom the same day. Result: **outcome (a), S confirmed at 60 s.** Ready
to be recorded in spec revision 9.

This document lives on the throwaway branch `spike/s1-m2-throttle` and
is the written result the spec's "Open items requiring spike or
measurement" list asks for. Revision 9 of
`docs/design/items-pipeline-m2.md` (branch `items-pipeline-m2-spec`)
records the outcome and freezes; this branch is never merged
(R3-4/R7-3).

## What was prototyped

Both halves the spec requires judged together (D9, R6-3):

1. **The D9 five-rule state machine** in `MainWindow`, against the real
   widgets: per-search items-dirty flags cleared only by that search's
   own successful refilter and consumed by the extended
   activation-refilter gate (rule 1/4); an intersection-gated,
   **non-resetting** trailing throttle owned by the current search with
   S injectable at runtime (rule 2; provisional S = 60 s); refilter on
   fire with capture-before-reset (rule 3); cancellation by any
   refresh of the shown view — user refilter (R5-5), tab switch
   (rule 4), final snapshot (rule 5). Intersection implements **both
   halves**: any delta item passing the current filter set, plus the
   removal half via a per-refilter set of visible fetch-source keys.
2. **The R6-3 restore-fidelity contract**: expansion keyed by stable
   `(type, id)` (`ItemLocation::stableKey()`) instead of header text;
   reselection by stable item identity (GGG item id) instead of
   `shared_ptr` identity, with the details panel adopting the
   replacement object; scroll preserved across the throttled reset by
   a stable top-row anchor (bucket key + item id) with the raw
   scrollbar value as fallback; all three captured immediately before
   every throttled reset.

The delta producer is simulated: `ItemsManager::SpikeApplyTabDelta`
replaces one fetch source's items in the collection and emits the
prototype `TabRefreshed(location, delta_items)`. D7 per-delta pricing
is out of the spike's scope (not part of what is being judged).

## Harness

`tests/spike_s1m2_harness` (built by the normal CMake tree; not a
ctest). It constructs the real `MainWindow` offline on the
`tst_mainwindow` fixture substrate, generates a deterministic
collection (`tests/spikedataset.h` — seedable; **M2-M2 reuses these
datasets** as its fixed reply/removal shapes), applies the initial
full refresh, then streams churned per-tab deltas.

Presets: `--preset smoke` (50 tabs / ~1k items), `--preset 100k`
(2,000 tabs / ~100k items), `--preset 1m` (2,000 tabs / ~1m items);
`--tabs/--mean/--seed` override. `--auto` runs an unattended
delta-stream smoke (S = 5 s, 1 delta/s, 35 s) and exits nonzero if
fewer than two throttled resets applied.

Control panel (separate window beside the main one):

- **Throttle period S** — live spinbox (persisted to the harness's
  temp settings), so cadence can be compared at 60 s / 30 s / 120 s in
  one session.
- **Delta arrival interval** (default 20 s — the rate-limited
  ~1 tab/20 s reality) and **per-tab churn %** (default 30%).
- **Start/pause stream**, **inject one delta now**.
- **Rename a random tab (delta)** — drives the expansion-key half of
  the fidelity contract: under header-text keys a rename orphans
  expansion; under stable keys it must survive.
- **Simulate terminal failure** — the stream stops with no final
  emit; the pending tick must survive and fire (R5-3).
- **Final snapshot** — the full `ItemsRefreshed` path (rule 5).
- Live readout: pending-tick countdown, deltas applied, throttled
  reset count, last/avg/max reset latency with filter vs. restore
  split.

Every throttled reset logs and displays:
`total_ms` (capture → scroll restored), `filter_ms`
(filter loop + model reset inside `FilterItems`), `restore_ms`
(everything else: expansion restore, reselection, scroll), plus
visible item and bucket counts.

## Measured results (Release build, offscreen, July 29 2026)

> Machine: Tom's Apple Silicon Mac (Darwin 25.6.0), Release
> (`build-s1m2`, `-DCMAKE_BUILD_TYPE=Release`), unfiltered current
> search (worst case: every reset refilters and re-sorts the whole
> collection), default name-column sort, `--auto` stream (5 resets at
> 100k, 3 at 1m; values were stable across resets, ±3%).

| Preset | Items / buckets | Reset total | filter+model | re-sort | expansion restore |
|---|---|---|---|---|---|
| 100k (2,000 tabs) | 101,514 / 2,000 | **~455 ms** | ~60-67 ms | ~390-395 ms | ~0 ms |
| 1m (2,600 tabs, 80% quad) | 975,711 / 2,600 | **~5,370 ms** | ~305-327 ms | ~5,040-5,076 ms | ~0 ms |

Startup for reference: 100k materializes in ~0.3 s with the initial
full refresh at ~0.45 s; 1m materializes in ~2.1 s, initial refresh
~5.5 s.

**The dominant cost is not the filter or the model reset — it is the
whole-model re-sort** that `setSortingEnabled(true)` triggers after
every reset (`ItemsModel::sort` → `Search::Sort` over every bucket,
because `FilterItems` rebuilds all buckets and sortedness is lost).
The R6-3 fidelity machinery itself is essentially free at both scales
(expansion restore ~0 ms; stable-key save, stable-id reselect, and the
scroll anchor are all sub-millisecond against these totals).

Caveats stated honestly:

- Offscreen numbers exclude post-reset paint; the hands-on run judges
  felt jank. Paint is expected to be small against the 1m sort cost
  and possibly noticeable against the 100k one.
- The unfiltered whole-collection search is the worst case. A filtered
  search re-sorts only the filtered subset, so typical working
  cadence (a name filter active) is far cheaper — judge both by hand.
- The refilter being O(whole collection) is by design (M2 keeps the
  reset-based application until M3); S1-M2 records the number, M2-M2
  owns the budgeted measurement on the production path.

## What Tom should drive by hand (the judgment that is his)

Build (Release — the debug build roughly doubles the reset numbers):

```
cmake -S . -B build-s1m2 -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/Users/tom/Qt/6.11.1/macos
cmake --build build-s1m2 --target spike_s1m2_harness -j 8
```

Run: `./build-s1m2/tests/spike_s1m2_harness --preset 100k`
(add `--preset 1m` for the scale check; startup takes ~8 s there).

1. **Steady-state cadence (the core question).** Expand a few tabs,
   select an item mid-list, scroll somewhere deliberate. Start the
   stream (20 s arrivals, S = 60 s) and just *use* the window —
   scroll, browse tooltips — for several S periods. The question:
   is one reset-plus-restore per minute under your feet acceptable,
   i.e. do you notice the reset mostly not at all?
2. **Fidelity under the reset.** At each tick: does your expansion
   set survive? Does your selected item stay selected (its row may
   legitimately move if its tab sorted differently)? Does the top of
   the viewport stay anchored? Selection loss is only legitimate when
   the item was churned away.
3. **Rename survival.** Expand a few tabs, hit "Rename a random tab"
   a few times, wait for the tick: renamed tabs must keep their
   expansion state (this is the case the old header-text keys lose).
4. **Sorted-column stress.** Sort by a column (e.g. name), then let
   ticks land: reselection/scroll anchoring under re-sorted buckets is
   the hardest fidelity case — judge whether the view still feels
   stable.
5. **Filtered search.** Type a name filter (e.g. "Blazing"), let the
   stream run: ticks should only arm when a delta intersects (watch
   the pending label); non-intersecting deltas must not reset the
   view.
6. **Terminal failure.** Arm a tick (inject a delta), hit "Simulate
   terminal failure": the pending tick must still fire once (R5-3),
   then nothing further until user action.
7. **S tuning.** If 60 s feels wrong in either direction, move the
   spinbox (30/90/120) and repeat (1) — the tuned value is what
   revision 9 records under outcome (a).

## What the measurements imply (for Tom to confirm or overrule)

- **At 100k, outcome (a) looks viable.** ~0.46 s once per S = 60 s is
  a sub-half-second hitch a few times a refresh hour; with fidelity
  in place there is no state loss, only the pause. Whether that pause
  is *felt* as a hitch under the hands (especially mid-scroll) is
  exactly the subjective half the spike can't decide.
- **At ~1m unfiltered, outcome (a) at S = 60 s is not defensible on
  the numbers alone**: a ~5.4 s synchronous freeze per minute is an
  unusable steady state. If the hands-on run confirms it, the options
  the spec already anticipates are: tune S up sharply for such
  collections, or take outcome (b) — and note the freeze recurs at
  every *user-initiated* refilter at that scale too, throttle or not,
  so (b) does not make a 1m unfiltered search pleasant; it only makes
  the pauses user-chosen.
- **Attribution points at the re-sort, not the throttle.** If M2
  wanted the 1m case under outcome (a), the lever is making the
  reset's re-sort cheaper (e.g. filtering from a pre-sorted master
  list so buckets are born sorted), not the state machine — that is
  a design argument for revision 9 to record as considered-or-
  deferred, not something this spike is chartered to decide.

## Result for revision 9 (Tom's call, July 29, 2026)

- **Outcome (a). S confirmed at 60 s.** The steady-state
  reset-plus-restore cadence at the driving scale (~100k items /
  2,000 tabs) is acceptable with the R6-3 fidelity contract in place.
- Reset latency at 100k / 1m: recorded above (Release, offscreen,
  worst-case unfiltered search; re-sort dominates at ~86% / ~94%).
- Fidelity contract verdict: **holds.** Expansion, selection, and
  scroll survive ticks, including tab renames (stable keys) and
  sorted-column stress; mechanically pinned by `tst_spike_s1m2` and
  the suite passing with the stable-key/stable-id machinery in place.
- Hands-on observation worth carrying into implementation: a rename
  into an active Tab filter surfaces only at the next tick (bounded
  by S, verified by test) — the S-window staleness is *felt* most on
  discrete user-watched events. Judged acceptable at S = 60 s.
- For revision 9 to note: at ~1m unfiltered the reset is ~5.4 s and
  outcome (a)'s constants are judged at the driving scale, not there;
  the dominant cost is the post-reset whole-model re-sort (a
  born-sorted-buckets change is a possible lever, deliberately not
  decided here; M3's bucket-scoped ops are the real retirement). The
  fidelity machinery itself costs ~0 ms at either scale.
