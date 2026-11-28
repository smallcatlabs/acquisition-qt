# Network Redesign — Review History

**Companion to `network-redesign.md`** (split out of that file's
Appendix B on July 19, 2026, at revision 5; the appendix's earlier
evolution is in the spec file's git history). This file preserves the
decision history for the network redesign: the review-round finding
tables (ER, IR, R4-\* through R7, S1, S2), the round narratives, and the
revision log. The spec cites these IDs inline and records only current
decisions; new review rounds and revision-log entries append here,
with the spec's status line updated in the same commit. Where a later
round superseded an earlier resolution, both are recorded. Git
history holds the full text of superseded designs.

## Open items from the first review round — resolved July 18, 2026

1. Gate constants: **blessed as spec'd** — in-flight cap 2, spacing
   floor 250 ms, retry cap 3. Named constants, provisional until
   capture data says otherwise; the revisit trigger for all three is
   capture evidence (lane idling at the gate, spike-correlated
   anomalies, retry exhaustion in the wild).
2. Degraded-HEAD behavior: **degrade and proceed** — decided; folded
   into D4. *(Rescinded July 19, 2026: the decision predated the
   containment principle; re-examined under it, Tom chose clean
   failure — see D4. The Discovery mechanism built for it is
   deleted.)*
3. `cancelChain()` semantics: verified against Qt docs (available
   since Qt 6.10, backwards propagation, nested-future limitation) —
   then **superseded the same day** by the external-review revision:
   the design no longer uses QFuture cancellation at all. See D2
   (stop_token; futures always finish) and the revision log.
4. Naming: **deferred to phase 3 by design.** `RateLimiter` keeps its
   name (app-facing surface; continuity with this research corpus).
   `RateLimitManager`'s name is decided when phase 3 rewrites the file
   anyway — rename is free at that moment and never again; proposals
   welcome then. Naming care goes to the new primitives (the gate, the
   stop-interruptible sleep), which have no history and will be read
   forever.

## External review backlog

The July 18 external review is recorded here so its identifiers and
scope do not depend on a chat transcript. `ER` means external-review
finding and is deliberately distinct from the cleanup register's
permanent `F` numbers.

| ID | Group | Finding | Status |
|---|---|---|---|
| ER1 | Cancellation and lifetime | Canceling a `QFuture` already awaited by a sibling coroutine makes QCoro call `result()` before a post-await liveness guard can run. | Resolved in D1/D2: stop tokens replace QFuture cancellation and every future finishes with a value. |
| ER2 | Degraded HEAD and topology | A provisional pump does not guarantee exactly one unpaced request; multiple provisional/established pumps can overlap, and queue-only merging loses history and ordering. | Resolved in D4: explicit endpoint state machine; no provisional pump exists. (July 19: the Discovery state that resolution introduced was itself deleted — setup now fails cleanly instead of degrading through a discovery GET; the no-two-pumps property stands.) |
| ER3 | Gate ownership and scope | A hub-owned gate cannot cover pre-session login/OAuth traffic; the literal host wildcard also includes legacy CDN image traffic, and the API must accommodate GET/HEAD/POST. | Resolved July 19 in D5 by shrinking the scope: pre-session login/OAuth traffic is a documented exclusion (N2 magnitude rationale); CDN hosts are outside. (IR round, same day: forum traffic also excluded — the gate is fully internal to the hub, no public permit API.) |
| ER4 | Cancellation and lifetime | Cancellation checkpoints, interruptible waits, top-level task ownership, and teardown were unspecified; a stopped request could still send or a coroutine could resume through a destroyed owner. | Resolved in D2/D3 and the shutdown/task-ownership section (IR3 later added the hub shutdown stop_source the teardown sequence was missing; R5-2 later deleted that stop_source along with the teardown choreography itself). |
| ER5 | Cancellation and lifetime | Retaining every facade future for abort can retain completed parsed payloads for a multi-hour update. | Resolved in D2: the stop source is the abort handle and no future registry exists. |
| ER6 | Degraded HEAD and topology | Header validity and policy-name/topology changes are not modeled; the current parser assumes structurally valid triplets and current hub maps can remain keyed by an old policy name. | Resolved in D8 (verification found the missing-header path is UB in today's parser, N20 corrected) and D4/D8 (July 19: rename handling simplified; hub maps cannot go stale because a pump's name never changes; IR1 made steady-state anomalies strict `Protocol` errors). |
| ER7 | Degraded HEAD and topology | Retry timing is underspecified: the applicable bucket depends on Q4, the attempt count is ambiguous, and the retry deadline should be reconciled with policy and gate deadlines. | Resolved in D3: deadline = max(padded Retry-After, GetNextSafeSend) through the gate; unconditional 60 s pad (round 2) fully decouples Q4; Retry-After strict-accepted in [1, 900]; 3 = total sends. (IR5 later fixed the exhausted-attempt sleep and violation-ordering holes and made the attempt table normative.) |
| ER8 | Degraded HEAD and topology | `FetchError` needs precise HTTP-vs-network precedence and a protocol/header failure that can represent a successful HTTP response with unusable rate-limit headers. | Resolved in D1/D8: status-first precedence, `Protocol` kind, full field inventory. (IR round added `Internal`.) |
| ER9 | Cancellation and lifetime | QCoro 0.11 was stale; 0.13 contains directly relevant QFuture-destruction and FetchContent fixes, and C++23 alone does not enforce the claimed compiler floor. | Resolved in the spec's dependency section. |

**Round 2 (July 18, group 2 re-review):** nine corrections to the
group-2 resolutions, all verified and adopted at the time — evidence
is endpoint-scoped, setup traffic is gate-exclusive, the
discovery-429 split, transport-error-over-2xx precedence, the
unconditional 60 s retry pad, the setup-failure cooldown, reset-scope
limits, and two documentation fixes. Of these, the retry pad, the
cooldown, the precedence rule, and established-keeps-topology survive
in the spec; the discovery and per-endpoint-counter machinery was
deleted July 19.

**Round 3 (July 19, group 2 re-re-review):** five blockers and four
smaller gaps, all resolved — the substantive outcome was the
**containment principle** (see the spec's design-principle section).
Of the round-3 resolutions, the HEAD-429 definition and the
cancellation-is-not-failure cooldown rule survive; the rename
containment path, the name-check-before-mutation ordering rule, and
the discovery-seeding rules were deleted in the July 19 simplification
(subsumed by the single policy-update rule in D3/D8 and the removal of
Discovery).

**Group 4 — complexity and testing: resolved July 19, 2026.** The
simplification pass *was* the complexity review, conducted with Tom
against the containment principle. Outcomes: the Discovery state
deleted (degraded HEAD = clean setup failure — Tom's explicit call:
"I'd rather have an error … if that allows us to simplify"); the
steady-state `Protocol` threshold, per-endpoint counters, and
queue-extraction primitive deleted; rename handling reduced to
detection; the `NameOnly` tier deleted; the gate's scope shrunk (also
resolving ER3); the injected clock/scheduler and integration coverage
folded into the spec's testing plan. *(The pass initially retained the
awaitable FIFO; the implementation-readiness review below reversed
that hours later — deque + on-demand drain — and corrected the pass's
log-and-continue rule for steady-state anomalies, IR1.)*

