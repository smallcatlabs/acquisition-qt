# M1-M2 measurement result (items-pipeline M2, D10; post-M2 follow-up)

Status: **MEASURED July 31, 2026 — the gate FIRED. The prescribed D10
coalesce was built the same day and validated by the rerun appended
at the bottom of this document** (2,000-entry burst 23.0 ms → 0.9 ms
with the dialog connected; the status widget's marginal cost is now
indistinguishable from measurement noise). A 2,000-entry
`QueueUpdated` burst blocked the UI thread for ~23 ms, over one 60 Hz
frame (16.7 ms), and ~95% of that was the status widget's
per-emission handler — which runs whether or not the dialog is
visible, because `MainWindow::InitializeRateLimitDialog` constructs
and connects it unconditionally at startup. Per D10 the conditional
is determinate: the fix is the UI-side coalesce of the existing
signal (D9's non-resetting single-shot throttle pattern, much smaller
S); the limiter is not touched (network-redesign is frozen).

This is the resolution the spec's open-items entry asks for (M1-M2,
`items-pipeline-m2.md`; deferred out of M2 implementation because it
blocks nothing, recorded at the M2 wrap-up as a post-M2 follow-up).

## What was measured

Network-redesign D6 left the batch-submit `QueueUpdated` burst to
"coalesce on the UI side if the status dialog measurably stutters";
M2's D10 froze that as a measurement gate. The production shape being
reproduced: the worker's batch submit queues one request per tab in a
single loop turn; every `Enqueue` on the draining pump emits
`QueueUpdated` synchronously (`ratelimitmanager.cpp`), which fans
through the hub's re-emit (`RateLimiter::OnQueueUpdated` →
`QueueUpdate`) into `RateLimitDialog::OnQueueUpdate` — all direct
connections on the UI thread. The measured unit is the wall time of
the whole submission loop, which is exactly how long the UI thread is
blocked, since the burst completes inside one loop turn.

The harness (`tests/m1m2_benchmark.cpp`, run by hand, offscreen, in a
Release build) drives the real stack: a real `RateLimiter` over the
offline `FakeNetworkManager`, the endpoint established through the
real HEAD-probe path, and the real `RateLimitManager` pump suspended
in its pacing sleep on the `FakeScheduler` — never advanced during
the burst, so every enqueue takes the draining branch and emits, and
nothing is ever sent. Three receiver configurations attribute the
cost: dialog shown (user watching the status window), dialog hidden
(the production default), and no dialog (hub cost alone). Each
configuration runs one untimed verification rep — emission count
observed by a counting connection, end-to-end delivery checked
against the dialog's queue cell — followed by the timed reps, which
are unobserved.