## Implementation-readiness review — July 19, 2026 (Tom)

After the simplification pass, Tom reviewed the spec line-by-line
against the code and the Qt/QCoro documentation: six findings, three
further simplifications, and four pre-implementation investigations —
all verified and adopted in revision 3.

| ID | Finding | Resolution |
|---|---|---|
| IR1 | "Pace on the last-known policy" was unsound: `GetNextSafeSend` is state-driven (`ratelimitpolicy.cpp:274` returns *now* whenever the stored status is OK), so frozen-OK state means unpaced sends until 429 — repeatedly, since the retry then succeeds degraded. | D8: a steady-state parse failure or name mismatch records history + capture and completes `Protocol` immediately; the worker's first-failure stop ends the update; the endpoint stays Established and self-heals. Simpler than the rule it replaces. |
| IR2 | A stop token is not an update-generation barrier by itself: a future can succeed just before `request_stop()` and its consumer resumes after; Qt runs `.then` continuations synchronously on already-finished futures. | D6: post-await identity invariant and initialize-before-launch invariant, both pinned (ready-success/ready-error futures; late-resuming successful straggler). Generation tag stays deleted. |
| IR3 | The teardown sequence could not reach a pump suspended in a pacing/retry sleep or gate wait (queue closure only wakes a dequeue), and an aborted reply's entry could still retry during shutdown; worker task ownership was vague. | Shutdown rewritten: hub-owned shutdown stop_source observed at every wait and checkpoint; explicit completion inventory (active entries, deque remainders, parked setup entries, HEAD probes, gate waiters); task handles are owned members with destruction order verified by the spike. *(Superseded by R5-2: the stop_source and choreography were deleted; shutdown is destruction order.)* |
| IR4 | Always-finish was incomplete under exceptions: a dead pump loop wedges queued promises behind a dead consumer, and a destroyed `QPromise` notifies its future without a result — the exact resultless state D2 forbids. | D1 `Internal` kind; scoped completion guard on every entry; terminal pump-failed state that fail-fasts later submissions; facade parse continuations exception-tight (D7). *(R5-1 later moved the supervision itself into root-coroutine catch-alls.)* |
| IR5 | Retry-loop holes: an exhausted attempt slept up to ~961 s before reporting; violation recording was inconsistent (non-retryable and stopped 429s uncounted); Full-HEAD-429-without-valid-Retry-After was undefined; D3/D4 disagreed on the buffer term. | D3: normative attempt table; bookkeeping (history, capture, violation) always precedes stop/retry decisions; exhaustion completes immediately. D4: Full HEAD 429 without valid Retry-After is a setup failure. Buffer term unified. |
| IR6 | Gate contract underspecified: a permit released at dispatch makes the cap meaningless; without writer preference a busy pump starves HEAD setup; completing a promise before releasing the permit invites synchronous re-entrancy. | D5: permit span = acquisition through reply-finished; HEAD writer preference; release-permit-and-stabilize-before-completing rule (restated in D3). |

**Simplifications adopted:** the awaitable FIFO deleted — deque +
on-demand drain coroutine (no permanently suspended task, no
queue-close semantics; the earlier "the pump is built on it"
retention rationale was circular, and the genuinely required
primitive was the stop-interruptible sleep all along, since QCoro's
sleeps take no stop token); forum traffic removed from the gate
(`Shop` is strictly sequential with deliberate delays, `shop.cpp:388`
— the login-traffic magnitude argument applies identically, and the
gate becomes fully internal with no public API); validation and
parsing unified into a single total `parse(headers) → expected`
(the same grammar implemented twice would drift; arithmetic
untouched).

**Pre-implementation investigations:** the phase-0 QCoro spike (task
destruction, ready futures, `result()` vs `takeResult()`, synchronous
completion re-entrancy, stopped waits); the 2,000-tab batch-scale
measurement (memory and abort cost — measure before adding flow
control); QCoro FetchContent example/test flags. All folded into the
spec's phasing sketch.

**Standing boundary (restated from the review):** no further design
effort on policy-rename recovery, degraded-HEAD discovery, endpoint
migration, reprioritization, or gate-constant tuning. Detecting those
conditions and failing cleanly is the right level until real evidence
arrives.

## Review round 4 — July 19, 2026

A post-acceptance review of revision 3 (design bugs, inconsistencies,
gaps, complexity; conducted by Claude, adopted by Tom). No design
decision was reopened; four ambiguities were pinned and six minor
items fixed in place. The complexity pass came up empty: everything
remaining (writer preference, the setup cooldown, the terminal
pump-failed state, the completion guard) carries observed weight, and
nothing in the spec designs choreography for an unobserved event.

| ID | Finding | Resolution |
|---|---|---|
| R4-1 | D3's pseudocode read as holding the gate permit across a retry sleep (up to ~961 s) — one retrying entry plus a waiting HEAD under writer preference would stall the whole hub: a gate-shaped F57. | D3/D5: permits release at reply-finish and are never held across a retry sleep; each attempt re-acquires the gate. Pseudocode corrected; pump and gate pins added. |
| R4-2 | Resumption discipline was unspecified: `std::stop_callback` runs synchronously inside `request_stop()`, which the worker calls from a completion continuation — an unbounded synchronous cascade; and the teardown sequence presumed a wait-for-drains mechanism it never named. | D2: stop wakeups resume via the event loop; `request_stop()` returns before any coroutine resumes. Shutdown: teardown needs a live event loop; the concrete await mechanism is a phase-0 spike deliverable. *(The teardown half was superseded by R5-2; the queued-resumption rule survives.)* |
| R4-3 | Worker task topology was unspecified: where per-fetch task handles live, who owns them, how "update done" is detected — with apparent tension against D2's no-retention rule at the 2,000-tab scale. | D6: handles are owned members released at update end; a handle owns a frame, not a payload (no conflict with D2); update completion is counter reconciliation, as today. *(R5-1 later replaced release-at-update-end with the deferred sweep and abort-never-destroys-live-handles.)* |
| R4-4 | Gate liveness silently depends on the 10 s transfer timeout (permit turnover, HEAD acquirability, stopped-entry drain under never-abort) — stated only in the ground-truth appendix. | D5: the timeout is an explicit invariant of the gate design. *(R5-3 corrected the premise: the timeout was not existing behavior for the legacy endpoint; the facade establishes it.)* |

Minor items, fixed in place: HEAD 429s record their violation (D4); a
probe joining an existing pump updates its policy under the normal
rule (D4); the `Http`/`RateLimited` split for 429s is documented as
deliberate (D8); the D5 exclusion rationale no longer claims all
OAuth traffic is pre-session (token refresh is mid-session; the
magnitude argument is unchanged); the main-thread invariant is stated
explicitly (D7); batch-submit `QueueUpdated` volume is noted as a
UI-side concern (D6).

## Review round 5 — July 19, 2026 (Tom)

Tom's implications review of revision 4: not new edge cases, but
whether the accumulated design fits the code it must live in. Four
blockers, each verified against the code; two resolutions are net
deletions. One framing correction recorded: R4-2/R4-3 patched the
shutdown story instead of asking whether an asynchronous shutdown
should exist at all — R5-2 is the containment answer that question
deserved.

| ID | Finding | Resolution |
|---|---|---|
| R5-1 | Owned task handles do not supervise exceptions: QCoro rethrows a stored exception only to an awaiting consumer, so an escaped drain never runs the terminal-state transition and an escaped per-fetch task never counts its completion — a wedge with the exception sitting silently in the handle. Also: rev 4's release-on-abort rule contradicted D2's stragglers-resolve-later contract, and finalization triggered by the last completion could destroy the frame it runs in. | Exception policy rewritten: no exception ever escapes a root coroutine — every root task catch-alls and finishes non-exceptionally; the drain's catch implements the terminal state; a per-fetch catch feeds the first-failure path. Handles are lifetime-only; deferred sweep outside any completing coroutine; abort never destroys live handles. Spike + pump/worker pins. *(Rev. 11 later reconciled the spec text: the realized first-failure path is the counter-independent `AbortUpdate()`, which does not count the throwing fetch — counting a failure was functionally dead and drove status non-monotonic.)* |
| R5-2 | The rev-4 shutdown required queued resumptions and a live event loop — but `Application` is destroyed after `a.exec()` returns (`src/main.cpp`) and `UserSession` destroys `Shop`/`ItemsManagerWorker` before `RateLimiter` (`src/application.h`): the hub-driven teardown was unreachable at the only moment it would run. | Teardown choreography and the hub shutdown stop_source deleted. Shutdown is destruction order — which `Application` already implements: consumers first, task handles destroyed while suspended, hub aborts replies after all awaiters are gone. Every-future-finishes is scoped to live sessions (safe: no awaiter survives to observe a broken promise). Every wait takes one token. Destroy-while-suspended becomes a load-bearing spike deliverable. *(R6-1 later deleted the shutdown abort itself — the hub aborts and tracks nothing.)* |
| R5-3 | D5's timeout-liveness premise was false for the legacy endpoint: `Shop::UpdateStashIndex` builds its request with no transfer timeout (only the OAuth builders set one) — a stalled legacy request could hold a permit indefinitely; with a HEAD waiting under writer preference, the whole hub stops. | D5/D7: the facade establishes the timeout invariant on every request it builds (all five), test over every builder. The live pre-existing gap is recorded as F60 in the findings register. |
| R5-4 | Raw `QNetworkReply` ownership was never assigned — `NetworkManager` does not enable auto-deletion (default false), and today's callers double-`deleteLater`: the contradictory-ownership mistake that motivated the redesign (F59), re-created one level down. | D3: after dispatch the pump (or setup path, for HEADs) solely owns the reply via RAII with a `deleteLater` deleter; body and headers consumed before any completion or retry decision; the permit observes `finished` but never owns. Leak and double-deletion pins. *(R6-1 corrected the install point: at dispatch, not at await-return — rev 5's rule left the in-flight span ownerless.)* |

## Review round 6 — July 19, 2026 (Tom)

Tom's third implications round, asked with the explicit question
"are we over-specifying?" Assessment: no — R6-1 and R6-3 are
contradictions between existing normative sections (an implementer
would have to guess which sentence wins), and R6-2 is in scope only
because R5-2 made destruction order the shutdown mechanism, which
turned participant lifetimes into spec content. Two of the three
resolutions delete machinery. **Convergence declared: the spec is
frozen for implementation after this round** — further findings are
filed against phase-0/harness evidence, not re-readings.