Excluded, deliberately: the worker's per-future continuation attach
(worker-side cost, M2-M2's territory), and any real send or reply
handling (the gate never grants during the burst).

## Environment

- Hardware: Apple M4, 32 GB RAM (macMini-class), macOS 26.6
- Qt 6.11.1 (macos), Release build (`build-release/`), offscreen
  platform plugin
- spdlog level `info` (hot-path traces level-gated out, as in
  production)
- Measured tree: the harness commit `0819ecd3` (parent: the
  `items-pipeline-m3` S8 head `63980eb8`); both measurement commits
  were folded into `items-pipeline-m3`
- Command: `./m1m2_benchmark -platform offscreen [--entries N]`;
  medians over 7 timed reps (3 at 8,000)

## Result (pre-remedy)

Burst wall time, median (min–max), milliseconds:

| entries | no dialog (baseline) | dialog hidden (default) | dialog shown |
|--------:|---------------------:|------------------------:|-------------:|
| 2,000 | 1.027 (0.908–1.234) | 23.219 (22.790–23.310) | 23.015 (21.999–23.622) |
| 2,600 | 1.214 (1.114–1.281) | 29.339 (29.194–30.157) | 29.437 (28.957–29.689) |
| 8,000 | 3.543 (3.498–3.669) | 91.609 (90.690–92.852) | 92.959 (92.907–93.039) |

- **The status widget is ~95% of the burst**: ~22.0 ms marginal at
  2,000 entries (23.0 ms with the dialog connected vs 1.0 ms hub-only
  baseline), ~11.5 µs per emission, linear across all three sizes.
- **Hidden costs the same as shown** (23.2 vs 23.0 ms): the cost is
  the per-emission `QTreeWidgetItem::setText` traffic through the
  item/model machinery, not painting. Every large refresh start pays
  it even with no status window open.
- **The post-burst settle pass is trivial** (~0.15 ms in every
  configuration): Qt already coalesces the repaints into one frame.
  The synchronous handler is the entire problem — exactly the fix
  boundary D10 predicted ("coalesce on the UI side").
- The hub-only baseline (~0.5 µs/emission) prices the submission loop
  itself: request construction, promise/future pair, enqueue, and the
  two-hop signal fan-out with no widget receiver.

## Verdict

At the driving 2,000-tab scale the burst blocks the UI thread for
~23 ms — more than a full 60 Hz frame (and 2.8 frames at 120 Hz) in
one synchronous stall at every large-refresh batch submit, in the
default configuration. The D10 gate fires: build the UI-side
coalesce.

## Remedy (D10, prescribed)

`RateLimitDialog::OnQueueUpdate` parks the latest value per policy in
a map and returns; a non-resetting single-shot flush timer
(S = 100 ms, started only when idle) applies the whole batch in one
pass, looking rows up by name at flush time (a `PolicyUpdate` row
rebuild between arrival and flush would dangle a captured row
pointer). A single-loop-turn burst of any size becomes one apply, the
displayed queue depth is never more than S behind the real queue, and
steady-state arrivals (one per reply, seconds apart) each flush on
their own expired window as before. The hub's pre-establishment
parked-path `QueueUpdate` emissions share the same handler and are
coalesced by the same mechanism. Pins: `tst_ratelimitdialog`
(`burstCoalescesToOneApply`, `flushAppliesLatestValuePerPolicy`,
`rowRebuildBetweenArrivalAndFlushStillLands`).

## Caveats

- Offscreen platform: window-server painting is not measured. This
  cannot hide a miss — the measured stall is synchronous handler
  time, platform-independent, and painting was already coalesced to
  one frame per burst (the settle row).
- One policy row in the tree; a real session holds a handful. The
  row scan is O(rows) per emission pre-remedy, so more rows only
  worsen the miss; post-remedy it is O(rows) once per flush.
- The 60 Hz frame (16.7 ms) is the reference budget; on ProMotion
  displays the frame is 8.3 ms and the pre-remedy miss is
  proportionally worse.

## Rerun with the coalesce in place (July 31, 2026)

Same harness, same environment, same reps; the harness's end-to-end
verification now polls for the flush instead of asserting
synchronously (untimed either way). Burst wall time, median
(min–max), milliseconds:

| entries | no dialog (baseline) | dialog hidden (default) | dialog shown |
|--------:|---------------------:|------------------------:|-------------:|
| 2,000 | 1.225 (1.118–1.516) | 0.907 (0.901–0.925) | 0.919 (0.906–0.966) |
| 8,000 | 3.517 (3.510–3.749) | 3.627 (3.620–3.705) | 3.609 (3.576–3.649) |

The three configurations are now statistically indistinguishable
(~0.45 µs/emission; the dialog medians landing slightly below the
baseline at 2,000 is run-to-run noise). The status widget's marginal
cost of a 2,000-entry burst went from ~22 ms to less than the
run-to-run spread; the whole burst — hub, submission loop, and
status widget — sits at ~0.9 ms, more than a frame of headroom at
any refresh rate. The displayed queue depth lags the real queue by
at most the 100 ms flush interval. The D10 conditional is
discharged.