| ID | Finding | Resolution |
|---|---|---|
| R6-1 | Reply ownership was still contradictory: the RAII wrapper installed at await-return left the in-flight span — the reply's longest phase — ownerless; shutdown step 2 had the hub abort replies whose pointers lived only in already-destroyed frames (an implied registry the spec denied having); and `deleteLater` never runs once the event loop has stopped, so the claimed shutdown cleanup mechanism was wrong. | Wrapper installed at dispatch, before the first await — the owner is the pump frame at every instant, including suspension (destroying a suspended coroutine runs its locals' destructors; spike-verified, not assumed). The shutdown abort step is deleted: nothing aborted, nothing tracked; `abort()` is now never called anywhere. Post-loop `deleteLater` is inert; the `QNetworkAccessManager` parent (destroyed after the hub) is the documented backstop. *(The locals'-destructors premise was falsified by the spike — handle destruction detaches the frame and the wrapper never runs at shutdown; QNAM parent cleanup is the sole shutdown mechanism. The dispatch-time wrapper survives as the live-session owner. S1-1/S1-3.)* |
| R6-2 | Shutdown step 1 said `Shop` destroys "its task handles," but Shop has no tasks in this design; and `PoeApiClient` had no specified owner. | `Shop` stays callback-style: a context-bound `.then(this, …)` continuation that Qt discards on context destruction — the future equivalent of today's `this`-context connection (`shop.cpp:199`). `PoeApiClient` is a `UserSession` member declared after `rate_limiter` (destroyed after all consumers, before the limiter); its parse continuations capture values only, never the facade. |
| R6-3 | Policy mutation and response classification disagreed: D3 updated the policy from any Full matching landed reply, while D8 reached the total parse only on clean 2xx — leaving 429s, 403/500s, and 2xx-plus-transport-error ambiguous for pacing state, which would drift stale across non-2xx runs (today's code updates on every headered reply; N25: the server counts every exchange). | Observation separated from classification (D3/D8): header parsing is attempted independently on every landed response and Full + matching updates pacing; classification stays status/network-driven and decides only the caller's outcome; missing/malformed headers become `Protocol` only where the reply would otherwise be a clean 2xx success. |

## Errata batch (round 7) — July 19, 2026 (Tom)

Eight small corrections, adopted after the freeze under an explicit
test: each either corrects a statement that was factually false
(RFC 9110, Qt documentation, stale design references) or completes a
contract the harness would otherwise pin by accident. All eight
shrink or correct; none adds mechanism. The freeze holds, with the
bar restated: a further paper finding must show a false statement or
a missing harness-needed contract — "could be crisper" does not
qualify.

- **Gate fairness:** ordinary permits grant in arrival order (FIFO,
  no bypass) — a hot pump repeatedly beating a waiting one is lane
  starvation in miniature (F56-adjacent); the HEAD writer preference
  is the only queue-jump. Primitive-suite pin added (D5, testing §4).
- **Full grammar tightened:** nonempty policy/rule names, at least
  one rule with at least one item, limit hits > 0 (state hits ≥ 0
  stays — they start at zero, N24). Dead
  `RateLimitPolicy::EstimateDuration` (zero callers; contains the
  one reachable `/ max_hits` divide) added to the deletion list
  (D8, deletions, testing §3).
- **Retry-After reframed:** the grammar accepts any nonnegative
  integer — RFC 9110 delay-seconds permits 0 and arbitrary lengths,
  and 0 costs nothing (the pad dominates); the 900 s bound stays as
  explicit product policy (`RETRY_AFTER_CAP_SECS`), not validity —
  the old "stealth F57" rationale died when retry sleeps became
  visible, stop-interruptible, and permit-free (D3, D8, testing §2).
- **Unified terminal 429:** the R4-documented `Http{429}` /
  `RateLimited` split is deleted; every terminal 429 is
  `RateLimited`, carrying status, attempt count, and Retry-After
  acceptability (D1, D3 attempt table, D8).
- **Stale reprioritization claim removed:** "cheap later via
  cancel+resubmit" predated the per-update token — per-entry
  cancellation does not exist; reprioritization would need a new
  mechanism, still out of scope (D6).
- **Facade genuinely stateless:** the `QSettings` reference is
  dropped — account/realm/league are call parameters (D7).
- **Timeout wording corrected:** Qt's transfer timeout is an
  *inactivity* bound, not a total-duration bound; it bounds the
  observed stall class, and a byte-trickling reply is accepted
  rather than designed against (D5).
- **"Empirically exact" scoped:** N25/N26 validated the arithmetic
  for the captured, saturated policies; Q4's classification risk is
  independent and stands (D3, D8).

## Phase-0 QCoro spike (round S1) — July 19, 2026

First evidence round after the freeze, from running code rather than
re-reading: a standalone spike (`spikes/qcoro/`, QCoro pinned at
v0.13.0, Qt 6.11.1) exercised the spec's phasing-step-0 question
list. Ten findings; one falsifies the shutdown *mechanism* (the
shutdown *conclusion* survives by a different route), the rest
confirm premises or sharpen contracts. Verified plain and under
AddressSanitizer; the spike's in-process sentinel checks are the
authoritative leak accounting, with macOS `leaks` as a cross-check
on the subset it can root (see the second follow-up below).

- **S1-1 (falsifies R5-2's mechanism — spec shutdown section
  rewritten):** destroying a `QCoro::Task` handle while its
  coroutine is suspended does **not** destroy the frame. QCoro 0.13
  tasks are reference-counted: the promise holds its own reference
  until `final_suspend`, the handle holds a second. Handle
  destruction *detaches* — the frame survives, frame locals'
  destructors do **not** run (R6-1's premise), and the coroutine
  resumes normally if its awaited event is later delivered (verified
  for timer, `QFuture`, and `QNetworkReply` awaitables). A detached
  coroutine that runs to completion self-destroys at
  `final_suspend`; one that never resumes is leaked. QCoro has no
  API to destroy a suspended coroutine from outside.
- **S1-2 (shutdown conclusion survives via no-delivery):** all three
  awaiters resume only through the event loop — the reply awaiter
  connects `finished` with `Qt::QueuedConnection` to a context
  object owned by the frame, the `QFuture` awaiter resumes through
  `QFutureWatcher` event delivery, and timer awaits ride timer
  events. The shutdown-analog experiment (handles destroyed while
  suspended, then owners destroyed including a `QNAM` with an
  in-flight reply, with **no** further loop iterations) resumed
  nothing and did not crash. Cost: detached frames and their locals
  leak at process exit — a few hundred bytes per frame
  (`leaks`-verified) — acceptable, the process is exiting.
- **S1-3 (R6-1's reply release corrected):** because the frame is
  not destroyed at shutdown, the dispatch-time RAII wrapper never
  runs there; `QNetworkAccessManager` parent cleanup is the **only**
  shutdown reply cleanup, not a backstop. The wrapper still owns the
  reply on every live-session path (observed releasing the reply via
  `deleteLater` when a detached frame completed).
- **S1-4 (handle destruction is never cancellation):** mid-session,
  destroying a suspended task's handle detaches it and a later event
  resumes it while the loop runs — destroying handles neither stops
  nor cancels anything. D2's stop-token-only rule and D6's
  "abort never destroys live handles" + completed-only sweep are
  therefore load-bearing for lifetime, not merely stylistic; owners
  must never be destroyed while their suspended tasks could still be
  resumed by a live loop.
- **S1-5 (queued resumption verified, R4-2):**
  `QPromise::finish()` returns before the awaiter resumes — the
  `QFuture` awaiter delivers through the loop, so promise completion
  never re-enters synchronously. The stop-interruptible sleep
  prototype (QPromise + `std::stop_callback` posting a queued
  completion) delivered the full D2 contract: `request_stop()`
  returned before resumption, ~22 ms wake versus a 5 s sleep,
  pre-stopped tokens complete without suspending, undisturbed sleeps
  complete normally. `std::stop_callback` itself runs synchronously
  inside `request_stop()` — the queued indirection is mandatory.
- **S1-6 (ready awaits are synchronous):** `co_await` on a finished
  future does not suspend — the continuation runs inline in the
  caller, as does contextless `QFuture::then` on a finished future.
  Confirms the initialize-before-launch premise (IR2, D6).
- **S1-7 (move-out path):** `co_await qCoro(future).takeResult()`
  produced 0 copies / 3 moves; plain `co_await future` copies
  (`QFuture::result()`). The worker's single-consumer payload path
  should use `takeResult()` (D6).
- **S1-8 (R5-1 premises confirmed):** an unawaited task's exception
  is stored in the promise and vanishes silently when the handle
  dies — no terminate, no log; awaiting rethrows. Root-coroutine
  catch-alls stay mandatory. Destroying a completed task's handle
  from inside its own completion cascade did not fault (ASAN-clean;
  `final_suspend` resumes awaiters before dropping its own
  reference) — the deferred sweep remains the defensive choice.
- **S1-9 (completion cascades are synchronous):** a completing task
  resumes its awaiting coroutines directly on the completing stack
  (`TaskFinalSuspend`, source- and E10-verified). Coroutine-to-
  coroutine completion is the synchronous re-entrancy path; QFuture
  boundaries are the queued ones — relevant to D5/D6 stack-depth
  reasoning.
- **S1-10 (FetchContent hygiene confirmed; sharpened by Tom's
  review):** `QCORO_BUILD_EXAMPLES=OFF` and `QCORO_BUILD_TESTING=OFF`
  build no examples and register no QCoro tests with CTest, **with
  the parent's `BUILD_TESTING` left ON** — QCoro maps
  `QCORO_BUILD_TESTING` onto a directory-scoped `BUILD_TESTING` of
  its own, so the host's flag must not be forced (the spike's first
  cut forced it globally, which would have disabled acquisition's
  entire test suite if copied into the root project; caught in
  review, the checked-in reference now enables testing in the parent
  and verifies zero QCoro tests are registered).

**S1 follow-up (Tom's review of the spike round, same day).** Four
corrections: (1) three spec clauses still stated the falsified
mechanism — D3's reply-ownership bullet ("destroying a suspended
coroutine runs the destructors of its in-scope locals"), the
manager-harness reply-ownership pin ("task destruction mid-flight —
the dispatch-time wrapper releases it"), and the integration
shutdown pin ("resumes nothing and leaks nothing") — all rewritten
around live-session RAII versus shutdown-time QNAM parent cleanup
and the bounded detached-frame leak; (2) the shutdown analog now
also **destroys the unfinished `QPromise`s** the way the hub's deque
entries die (Tom verified the edge externally on Qt 6.11.1 first):
futures cancel, `.then` chains do not run, detached awaiters stay
suspended — and the same destruction mid-session would be ER1's
resume-into-`result()` hazard, which is why D2 confines
unfinished-promise death to post-loop shutdown; (3) the S1-10
CMake flag correction above; (4) leak-accounting honesty: the
in-process sentinels are authoritative — `leaks` cannot see
everything (details corrected in the second follow-up below).

**S1 second follow-up (external review, same day).** Four more
corrections: (1) the integration leak criterion was **impossible as
written** — it demanded that no promise shared state leak with a
detached frame, but QCoro's `QFuture` awaiter stores a `QFuture`
inside the frame, so the shared state is necessarily retained while
the frame leaks; the pin now specifies the bounded **transitive
closure** of the detached frames (frame + awaiter objects + watcher
and context QObjects + retained future handles and their shared
state + sleep timers) with replies and independently owned objects
explicitly outside it. (2) The shutdown analog previously discarded
the `.then` child future unawaited — production's worker awaits
`.then(parse)`'s **child**, not the parent; E8 now builds exactly
that topology (parent → parse continuation → child future → detached
awaiter suspended on the child) and pins that promise death cancels
the whole chain without running parse or resuming the child's
awaiter. (3) The leak-tool description contradicted the output:
observed on Qt 6.11.1/macOS, the reply frame is a stable root, the
**timer frames surface as root cycles**, and only the future frames
stay hidden behind Qt thread-data reachability (pending cancel
call-outs) — and the rooted set varies between runs, so the
sentinels remain the count and `leaks` is a cross-check on its
rooted subset. (4) D3 lifecycle wording: the reply wrapper's local
unwinds when the coroutine **body** completes; the frame
*allocation* lingers until the retained handle is swept (D6) — the
two events were previously conflated.

**S1 third follow-up (external review, same day).** Two refinements
to the E8 chain pin: the chain awaiter now uses the worker's actual
production await — `co_await qCoro(future).takeResult()`, which runs
as its own inner QCoro task frame — instead of a plain `co_await`;
and the child future's terminal state is asserted directly
(`isCanceled() && isFinished()` hold synchronously after promise
destruction, no event loop involved) rather than inferred from the
absence of parsing/resumption, with main's own child-future copy
released before leak accounting so only the detached frames retain
its shared state. Frame arithmetic corrected everywhere: seven
coroutine frame allocations leak — five top-level task frames (E6a +
four E8) plus the two inner task frames (`QCoro::sleepFor`'s,
`qCoro().takeResult()`'s).

## Phase-0 batch-scale measurement (round S2) — July 19, 2026

The other half of phasing step 0: peak memory and abort cost for the
full promise/future/frame/token population at the 2,000-tab scale
the items-pipeline doc names. Second binary in the spike project
(`spikes/qcoro/batch.cpp`, target `qcoro_batch`) — a fresh process
for clean memory baselines, away from `qcoro_spike`'s deliberate
leaks. It builds the spec's production topology per entry (pump
deque entry with realistic `QNetworkRequest` + endpoint label +
timestamp + `QPromise` + stop token; facade `.then(parse)` chain
with no parent-future retention; eager per-fetch worker suspended on
`co_await qCoro(child).takeResult()` with the post-await stop check;
handles in a worker-owned container; one `stop_source`). The
parsed-outcome stand-in is sized to the real facade payload —
`sizeof(std::expected<poe::StashWrapper, FetchError>)` is 336 bytes
against the actual headers (the spike static_asserts the probed
numbers, not the production types — re-probe manually if they change;
`poe::CharacterWrapper`'s outcome is 744, so a character-scale run
costs ~0.8 KB more per entry) — because the outcome occupies
compile-time frame slots per entry, not just completed shared state
(see the follow-up below). It then aborts
the maximal standing population — every entry still queued:
`request_stop()` → drain loop completes each entry `Canceled` at the
dequeue checkpoint → queued watcher resumptions → deferred sweep.
Numbers from a Release build, Qt 6.11.1/macOS, allocator bytes via
`malloc_zone_statistics`, footprint via `task_vm_info`; validity
checks also pass under ASAN.

- **S2-1 (standing population ≈ 6.5 KB/entry, linear):** the full
  per-entry machinery — queue entry (request with auth/UA headers
  and transfer timeout, label, timestamp, promise, token), parse
  chain and child future, suspended worker frame plus its inner
  `takeResult` task frame and `QFutureWatcher`, owned handle, with
  the outcome slots sized to the real stash wrapper — costs
  ~6.5 KB of heap. Measured 6542/6516/6520 bytes per entry at
  N=200/2,000/10,000 — linear, no superlinear surprise. The 2,000-tab
  population is **~12.4 MB heap (~11–13 MB footprint)**; even
  10,000 tabs is ~62 MB.
- **S2-2 (abort cost is milliseconds):** at N=2,000 the whole abort
  runs ~2–4 ms: Canceled-completion burst ~2.4 ms (2,000 promise
  completions with the pass-through parse continuations running
  inline on the completing stack), resumption drain ~1.1 ms (2,000
  queued `QFutureWatcher` resumptions through the event loop),
  handle sweep ~0.3 ms. N=10,000 stays ~13 ms. The dominant abort
  latency is therefore the one-time ~22 ms stop-interruptible
  pacing-sleep wake per pump (S1-5), not anything that scales with
  population. The batch-submit burst itself (build + chain + launch
  to first suspension, synchronous on the main thread) is ~8 ms at
  N=2,000.
- **S2-3 (memory fully unwinds; no retention at scale):** heap
  returns to ~baseline after the sweep (+0.23 MB at N=2,000,
  allocator noise) and `leaks --atExit` reports **zero leaks** —
  the canceled population leaves nothing behind: no completed-future
  retention, no frame residue (ER5/D2's no-retention rule holds at
  the population level). Intermediate points: after the Canceled
  burst the child shared states hold 2,000 stored outcomes
  (+6.6 MB over baseline); after resumption only the completed
  frames remain (+2.3 MB); the sweep releases those.
- **S2-4 (scale does not bend the S1 contracts):** at 2,000 entries
  no worker resumed during submission (all suspended), none resumed
  on the completing stack (queued delivery holds under a 2,000-
  completion burst), every post-await stop check fired, no parse
  continuation did real work on the abort path, and every handle was
  ready before the sweep.

**Verdict:** the numbers do not demand flow control — bounded
flow control stays unbuilt (the spec's "measure first" is resolved),
and D6's deferred sweep needs no incremental per-completion
reclamation. Not simulated: `QueueUpdated` signal emissions (D6
already routes any stutter to UI-side coalescing), real dispatch and
replies (at most the gate cap of 2 in flight — negligible against
the queued population), and success-path payload memory (an
items-pipeline sizing concern, not machinery cost).

**S2 follow-up (external review, same day).** Two corrections. (1)
The first cut's 104-byte synthetic outcome **undercounted the
standing population**: the real stash outcome is 336 bytes
(character: 744), and `sizeof` of the outcome type is baked into
the population at compile time — it landed in three frame slots per
entry along the `takeResult` path (the worker frame's local, the
inner task's promise result storage, and the awaiter's move-out
path), not just in completed shared state. The stand-in is now
padded to the real stash wrapper and static_asserted against
drift; per-entry cost rose 5.8 → ~6.5 KB (measured +706 bytes,
consistent with three 232-byte slot growths) and every number above
was re-measured. The verdict is unchanged. (2) The resumption-wait
**timeout path was itself an S1-1 hazard**: after a 30 s timeout the
code went on to sweep the handles and destroy the update — detaching
any still-suspended worker frames while they retain pointers into
the update's counters, which a later scale's event processing could
resume into freed state. On timeout the binary now deliberately
leaks the update (mirroring the shutdown contract: detached frames
must outlive nothing that can still be delivered to them) and runs
no further scales.

## Revision log

- **July 18, 2026** — drafted; open items reviewed with Tom and
  resolved; accepted.
- **July 18, 2026 (later)** — external review reopened the spec:
  correctness gaps in cancellation/lifetime, degraded-HEAD handling,
  and gate ownership; findings organized into four groups. **Group 1
  (cancellation and lifetime — ER1/ER4/ER5/ER9) resolved:**
  `std::stop_token` replaces QFuture cancellation wholesale (D2
  rewritten — ER1's UB is now unconstructible; futures always
  finish), pump token checkpoints and stop-interruptible sleeps
  specified (D3), never-abort-in-flight rule stated with its N25
  rationale, shutdown and task-ownership contract added, the
  future-retention registry deleted, QCoro pinned to 0.13.0 as a hard
  floor, explicit compiler floors required in CMake.
- **July 18, 2026 (group 2)** — degraded HEAD and policy topology
  (ER2/ER6/ER7/ER8) resolved: D4 rewritten as an explicit endpoint
  state machine with a Discovery fallback; D8 added (validity tiers,
  status-first classification, the `Protocol` error kind); D3 retry
  timing resolved. Verification found the degraded-header path is
  **UB in today's parser** (`ratelimitpolicy.cpp:52`): ground-truth
  N20 corrected in place, Q3 residual closed.
- **July 18, 2026 (group 2, round 2)** — nine re-review corrections
  verified and adopted (endpoint-scoped evidence, gate-exclusive
  discovery, unconditional 60 s retry pad, 60 s setup-failure
  cooldown, per-endpoint `Protocol` counter, transport-error
  precedence, discovery-429 split, reset-scope limits, two
  documentation fixes).
- **July 19, 2026 (group 2, round 3 + design principle)** — round-3
  findings resolved under the newly stated **containment principle**:
  rename handling rewritten from live migration to containment; name
  check ordered before mutation; discovery seeding restricted to new
  pumps; HEAD 429 defined; setup cancellation specified; pumps live
  to shutdown; round-3 pins added.
- **July 19, 2026 (simplification pass)** — Tom reviewed the
  accumulated design against the containment principle and directed a
  simplification; groups 3 and 4 resolved in the same pass.
  **Deleted:** the Discovery state and all its machinery (degraded or
  failed HEADs now fail the endpoint's requests cleanly under the
  setup cooldown — reversing the first round's degrade-and-proceed
  decision); the steady-state `Protocol` threshold, per-endpoint
  consecutive counters, and the queue-extraction primitive; rename
  containment; the `NameOnly` tier and join shortcut. **Resolved:**
  ER3 by shrinking the gate with pre-session login traffic as a
  documented exclusion; group 4 by the pass itself. The spec was
  rewritten clean from the surviving decisions; review history moved
  to an appendix (since split into this file). Status returned to
  **accepted**.
- **July 19, 2026 (implementation-readiness review — rev. 3)** —
  Tom's line-level review against the code and Qt/QCoro
  docs; six findings (IR1–IR6), three simplifications, four
  investigations, all adopted. **Substantive reversals:** steady-state
  degraded or mismatched headers are a strict `Protocol` error (IR1 —
  the simplification pass's log-and-continue rule was silent
  continuation on bad state: the arithmetic is state-driven and a
  frozen-OK policy paces nothing); the awaitable FIFO is deleted
  (deque + on-demand drain); forum traffic leaves the gate (fully
  internal now). **Added — invariants and error paths, not
  mechanism:** hub shutdown stop_source observed at every wait;
  `Internal` error kind with scoped completion guards and a terminal
  pump-failed state; worker post-await and initialize-before-launch
  invariants; gate permit span, HEAD writer preference, and
  complete-promise-last ordering; the normative attempt table
  (exhaustion never sleeps; violations recorded before stop/retry
  decisions; Full HEAD 429 without valid Retry-After = setup
  failure); phase-0 QCoro spike and 2,000-tab measurement;
  exception-tight facade continuations; QCoro FetchContent hygiene.
- **July 19, 2026 (review round 4 — rev. 4)** — a post-acceptance
  review pinned four implementation ambiguities (R4-1–R4-4): permits
  are never held across retry sleeps (the pseudocode read otherwise —
  a ~16-minute stall hazard); stop wakeups resume via the event loop,
  never synchronously inside `request_stop()`, and teardown's
  drain-await mechanism became a phase-0 spike deliverable; worker
  task-handle ownership and update completion are specified; the 10 s
  transfer timeout is named a gate liveness invariant. Six minor
  items fixed in place. No design decision reopened; the complexity
  pass found nothing to delete.
- **July 19, 2026 (review round 5 — rev. 5)** — Tom's
  implications review of rev 4 against the code it must live in: four
  blockers (R5-1–R5-4), two resolved by deletion. **Exception
  supervision moved into the coroutines** — no exception ever escapes
  a root task; owned handles supervise nothing (QCoro rethrows only
  to an awaiter); the drain's catch-all implements the terminal pump
  state; deferred handle sweep; abort never destroys live handles.
  **The asynchronous teardown choreography and the hub shutdown
  stop_source are deleted** — rev 4's sequence needed a live event
  loop after `a.exec()` had returned and a worker the destruction
  order had already destroyed; shutdown is now destruction order
  (consumers first, handles destroyed while suspended, replies
  aborted after awaiters are gone), every wait takes one token, and
  every-future-finishes is scoped to live sessions. **The facade
  establishes the transfer-timeout invariant** — rev 4 called it
  existing behavior, false for the legacy stash-index request (F60).
  **Raw reply ownership assigned** — the pump/setup path solely owns
  replies via RAII (F59's lesson, applied one level down). Net
  complexity: negative.
- **July 19, 2026 (docs split)** — review history moved from the
  spec's Appendix B into this file; the spec keeps only current
  decisions, inline finding-ID citations, and a pointer here. No
  content change beyond heading levels, cross-reference wording, and
  superseded-by notes added to the ER4/IR3/IR4/R4-2/R4-3/R4-4 rows.
- **July 19, 2026 (review round 6 — rev. 6; spec frozen for
  implementation)** — third implications round; convergence
  declared. **Reply ownership made total (R6-1):** the RAII wrapper
  installs at dispatch, so the pump frame owns the reply through
  suspension; the shutdown abort step and its implied in-flight
  registry are deleted — `abort()` is never called anywhere; QNAM
  parent cleanup is the documented post-loop backstop (`deleteLater`
  is inert without a running loop). **Consumer lifetimes specified
  (R6-2):** `Shop` stays callback-style via a context-bound
  continuation; `PoeApiClient` is a `UserSession` member declared
  after `rate_limiter`, its continuations capture values only.
  **Observation separated from classification (R6-3):** every landed
  reply with Full matching headers updates pacing regardless of
  status; outcomes stay status-driven; `Protocol` only on a
  would-be-clean 2xx. Further findings are filed against
  phase-0/harness evidence, not re-readings.
- **July 19, 2026 (errata batch, round 7 — rev. 7; freeze holds)** —
  eight corrections and contract completions, all shrinking (list
  above): gate ordinary-waiter FIFO; Full grammar tightened and dead
  `EstimateDuration` slated for deletion; Retry-After grammar per
  RFC 9110 with the 900 s cap relabeled product policy; one
  `RateLimited` kind for every terminal 429; the stale
  cancel+resubmit reprioritization claim removed; the facade made
  genuinely stateless; the transfer timeout correctly described as
  an inactivity bound; "empirically exact" scoped to the captured
  policies. Bar for any further paper finding: a false statement or
  a missing harness-needed contract.
- **July 19, 2026 (phase-0 spike, round S1 — rev. 8)** — first
  running-code evidence round. S1-1 falsified R5-2's
  destroy-while-suspended mechanism (QCoro tasks are refcounted;
  handle destruction detaches, locals' destructors do not run, a
  detached frame resumes if its event is later delivered). The
  shutdown section was rewritten around **detach plus no delivery**:
  every awaiter resumes only via the event loop, so post-`exec()`
  destruction resumes nothing; detached frames leak a few hundred
  bytes each at process exit; QNAM parent cleanup is the only
  shutdown reply cleanup (S1-2, S1-3). Handle destruction is never
  cancellation (S1-4). All other spike-verified premises held:
  queued stop wakeups and the sleep primitive (S1-5), synchronous
  ready-future continuation (S1-6), `takeResult()` move-out (S1-7),
  silent unawaited exceptions and the sweep (S1-8), synchronous task
  completion cascades (S1-9), FetchContent hygiene (S1-10). Spike
  code lives in `spikes/qcoro/`; the 2,000-tab measurement remains
  open. Same-day follow-up from Tom's review: three leftover spec
  clauses still stating the falsified mechanism rewritten (D3
  reply ownership, two testing-plan pins), the shutdown analog
  extended to destroy unfinished promises (futures cancel, `.then`
  chains break, detached awaiters stay suspended), and the S1-10
  guidance corrected — QCoro's flags only, never the global
  `BUILD_TESTING`. A second follow-up (external review, same day)
  replaced the impossible no-shared-state leak criterion with the
  transitive-closure bound, gave E8 the production chained-future
  topology (worker awaits `.then(parse)`'s child), matched the
  leak-tool description to its actual output, and separated
  body-completion local unwinding from frame-allocation reclamation
  in D3.
- **July 19, 2026 (batch-scale measurement, round S2 — freeze
  holds)** — phasing step 0 completed: the 2,000-tab measurement ran
  (`spikes/qcoro/batch.cpp`). Standing population ~6.5 KB/entry,
  linear to 10,000 (~12.4 MB at 2,000 tabs); abort costs milliseconds
  end to end (~2–4 ms at 2,000; the ~22 ms per-pump sleep wake
  dominates) and memory unwinds to baseline with zero leaks after
  the sweep. Verdict: no flow control, no incremental reclamation —
  the spec's "measure first" clauses resolved with numbers; D6 and
  the phasing sketch cite S2 inline. Same-day follow-up (external
  review): the outcome stand-in resized from 104 bytes to the real
  336-byte stash wrapper (it occupies three compile-time frame slots
  per entry — the first cut undercounted by ~0.7 KB/entry; numbers
  re-measured, verdict unchanged), and the resumption-wait timeout
  path fixed to leak the update deliberately instead of sweeping
  live frames into a use-after-free.
- **July 20, 2026 (phase-5 planning-readiness amendment — rev. 9;
  freeze holds)** — the phase-5 planning handoff exposed an authority gap:
  D6 required batching but the testing plan did not require a regression
  test that would fail if worker-side serialization returned, and several
  preservation expectations existed only as planning prose. D6 now states
  the staged discovery/batch shape, per-policy order, active-`Update()`
  refusal, and cancellation/status accounting explicitly. Testing-plan item
  5 now requires the F56 concurrency shape, shared update-token identity, and
  preservation of the M1/F53/F55 boundaries under reordered completion.
  Test seams, mutation mechanics, shutdown-runner layout, and commit ordering
  remain in the phase-5 verification contract (the transient execution plan was
  removed once phase 5 landed); no network-layer design changed.
- **July 21, 2026 (phase-5 implementation reconciliation — rev. 10;
  freeze holds)** — a phase-5D review found D6's "the callback pyramid with
  its flag pairs (`m_need_*` / `m_has_*`) collapses into control flow"
  overstated the realized shape. The per-fetch pyramid did collapse into one
  flat coroutine per fetch, but the root orchestration is a counter-driven
  join, not a linear top-to-bottom await: D6's own concurrency requirements
  (required lists launched without awaiting one another; each list's content
  launched from its own handler, never held behind the sibling list; child
  batches discovered dynamically in replies) make a static `co_await` sequence
  over the update tree impossible, and the `m_has_*` list-arrival flags are
  irreducible under a counter-driven join — they distinguish "list not
  arrived" from "required list arrived empty," which `received == needed`
  cannot, and the concurrent content-during-list-arrival window depends on the
  same signal. D6 amended to describe the counter-driven topology and the
  residual flags accurately; `m_need_*` reclassified as selection config. No
  code or behavior changed — the implementation (5B's fire-and-owned per-fetch
  tasks + counter-driven finalization) was already correct; the prose was
  reconciled to it. The ceremonial root task handle (R4-3) is also removed:
  RunUpdate becomes a synchronous `void` method — a root that never suspends
  owns no asynchronous lifetime, and its catch-all works identically in an
  ordinary function — so `m_update_task` is deleted and R4-3 / the plan's
  cross-package invariant 3 are amended to match (only the per-fetch tasks are
  owned handles now). Two stale test comments and a test-only header backdoor
  were fixed in the same review; a friend accessor moved out of the production
  header.
- **July 21, 2026 (phase-5 exception-policy reconciliation — rev. 11;
  freeze holds)** — a phase-5 review found the frozen R5-1 exception policy
  contradicted the shipped worker. The spec said a per-fetch task's catch-all
  should "count the completion, then `request_stop()`"; production instead
  finalizes a failed update through a direct, counter-independent
  `AbortUpdate()` (the single idempotent terminal transition guarded on
  `WorkerState`). The implementation is correct and the spec text was wrong:
  counting a failed fetch as received is functionally dead (no path reads the
  counter on failure) and feeding it through the success-equality counters
  drove `needed` below `received`, making reported progress non-monotonic
  (P-STATUS). The direct abort is the stronger anti-wedge guarantee — it does
  not depend on every straggler counting. The exception-policy bullet and the
  R5-1 pin in the spec are reconciled to the direct-abort path; the phase-5
  verification contract's July 21 W-THROW revision note already recorded and
  accepted this. No code or observable-behavior change.
