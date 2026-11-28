# Network Redesign: Typed Facade, Coroutine Pumps, and the Gate

**Status: ACCEPTED July 20, 2026 (revision 11, July 21, 2026) — frozen for
implementation; phase 0 complete (spike + batch-scale
measurement); phase 1 complete (manager harness,
`tests/tst_ratelimitmanager.cpp`, July 19, 2026); phase 2 complete
(QCoro dependency + primitives, `src/ratelimit/scheduler.{h,cpp}` /
`stopsleep.{h,cpp}` / `gate.{h,cpp}`, July 19, 2026); phase 3 complete
(July 20, 2026: the D8 total parse, the pump rewrite, async HEAD setup
with parking and cooldown — F57, F58, and F5-modernization resolved);
phase 4a complete (July 20, 2026: the `QFuture<FetchOutcome>` boundary,
the pump's stop checkpoints, the `Protocol` outcome half, the
`PoeApiClient` facade, and `Shop` — F60 and F50 resolved, F59 narrowed to
the legacy adapter; phase 4 split into 4a/4b for harness reasons,
recorded in the phasing sketch); phase 4b complete (July 20, 2026: the
worker's call sites moved to the facade, `tst_workerupdate` onto a typed
facade fake, `tst_ratelimiter` off the legacy wrapper, and
`RateLimitedReply` + the `Submit()` adapter + its synthetic reply deleted
— F59 resolved); phase 5 complete (July 21, 2026: the worker rewritten to
staged batch submission through coroutine pumps with a single per-update
`std::stop_source`; the worker queue and update-generation machinery deleted;
full-chain integration runner under CI ASan/LSan — F56 resolved).** Drafted
July 18 and
reviewed through six rounds in two days, plus one post-freeze errata
batch (rev 7: eight corrections and contract completions, all
shrinking — R7 in the review history). The review process converged
(later rounds found contradictions among earlier fixes and resolved
them by deletion), so further findings are filed against
phase-0/harness evidence, not re-readings; any further paper finding
must show a false statement or a missing harness-needed contract.
Rev 8 is the first such evidence round: the phase-0 QCoro spike
(round S1, code in `spikes/qcoro/`) falsified R5-2's
destroy-while-suspended mechanism — QCoro task handles detach, not
destroy — and the shutdown section now rests on
detach-plus-no-delivery (S1-1..S1-4); every other spike-tested
premise held (S1-5..S1-10). The batch-scale measurement (round S2,
same spike project) closed phasing step 0's open question with
numbers: ~6.5 KB per entry, milliseconds to abort 2,000 tabs — no
flow control (S2-1..S2-4). Rev 9 is the phase-5 planning-readiness
amendment: it makes D6's staged batch shape and active-update policy
explicit and adds the missing F56 and preservation acceptance criteria;
it changes no network-layer design. Rev 10 (July 21, 2026) is a phase-5
implementation reconciliation: D6's "the callback pyramid with its flag
pairs collapses into control flow" overstated the realized shape — the
per-fetch pyramid collapsed into coroutines, but the root is a
counter-driven join and the `m_has_*` list-arrival flags are irreducible
under it; the prose is reconciled to the (already correct) implementation, and the
ceremonial root task handle — dead weight for a root that never suspends —
is removed (RunUpdate becomes a synchronous method; R4-3 amended). No design
or observable-behavior change. Rev 11 (July 21, 2026) reconciles the R5-1
exception-policy text with the realized worker: a per-fetch throw finalizes
through the direct, counter-independent `AbortUpdate()`, not "count the
completion then `request_stop()`" — counting a failed fetch is functionally
dead and drove reported progress non-monotonic (P-STATUS), so the frozen text,
not the code, was wrong. No code or observable-behavior change. The finding
tables (ER, IR, R4-\*,
R5-\*, R6-\*, R7, S1, S2), the round narratives, the reversal records,
and the revision log live in `docs/archive/network-redesign-reviews.md`; this
spec records only current decisions and cites finding IDs inline
where a decision's shape came from a review. The spike is throwaway
evidence, not production code; production code against this spec
begins with phasing step 2 (the primitives).

This document specifies the redesign of acquisition's rate-limited
networking: how the items worker, the rate limiter, and the network
boundary between them are restructured. It is the design output of the
network ground-truth research phase
(`docs/design/network-ground-truth.md`) and the re-derivation of the
paused F56–F59 fix shapes (`docs/cleanup/findings.md`). It also answers
the question `docs/design/items-pipeline.md` deferred to the M2 spec:
**where does scheduling live?** Answer: pacing and retry live in the
rate limiter; selection and ordering live in the worker; flow control
between them is cancellation, not queue management.

Every load-bearing design choice cites the ground-truth claims (N-numbers,
P-A/P-B) or findings (F-numbers) it rests on. If a cited claim falls,
the decision that cites it must be re-examined.

**Staleness preamble** (per the items-pipeline working rules): written
against branch `network-redesign` (named `fix-f57-f59-ratelimit-retry`
until the July 19 rename) at `b135ff5` (code
identical to master merge `03b15a3` plus the capture instrument). Code
anchors: `src/ratelimit/ratelimiter.{h,cpp}`,
`src/ratelimit/ratelimitmanager.{h,cpp}`,
`src/ratelimit/ratelimitpolicy.{h,cpp}`, `src/itemsmanagerworker.{h,cpp}`,
`tests/fakenetwork.h`. Re-verify anchors before following this spec if
those files have moved on.

---

## Current rules

A summary for working on the code. The decisions below remain the
authority; where this summary and a decision disagree, the decision
wins.

- What has not been seen in real traffic is contained: detect it, log
  it loudly, capture it, fail the affected requests cleanly. Never
  carry on with bad state. (Design principle)

**Boundary and cancellation**

- Every rate-limited call goes through `PoeApiClient` and returns
  `QFuture<std::expected<T, FetchError>>`. Nothing above it sees a
  `QNetworkRequest` or `QNetworkReply`. Errors are values: root
  coroutines, `RunUpdate` and the parse continuation catch everything
  and finish with one. (D1, D7)
- Every promise reaches a final state. `CompleteRequest` settles an
  entry once; the drain's guard settles an abandoned one as
  `Internal`. (D2)
- Never call `QFuture::cancel()`, `cancelChain()` or
  `QNetworkReply::abort()`. The `std::stop_token` is the only
  cancellation channel, and `Canceled` is an outcome, not a failure.
  A stop callback only schedules work. (D1, D2; N25)
- The pump checks the token at dequeue, after each sleep, after the
  gate and after the reply lands. A stopped request already in flight
  lands and is recorded before it completes `Canceled`. (D2)
- Everything runs on the main thread (`RateLimiter::SubmitFuture`
  asserts it). (D7)

**Worker**

- One `std::stop_source` per update; every call in it carries that
  token. A terminal failure goes to `AbortUpdate()`. Each per-fetch
  coroutine checks the token right after its `co_await`
  (`ProcessIfLive`). (D2, D6)
- The worker has no queue and no pacing: lists launch without awaiting
  each other, and each list or parent reply launches its whole batch
  in source order. Counters are set before the first launch. (D6; F56)
- `m_fetch_tasks` owns every per-fetch task; `SweepTasks` runs queued
  and destroys completed tasks only. (D6)

**Pumps and the gate**

- Each `RateLimitManager` drains a plain deque with an on-demand
  `Drain()` task. A throw there fails the pump for the session: later
  submissions fail fast with `Internal`. (D3; Exception policy)
- Every landed reply records history and capture, and every 429 a
  violation, before any stop or retry decision; headers are parsed
  whatever the status. Only a full parse naming the pump's policy
  updates it; a new shape under the same name clears history. (D3, D8)
- Classification order: 429, other non-2xx (`Http`), a Qt error even
  after a 2xx (`Network`), a clean 2xx with unusable headers
  (`Protocol`). (D8)
- A 429 is sent at most `MAX_ATTEMPTS` (3) times, waiting until
  `max(now + Retry-After + 60 s + 1 s,
  GetNextSafeSend)`. `ParseRetryAfter`
  accepts integers from 0 to 900 only; anything else makes the 429
  terminal `RateLimited`. (D3, D8; N12, N23)
- The reply's owner (`ReplyGuard`) is installed at dispatch, before
  the first await. (D3)
- Every hub send acquires the `Gate`: 2 in flight, 250 ms between
  dispatches, first in, first out. A HEAD takes the whole gate, and a
  waiting HEAD blocks new permits. A permit is released when its reply
  finishes, before the promise completes, and is never held across a
  sleep. (D3, D4, D5; F5, F58, N18)
- Every request the facade builds sets `kPoeApiTimeout`; the gate's
  liveness depends on it. (D5, D7; F60)
- Login, OAuth, forum posts and CDN traffic stay outside the hub. (D5)

**Endpoint setup**

- An endpoint goes Unknown, Probing, Established; entries park in
  order behind the probe. No nested event loop. (D4; F5)
- A HEAD 429 with an acceptable Retry-After establishes the pump with
  a hold. Any other setup failure fails the parked entries with a
  `FetchError` and starts a cooldown of at least 60 s, during which
  submissions fail fast. Cancellation never aborts a probe or starts a
  cooldown. Established is sticky. (D4; N16, N24)

**Shutdown and build**

- Shutdown is destruction in `UserSession` member order after
  `a.exec()` returns: `Shop` and the worker, then `PoeApiClient`, then
  the hub, and the `NetworkManager` last. Never destroy an owner while
  the event loop runs. (Shutdown; D7)
- `Shop` uses no coroutines; it takes futures with `.then(this, ...)`.
  (D7)
- QCoro is pinned at v0.13.0 with its examples and tests off. Never set
  the global `BUILD_TESTING`. (Dependency: QCoro)

**No longer applies**

- The staleness preamble, the phase notes on temporary code, and every
  "today" description of the old code (`SetupEndpoint`,
  `RateLimitedReply`, `DiscardIfStale`, the generation tag): deleted.
- The rules the spec reverses in its own text: discovery GET, the
  `NameOnly` tier, degrade-and-proceed setup, log-and-continue on bad
  headers, the `Http{429}` split, the shutdown stop token, abort at
  shutdown.
- Per-entry cancellation and reprioritization were never built.

## The problem being solved

Today there are two queues and two in-flight trackers stacked on top of
each other: each `RateLimitManager` has a queue and an active-request
slot, and `ItemsManagerWorker` has another queue (`m_queue`) with its own
one-in-flight discipline. The worker layer exists only as a workaround
(`ea9dd95`, v0.17.0) for an abort-desync problem that itself traces to an
unclear ownership contract on `RateLimitedReply` (F59). The visible
damage: cross-policy starvation (F56), a 429 retry that wedges updates
until restart (F57), dead pacing code (F58), and a blocking nested event
loop for HEAD setup (F5).

The redesign dissolves these with three moves rather than four fixes:

1. **Ownership and completion move to Qt-native types** — `QFuture` with
   `std::expected` payloads. `RateLimitedReply` (the custom reply
   boundary) is deleted; the limiter's internal `RateLimitedRequest`
   queue entry stops owning a reply and carries a `QPromise` instead;
   cancellation becomes the abort mechanism; F57 and F59 stop being
   expressible.
2. **Queueing returns to the rate limiter** — the worker batch-submits
   and counts completions; per-policy parallelism returns automatically
   (F56); the worker stops doing flow control at all.
3. **Each policy manager becomes a coroutine "pump"** — one linear
   drain loop (take, wait, send, maybe retry, deliver) instead of a
   state machine smeared across timer and signal handlers. The 429
   retry becomes a loop iteration that is impossible to wedge and
   trivial to test.

## Design principle: contain the unexpected (added July 19, 2026)

Stated by Tom after the round-3 review, as both a design rule and the
convergence criterion for further review rounds:

> Prefer a simple system that **errors when something unexpected
> happens** over a complex one that continues through it.

Operationally: **continuation semantics are designed only for observed
reality** — 429s, network failures, cancellation, the documented
header contract. Events that are unobserved or observed once
(policy renames, multi-endpoint policies, degraded header sets) get
**containment**: detect, log loudly, capture, fail the affected
requests cleanly, and re-establish on a later update. **Build the
detection, not the response**: for a never-observed event, the capture
of its first real occurrence is worth more than any pre-designed
choreography, which would be guessing at the event's shape anyway.
P-A already prices in the residual — a mishandled rare corner costs at
most a mistimed request or one violation, which the design absorbs by
construction; what it must never cost is a wedge, a crash, or silent
continuation on bad state.

The July 19 simplification pass applied this principle retroactively
to the decisions that predated its statement; the
implementation-readiness review then applied it once more to the
simplification's own output (IR1: log-and-continue on degraded
headers turned out to be silent continuation on bad state — it is now
a clean error). The reversals are recorded in the sections they touch
and in the review history (`docs/archive/network-redesign-reviews.md`).

## Layer contracts

Five layers, each speaking one language, each testable alone:

| Layer | Class | Speaks | Owns |
|---|---|---|---|
| Orchestration | `ItemsManagerWorker` | tabs, characters, selections | what to fetch, in what order, when an update is done |
| Typed facade | `PoeApiClient` (new) | PoE domain types ↔ HTTP | request building, response parsing, error taxonomy |
| Hub | `RateLimiter` | endpoints, policies, the IP | endpoint→policy topology, HEAD setup, the gate, capture |
| Policy pump | `RateLimitManager` | one policy's pacing | queue, timing, send, 429 retry, history |
| Policy arithmetic | `RateLimitPolicy` | header math | next-safe-send calculation — arithmetic **unchanged** (N25, N26); parsing becomes a total function (D8) |

### D1. The boundary type: `QFuture<std::expected<T, FetchError>>`

- The limiter's submit returns
  `QFuture<std::expected<QByteArray, FetchError>>` (body bytes on
  success). The facade chains `.then(parse)` onto it and returns
  `QFuture<std::expected<Payload, FetchError>>` with `Payload` a typed
  struct (`poe::StashListWrapper`, `poe::StashWrapper`,
  `poe::CharacterWrapper`, …).
- **Errors are values, never exceptions.** No exception crosses a layer
  boundary. `FetchError` is one struct with a kind enum:
  `Network` (transport failure — including a reply truncated after a
  2xx status arrived; carries the Qt error; precedence is D8's),
  `Http` (non-2xx status; carries the status),
  `Parse` (facade-level JSON failure),
  `Protocol` (2xx whose rate-limit headers fail to parse or carry a
  mismatched policy name — setup or steady state, D4/D8),
  `RateLimited` (a terminal 429 — retries exhausted *or*
  non-retryable; carries the attempt count and Retry-After
  acceptability; one kind for every terminal 429, R7 — see D3/D8),
  `Internal` (an exception escaped inside the pump or facade — a bug,
  contained as a value; see the exception policy),
  `Canceled` (the caller requested stop — see D2). `Canceled` is an
  outcome, not a failure: consumers treat it as "return silently" and
  it never counts toward request-failure accounting. **Futures are
  never canceled in the `QFuture` sense — every future finishes**
  while any consumer is alive to await it (revised July 18 after
  external review; scoped to live sessions by R5-2 — at application
  destruction promises may die unobserved, safely, because every
  awaiter is destroyed first: see the shutdown section; the whole
  cancellation design is D2).
- `RateLimitedReply` and the worker-facing reply boundary (the
  `Submit()`/callback shape) are deleted. `RateLimitedRequest` is **not**
  deleted: it was always the limiter's internal queue entry — never a
  type the worker named — and it is transformed in place. The field that
  used to own a reply (`std::unique_ptr<RateLimitedReply>`) becomes the
  `QPromise` the caller's future is built on; the entry also holds the
  `QNetworkRequest`, the endpoint label, capture timestamps, and the stop
  token, and `NetworkCapture` still consumes it. (An earlier draft of
  this section listed the entry as deleted; that was wrong — the type
  persists. A rename to something like `RequestEntry` is available as a
  cosmetic follow-up, but is not required.)
- Resolves F59 by construction (Qt's shared state is the single owner)
  and removes the object F57 destroyed.

### D2. Cancellation: one `std::stop_token` per update; futures always finish

**Rewritten July 18, 2026** after ER1 demonstrated the
original `cancelChain()` design was unsafe: QCoro's `QFuture` awaiter
(verified at v0.13.0) resumes *immediately* when a future is canceled
and calls `result()` unconditionally in `await_resume` — before any
guard in the coroutine body can run. A sibling coroutine suspended on
its own future when another request fails and cancels the batch would
resume straight into `result()` on a canceled, resultless future:
undefined behavior, unpreventable by any check written *after*
`co_await`. The fix is not a safer guard but a design in which the
hazard is unconstructible:

- **`QFuture::cancel()` / `cancelChain()` are never used, anywhere.**
  The only cancellation channel is `std::stop_token`. Every promise
  reaches a final state — payload, `FetchError`, or `Canceled` —
  so awaiting is unconditionally safe, even through QCoro's naive
  awaiter, even for a coroutine suspended at the moment its update is
  aborted (the ER1 regression pin in the test plan).
- The worker holds one `std::stop_source` per update; every facade
  call takes its token, which travels with the queue entry into the
  pump. Abort = `request_stop()` — the worker proceeds with its own
  abort bookkeeping immediately and never waits on stragglers; they
  resolve `Canceled` asynchronously and their coroutines return
  silently. (The token is the update's *identity* only under the
  worker's post-await invariant — D6.)
- **Pump checkpoints** (ER4): the stop state is
  checked at dequeue, after the pacing sleep, after gate acquisition,
  after the reply lands, and after a retry sleep — before every send.
  "Stopped" means the entry's token — the *only* token anywhere
  (R5-2 deleted the hub shutdown token: shutdown is destruction, not
  a signal — see the shutdown section). A stopped entry completes
  `Canceled` at the first checkpoint that sees it. Pacing and retry
  sleeps are **stop-interruptible** (the sleep primitive takes the
  entry token and wakes on stop), so an aborted update's queue drains
  promptly instead of at pacing speed. The failure mode of a *missed* checkpoint is one
  wasted — but paced, gated, harmless — send, not undefined behavior;
  that graceful degradation is a deliberate property of this design.
- **Queued resumption (R4-2):** stop-triggered wakeups are delivered
  through the event loop — `request_stop()` returns before any
  suspended coroutine resumes; no stop callback resumes a coroutine
  synchronously. The worker's first-failure `request_stop()` runs
  inside a promise-completion continuation; synchronous resumption
  there (`std::stop_callback` runs on the requesting thread, inside
  the `request_stop()` call) would cascade every suspended pump drain
  and worker coroutine onto the current stack — exactly the unbounded
  re-entrancy D5's complete-promise-last rule exists to avoid. The
  stop-interruptible sleep and the gate implement wake-on-stop as a
  queued resume; spike-verified (S1-5): `std::stop_callback` runs
  synchronously inside `request_stop()`, the queued indirection
  delivers the contract, and QCoro's `QFuture` awaiter itself
  resumes through the event loop (`QFutureWatcher`), so promise
  completion never re-enters an awaiter synchronously. (Completion
  of a *task* awaited by another coroutine, by contrast, resumes the
  awaiter synchronously on the completing stack — S1-9.)
- **Destroying a task handle is never cancellation (S1-4):** QCoro
  0.13 task handles are shared references, not owners — destroying
  one while the coroutine is suspended *detaches* the frame, which
  survives and resumes normally when its awaited event is delivered
  (see the shutdown section). The stop token is the only cancellation
  channel; handle sweeps destroy completed tasks only (D6).
- **In-flight requests are never `QNetworkReply::abort()`ed by
  cancellation.** The server has already counted an in-flight request
  (N25: state headers are post-increment, 1:1); aborting it would
  discard the reply's headers and leave the pacing history missing an
  event the server's counters include — exactly the client-model
  divergence P-A warns against. A stopped in-flight request lands,
  is recorded in history and capture (a 429 still records its
  violation — D3), and then completes `Canceled`. **`abort()` is
  never called at all (R6-1):** its one remaining purpose — waking
  awaiters at shutdown — vanished when R5-2 made shutdown stop
  delivering events to the awaiters. At shutdown a still-in-flight
  reply is cleaned up by its `QNetworkAccessManager` parent — the
  only shutdown cleanup, since the owning frame is detached, not
  destroyed, and its RAII wrapper never runs there (S1-3); the
  wrapper owns the reply on every live-session path.
- **No future retention** (resolves ER5): the
  worker does not keep a registry of outstanding futures — the
  stop_source is the abort handle. Each per-fetch coroutine holds its
  future as a local, whose shared state is released when the task
  ends; completed results are not accumulated anywhere a multi-hour
  update could bloat. The facade likewise retains no parent futures
  after chaining.
- This deletes the worker's abort machinery: the five `m_queue = {}`
  sites, the counter-snapping, `DiscardIfStale`, and the generation
  tag (see D6).

### D3. Policy managers become coroutine pumps

**Revised July 19 (IR round):** each `RateLimitManager` owns a plain
`std::deque` of entries and an **on-demand drain coroutine** — the
earlier long-lived awaitable FIFO is deleted (its retention rationale
was circular; see the review history). Submission pushes an entry and starts the
drain task if none is running; since submission is main-thread-only
(the `Q_ASSERT` stays), "is one running" is a plain bool with no
races. The drain processes entries until the deque is empty, then
returns. There is no permanently suspended task and no queue-close
protocol.

Illustrative shape (the attempt table below is normative):

```
drain():                                   // started by submit when none is running
    while not m_deque.empty():
        entry = m_deque.pop_front()
        if stopped(entry): complete Canceled; continue
        co_await sleepUntil(m_policy->GetNextSafeSend(m_history), entry)  // stop-interruptible
        if stopped(entry): complete Canceled; continue
        for attempt in 1..MAX_ATTEMPTS:            // 3 total sends: 1 original + 2 retries
            permit = co_await m_gate.acquire()     // global gate, D5
            if stopped(entry): complete Canceled; break
            reply = own(permit.dispatch(entry))    // RAII owner installed before any await (R6-1)
            co_await reply.finished()              // permit released when the reply finishes (D5)
            // no permit is held past this line — a retry sleep is permit-free (R4-1)
            record history; feed capture
            if 429: record violation               // always — before stop/retry decisions
            if Full headers and policy name matches: m_policy->Update(headers)
            emit signals
            outcome per the attempt table; retryable 429 sleeps and continues,
            everything else breaks with a final outcome
        reach a stable pump state; only then complete the promise
        (the permit was already released at reply-finish — D5)
```

**Per-send bookkeeping (always first):** every landed reply records
its history event and its capture record, and every 429 records a
violation (counter + log) — *before* the stop check and *before* any
retry decision, because the server counted the exchange (N25)
regardless of what the client does next. **Observation follows
bookkeeping and precedes classification (R6-3, D8):** the rate-limit
headers are parse-attempted on every landed reply regardless of
status, and a Full set with a matching policy name updates pacing
state — 429s, 403/500s, and a 2xx-plus-transport-error all keep the
policy current (today's code updates on every headered reply; the
server counted them all, N25).

**Attempt table (normative):**

| Landed reply (after bookkeeping) | Outcome |
|---|---|
| entry stopped (its token) | complete `Canceled` (the violation of a stopped 429 is already recorded) |
| 429, valid `Retry-After`, attempt < `MAX_ATTEMPTS` | stop-interruptible sleep to `max(now + RetryAfter + RETRY_BUCKET_PAD + buffer, GetNextSafeSend(history))`, then re-send |
| 429, valid `Retry-After`, attempt = `MAX_ATTEMPTS` | complete `FetchError{RateLimited}` **immediately — never sleep on an exhausted attempt** |
| 429, no acceptable `Retry-After` (missing, non-numeric, negative, or above the cap) | complete `FetchError{RateLimited}` (non-retryable; violation already recorded; unified kind — R7) |
| any other non-2xx status | complete `FetchError{Http}` |
| Qt transport error (incl. alongside a 2xx — D8 precedence) | complete `FetchError{Network}` |
| clean 2xx, Full headers, matching policy name | `Update(headers)`; complete success (payload) |
| clean 2xx, headers fail to parse **or** mismatched policy name | complete `FetchError{Protocol}` (strict — D8/IR1) |

A matching policy name may legitimately arrive with a different counter
shape. The legacy stash endpoint does this when a request moves between an
unauthenticated POESESSID response (`Ip`) and an authenticated response
(`Account,Ip`). The pump adopts the new shape and clears its local request
history before pacing against it: those events describe a different set of
counters. This handled transition is logged at `info` so setting a valid
POESESSID does not surface a warning in the UI; full old/new policy reports
remain available at `debug`. A shape is the ordered rule names and each
rule's ordered bucket periods; hit and restriction changes within that shape
remain ordinary definition updates and retain history.

- The retry is invisible to callers: the future completes only on a
  final outcome — success, non-retryable error, or retries exhausted.
  P-A (graceful 429 recovery is a first-class requirement) is satisfied
  structurally: a 429 can no longer wedge anything (F57).
- **Retry timing (ER7, resolved):** the retried send is scheduled at
  `max(now + RetryAfter + maxBucket + buffer, GetNextSafeSend(updated
  history))`, and it passes the gate like every send — so the three
  deadlines (server's, policy's, gate's) reconcile by construction.
  `maxBucket` is an **unconditional named constant,
  `RETRY_BUCKET_PAD_SECS = 60`** (round 2 — "the policy's maximum
  bucket" still required classifying rules, and the legacy policy's
  bucket tiers are unknown, N21b): no policy inspection at all, so the
  retry path is genuinely immune to the initial-vs-sustained question
  (Q4). Recorded assumption: 60 s is the largest bucket tier GGG has
  named for any policy (N12) and is assumed to be a ceiling for the
  legacy policy too — the N14 ask-GGG channel is the remedy if
  evidence ever contradicts. A rare retry over-pays ≤ 55 s for this;
  accepted. (N19's padding remains provisional pending Q9 capture
  data either way.) `Retry-After` — **grammar vs product policy
  (reworded R7; the old [1, 900] "validity" window mislabeled both
  ends):** the grammar accepts any nonnegative integer — RFC 9110
  delay-seconds permits 0 and arbitrary lengths; 0 is valid and
  costs nothing (the pad and `GetNextSafeSend` dominate the
  formula). A **product-policy cap**, `RETRY_AFTER_CAP_SECS = 900`
  (longest observed restriction 600 s, N23, plus headroom), declines
  longer waits as terminal `RateLimited` — not because they are
  invalid HTTP but because obeying a never-observed multi-hour wait
  is continuation through the unexpected; the pause would be visible
  (`Paused`) and stop-interruptible and permit-free, so nothing
  wedges — containment simply prefers a clean, actionable error.
  Missing, non-numeric, and negative values are non-retryable the
  same way (terminal `RateLimited`, D8).
- **Retry sleeps are permit-free (R4-1):** the permit is released the
  moment the reply finishes (D5's permit span) and each attempt
  re-acquires the gate — a permit is never held across a retry sleep.
  A retry sleep can run to ~961 s (900 + 60 + 1); holding one of the
  gate's two permits through it, with a waiting HEAD blocking the
  rest under writer preference, would stall the entire hub behind one
  429 — a gate-shaped F57.
- **Raw reply ownership (R5-4; corrected R6-1 — rev 5's
  install-at-await-return left the in-flight span, the reply's
  longest phase, ownerless):** `NetworkManager` never enables Qt's
  auto-deletion (default false), so ownership must be explicit — the
  contradictory-ownership mistake this redesign exists to kill (F59)
  must not be re-created one level down. The RAII wrapper (a
  `deleteLater` deleter) is installed **at dispatch, before the
  first await** — dispatch and await are separate steps — making the
  wrapper a local in the pump's frame. The frame owns the reply on
  **every live-session path**, including through suspension: the
  wrapper runs when the coroutine body completes and its locals
  unwind (normal, error, exception, cancellation-after-landing) —
  the frame *allocation* itself lingers until the retained handle
  is swept (D6), but its locals are already gone by then.
  At shutdown the frame is *detached*, not destroyed (S1-1), so the
  wrapper never runs there — the reply's `QNetworkAccessManager`
  parent, destroyed after the hub, is the sole shutdown cleanup
  (S1-3); still no registry and no abort. Body copied and headers
  consumed before any completion or retry decision. Every
  live-session path — success, error, each retry attempt, exception
  (composes with the completion guard), cancellation-after-landing —
  cleans up through that one wrapper; no per-path deletion code.
  The gate permit observes `finished` but never owns or deletes the
  reply. The setup path owns its HEAD reply under the same rule
  (D4).
- Retries are bounded (`MAX_ATTEMPTS = 3`) so a systemically broken
  policy cannot hammer the API — each 429 still increments the violation
  counter and logs (N10: layer-4 goodwill is finite).
- **Completion ordering (IR6):** the promise is completed only after
  the permit is released and the pump has reached a stable state —
  completion can synchronously run facade parsing and worker
  continuations (D6), which may immediately re-enter the limiter.
- `RateLimitPolicy` and its history-based arithmetic are untouched —
  N25/N26 confirmed the pacing empirically exact **for the captured,
  saturated policies (scoped R7)**; Q4's initial-vs-sustained
  classification remains a known, independent risk, deliberately out
  of scope (see the items-pipeline interaction section). The pump
  calls the same `GetNextSafeSend(m_history)`.
- Existing UI signals (`PolicyUpdated`, `QueueUpdated`, `Paused`,
  `Violation`) are preserved — the status bar and rate-limit dialog keep
  working. The capture instrument keeps its hooks (record every
  exchange including retries, `scheduled` updated on retry).
- Requires two small primitives (new, `src/ratelimit/`), unit-tested
  standalone with an injected clock: a **stop-interruptible sleep**
  (QCoro's sleeps take no stop token, so this is required regardless
  of queue design) and the gate (D5). The deque + drain is ordinary
  code in the pump, not a primitive.

### D4. Endpoint setup: probe once, establish or fail cleanly

**Rewritten July 19, 2026 (simplification pass).** The Discovery state
and everything built on it — the discovery GET, adoption-seeding
rules, the discovery-429 split, the setup-cancellation promotion
protocol — are **deleted** (reversal recorded in the review
history). Every
endpoint the hub knows is in exactly one state:

```
Unknown → Probing → Established
```

with any setup failure returning to `Unknown` under a cooldown. No
provisional pump object exists in any state; two pumps can never serve
one policy (ER2 resolved structurally).

- **Unknown → Probing:** the first submission for an unknown endpoint
  parks in the hub and fires the HEAD asynchronously (no nested event
  loop — the `QEventLoop` block in `SetupEndpoint` is deleted; the
  main-thread `Q_ASSERT` on submit stays). Submissions arriving
  mid-setup park behind it, keeping submission order.
- **HEAD exclusivity is enforced at the gate (D5):** a HEAD acquires
  the gate exclusively — nothing else in flight while a probe runs.
  This preserves N18/F5 **deliberately** rather than as a side effect
  of accidental serialization, and it holds even with multiple
  endpoints setting up concurrently (each HEAD serializes at the
  gate). The F5 standing constraint is amended, not violated: one HEAD
  at a time, ever, is now a property of the gate.
- **Probing → Established** (the HEAD returns a **Full** header set,
  per D8): create-or-join the pump by policy name (N6: same-named
  policies share counters; the observed topology is strictly 1:1 —
  five endpoints, five policies, N23 — but the join is a map lookup
  and it is the documented contract). A probe that joins an existing
  pump updates that pump's policy under the same rule as any reply
  (Full headers, matching name — D3/D8; R4 minor). Parked entries
  forward to the pump in submission order.
- **HEAD 429 with Full headers and a valid `Retry-After`** (genuinely
  reachable: counters persist across restarts, N24, so booting during
  an active restriction can 429 the boot probe; GGG itself floated
  HEAD-after-429 resync, N16): the reply still teaches the topology —
  establish the pump, with its first send scheduled no earlier than
  `now + RetryAfter + RETRY_BUCKET_PAD + buffer` (the D3 formula's
  degenerate case); the HEAD consumes no send attempt. The 429 is a
  real server-side violation and is recorded — counter, log, capture
  — like any pump-path 429 (R4 minor). A Full HEAD
  429 whose `Retry-After` is missing or unacceptable (per D3's
  grammar and cap; IR5/R7) is a **setup failure** under the cooldown
  below — containment declines to invent a hold time.
- **Setup failure — anything else:** a HEAD that fails in transport,
  returns a non-2xx status (including any 429 not covered above), or
  returns 2xx/204 with headers that fail to parse. Every parked entry
  for that endpoint completes with the corresponding `FetchError`
  (`Protocol` for unparseable headers, `Network` or `Http` otherwise);
  the failure is logged at error, surfaced as a user-visible
  notification, and captured (probes are captured before validation —
  already implemented); the endpoint resets to `Unknown` under the
  cooldown below. In a worker update the first failure also triggers
  `request_stop()`, washing the update's remaining entries through
  `Canceled`.

  **This is the July 19 reversal of the first round's
  degrade-and-proceed decision** (Tom's call, under the containment
  principle). The deleted discovery fallback existed for a server-side
  HEAD-regression class last observed in Dec 2023 (N20) — fixed after
  Tom's report, confirmed healthy on all five endpoints in the first
  instrumented capture (N24), and with GGG aware acquisition depends
  on the mechanism (N16, N17). If it ever regresses again: updates
  fail with a clear error, the capture records the regression's actual
  shape from the first affected session, and a fallback can then be
  built against evidence instead of guesses. Two invariants bound the
  failure mode: it must never crash (the D8 total parse removes
  today's UB path — a degraded reply crashes the current parser) and
  it must never run unpaced. A synthetic default policy remains
  rejected (inventing pacing numbers GGG never sent). `FatalError`
  leaves the hub entirely.
- **Setup-failure cooldown** (round 2): every failure-driven reset to
  `Unknown` records `earliest_next_probe = now +
  max(SETUP_RETRY_COOLDOWN, validated Retry-After when one was
  present)`; submissions during the cooldown fail fast with the prior
  failure's kind, no probe sent — a caller resubmitting from its
  completion handler cannot loop HEAD probes against N16's narrow
  sanction. `SETUP_RETRY_COOLDOWN = 60 s`, named, provisional.
- **Setup cancellation:** parked entries whose token stops are pruned —
  completed `Canceled` — without affecting the endpoint's state. An
  in-flight HEAD is never aborted (D2's let-it-land rule; R6-1
  removed even the shutdown abort): its reply is processed for
  topology and capture even if every
  parked entry has been canceled — knowledge the server already
  charged us for is kept. A probe that succeeds with no live entries
  simply leaves the endpoint `Established` and idle; a probe that
  fails applies the normal failure path (the cooldown is
  failure-driven, not entry-driven). Cancellation alone never installs
  a cooldown.
- **Established is sticky:** an established endpoint keeps its mapping
  through ordinary request failures (a timeout must not discard valid
  topology and burn a HEAD — N16's sanction is one HEAD at boot), and
  **no steady-state event resets topology**: a steady-state `Protocol`
  error (D8) fails the request, not the endpoint, and self-heals — if
  the server recovers, the next update's Full replies update the
  policy normally. Pumps, once created, live until shutdown; a pump
  with an empty deque simply has no drain task running. The hub's maps
  cannot go stale: a pump's policy name never changes after creation
  (D8's update rule), unlike today, where `Update()` swaps the policy
  while `GetManager`'s maps keep the old key forever.
- **Ordering and identity during setup:** parked entries keep their
  own submission order. Capture records and queue-status signals label
  parked traffic by endpoint until the endpoint is established, then
  by policy name.

### D5. The gate: one object for layer 1

**Scope resolved July 19 (ER3) by shrinking the promise; contract
sharpened and forum traffic removed later the same day (IR round).**

The gate is **internal to the hub**: every send the pumps and the
setup path dispatch — the five rate-limited endpoints (the four API
endpoints plus the legacy stash index) — acquires it. It has no
public API. Three properties, each a named constant, plus three
contract rules:

- **In-flight cap: 2.** P-B requires re-parallelization to state its
  global burst bound explicitly; this is it. The point is to stop
  idling policy lanes (F56), not to maximize concurrency — under N4's
  "seconds per request" reality, lanes are rarely ready simultaneously,
  so 2 already captures nearly all of the win. Tunable; revisit with
  capture data.
- **HEAD exclusive, with writer preference (IR6):** a HEAD probe takes
  the whole gate (N18, F5, N2), and once a HEAD is *waiting*, no new
  ordinary permits are issued — without this, a busy established pump
  could starve endpoint setup indefinitely.
- **FIFO among ordinary waiters (R7):** ordinary permits are granted
  in arrival order — no bypass. Without this, a hot stash pump could
  repeatedly beat a waiting character pump at every release: lane
  starvation in miniature, exactly what the gate exists to prevent
  (F56-adjacent, not hypothetical). The HEAD writer preference is
  the only queue-jump.
- **Minimum inter-send spacing: 250 ms** across everything the gate
  sees. This is F58's intent — silently dead today — implemented
  deliberately at the right scope: it flattens the ~0.2 s intra-burst
  spikes N26 observed (≈5 req/s) to ≤4 req/s without materially slowing
  small updates (5 tabs: ~1.25 s vs ~1 s). Layer 1 triggered on a burst
  of ~17 req/s sustained (N2); this keeps normal traffic an order of
  magnitude inside that. Tunable; revisit with capture data. F58's
  dead code is deleted.
- **Permit span (IR6; sharpened R4-1):** a permit is held from
  acquisition until the reply finishes (`QNetworkReply::finished`) —
  released at dispatch, the in-flight cap would bound nothing — and
  never longer: release happens at reply-finish, so a permit is never
  held across a retry sleep (D3).
- **Liveness rests on the transfer timeout (R4-4; corrected R5-3):**
  the gate's liveness — permit turnover, HEAD exclusivity ever
  becoming acquirable, prompt drain of stopped in-flight entries
  under the never-abort rule (D2) — depends on replies not stalling
  indefinitely. **Precision (R7):** Qt's transfer timeout is an
  *inactivity* bound — it aborts a transfer only after a period with
  no bytes exchanged, not after a total duration — so it bounds the
  stall class (the observed one); a byte-trickling reply could hold
  a permit longer, accepted (never observed, and a speculative
  absolute deadline is exactly the machinery containment declines to
  build). R4 called the 10 s timeout "existing behavior";
  that was **false for the legacy stash-index request**, which today
  has no transfer timeout at all (`Shop::UpdateStashIndex` builds a
  bare request; only the OAuth builders set one — F60). The invariant
  is therefore *established by the facade* (D7), which sets the
  timeout on every request it builds — all five — with a test over
  every builder. Removing or unbounding the timeout requires
  redesigning the gate.
- **Completion ordering (IR6):** the permit is released, and the pump
  reaches a stable state, *before* the entry's promise is completed —
  promise completion may synchronously re-enter the limiter (D3, D6).

**Scope rationale:** layer 1 watches the user's IP (N2, N22), and the
traffic that can plausibly burst is exactly the traffic the hub
paces. **Login and OAuth traffic stays outside**: the login league
list and the OAuth authorize/token exchanges precede the hub, and
mid-session token refreshes add a handful more (R4 wording fix —
refresh is not pre-session, but the magnitude argument is
unchanged); layer 1's one known trigger was over a thousand requests
in a minute (N2), three orders of magnitude away. **Forum traffic is likewise outside (IR round,
reversing this spec's earlier revisions):** `Shop` is strictly
sequential today — one thread at a time with deliberate delays
(`shop.cpp:388`) — user-triggered, and has never brushed layer 1; the
same magnitude argument applies. Gating it bought a public permit
API, external reply-lifetime tracking, and shutdown obligations for
non-hub code, for no demonstrated risk reduction. Non-GGG hosts
(GitHub, imgur, Sentry) and CDN image traffic (poecdn, webcdn) are
likewise outside — they are not API infrastructure. P-B still holds:
the stated global bound is the gate cap over the parallel lanes, and
forum adds at most one slow, serialized request on top.

This resolves the Q10b design question: the redesign *coordinates* the
API and legacy regimes (both flow through the hub) and *tolerates*
forum and login traffic as-is, documented here.

### D6. The worker: batch submit, count completions, no flow control

- "Submit all selected fetches" is **staged by discovery, not paced by
  the worker**. `All` and `TabsOnly` require both fresh lists; a partial
  refresh requires only the location types represented by its selection,
  preserving M1. Required list requests are launched without awaiting one
  another. The complete stage is initialized before its first launch and every
  member is invoked even if an earlier synchronously ready/fail-fast result
  stops the update; later submissions receive that stopped token and complete
  through the ordinary cancellation path. As each required list result is
  processed, it independently defines and launches that list's complete
  content batch; character content is never held behind the stash list. Folder
  children discovered in the stash list join that list's batch. Map/Unique
  children discovered in a parent content reply are appended as one complete
  child batch. There is no wait between entries inside a batch.
- Per-policy FIFOs preserve the source traversal order that defines each
  batch: fresh-list/model order, recursive Folder-child order, and
  parent-reply child order. There is no global order between the independent
  stash and character lanes, and request priority never comes from unordered
  container iteration. Reprioritization stays out of scope (M2+ — and it is
  **not** cheap later, R7: the stop token is per-update, so per-entry
  cancellation does not exist; reprioritization would need a new mechanism,
  deliberately not designed now).
- Worker-side serialization is explicitly rejected: one-at-a-time
  submission *is* F56 — it recreates idle policy lanes by construction.
  Batching is also strictly less state: no `m_queue`, no
  `SubmitNextItemRequest`, no re-entrancy dance.
- Orchestration becomes coroutines (`QCoro::Task<>` methods,
  `co_await` on facade futures): the per-fetch callback *pyramid* collapses
  into one flat coroutine per fetch, each reading top-to-bottom — await its
  facade future, check the token, process. The root does **not** linearize
  into a single top-to-bottom await, and cannot: this section's own
  requirements — required lists launched without awaiting one another, each
  list's content launched from its own handler and never held behind the
  sibling list, child batches discovered dynamically in replies — make the
  update a growing, data-dependent fan-out, not a static `co_await` sequence
  over a known set. So the root launches the required lists and returns; each
  per-fetch coroutine self-drives (process, launch any follow-on batch,
  increment its counter), and the completion that reconciles the counters
  finalizes the update (counter-driven join, rev. 10). Completion counting
  (`m_stashes_received` etc.) stays — it drives both the status bar and
  finalization.
- The flag pairs do **not** all vanish, and the `m_has_*` list-arrival flags
  cannot: under a counter-driven join `received == needed` is trivially true
  for a required-but-empty list (an empty list, or a `TabsOnly` update that
  fetches no content), so a separate "the list itself arrived" signal is
  required to keep finalization from firing before the list lands — and it is
  also what the concurrent content-during-list-arrival window above depends
  on, since a ready content fetch can complete while its sibling list is still
  outstanding and the finalization check it triggers must know the sibling has
  not arrived. `m_need_*` is derived once from the selection (config, not
  callback state). What collapsed is the nested-callback pyramid; the residual
  flags are the minimal list-arrival state the counter-driven join needs
  (rev. 10).
- **Task topology and handle ownership (R4-3; rev. 10):** the root
  orchestration is a synchronous method that never awaits, so it owns no
  task handle; the worker owns one per-fetch `QCoro::Task<>` per
  submission, held in an owned handle container — no fire-and-forget
  anywhere. A task handle owns a coroutine
  frame, not a payload, so this does not conflict with D2's
  no-future-retention rule; 2,000 frames are memory, which the
  phase-0 measurement weighed: ~6.5 KB per entry for the whole
  standing machinery (entry + promise + chain + suspended frames +
  watcher + handle, outcome slots sized to the real stash wrapper),
  ~12.4 MB at the 2,000-tab scale, linear to
  10,000 (S2-1) — acceptable. Ownership is for
  **lifetime control only** — it supervises nothing (R5-1, exception
  policy). Handles are reclaimed by a **deferred sweep that runs
  outside any completing coroutine** (R5-1): finalization triggered
  by the last completion must not clear the container holding the
  frame it is running in, and **abort never destroys live handles**
  — rev 4's released-on-abort rule contradicted D2's
  stragglers-resolve-later contract; stragglers finish `Canceled`
  (bounded by the transfer timeout) and are then swept. The sweep
  destroys **completed** tasks only — destroying a suspended task's
  handle would detach it, not stop it (S1-1/S1-4), and the frame
  would still resume later while the loop runs. No
  incremental per-completion reclamation beyond that sweep — the
  measurement did not demand it: sweeping 2,000 completed frames
  takes ~0.3 ms and releases everything (heap back to baseline,
  zero leaks — S2-2, S2-3). "Update done" remains completion counting:
  each per-fetch coroutine increments its counter after its await
  resolves (post-await check first), and the completion that
  reconciles the counters triggers finalization, exactly as today's
  handlers do.
- **Batch-submit signal volume (R4 minor):** submitting thousands of
  entries fires as many synchronous `QueueUpdated` emissions in one
  loop. Not a correctness issue; if the status dialog measurably
  stutters, coalesce on the UI side (M2's coalescing work), not in
  the limiter.
- **Post-await identity invariant (IR2):** the update token is the
  update's identity *only because* every worker coroutine checks
  `stop_requested()` immediately after every `co_await`, before
  touching worker state. A future can complete successfully an
  instant before `request_stop()`; its consumer resumes afterward and
  must not mutate state that now belongs to a later update. Each
  per-fetch coroutine has essentially one await site, so the check
  surface is small; pinned with ready-future tests.
- **Initialize-before-launch invariant (IR2; spike-verified S1-6):**
  Qt runs `.then` continuations synchronously when attached to an
  already-finished future, and QCoro resumes without suspending on
  ready futures — so a fail-fast submission (setup cooldown,
  shutdown) can run completion logic *during the batch-submit loop*.
  All per-update counters and batch state are initialized before the
  first submission is made. Pinned with ready-success and
  ready-error futures.
- **Payload extraction (S1-7):** the worker's single-consumer path
  moves the parsed payload out with `co_await
  qCoro(future).takeResult()` (0 copies, spike-measured); plain
  `co_await future` copies via `QFuture::result()`.
- **Update-state policy:** an `Update()` received while an initialized update
  is active is refused: it submits nothing and neither stops, replaces, nor
  queues behind the active update. Deferral while the worker is still
  initializing remains a separate existing behavior. Whole-update
  replacement/coalescing is an M2 concern alongside reprioritization and
  durable progress.
- Failure semantics at the update level are unchanged from M1: a terminal
  failure aborts the update (`request_stop()` on the update's stop_source,
  no `ItemsRefreshed`, no rebase). Replies already processed and atomically
  applied before the failure remain applied; a consumer that resumes after
  stop applies nothing. Canceled siblings return silently, do not add request
  failures, and cannot produce another terminal transition. The first terminal
  failure returns the worker to idle immediately rather than waiting for those
  siblings, so a new update may be active while stopped tasks from the old one
  are still settling. Completion/status counters remain monotonic and include
  each child before that child is launched. Per-tab retry/durable progress
  remain M2 concerns and are not blocked by anything here.
- The update generation tag is deleted outright, not shrunk (revised
  July 18): each update owns its stop_source, and identity is carried
  by the token under the post-await invariant above. A straggler from
  a stopped update resolves `Canceled` and its coroutine returns
  silently, and a straggler that resolved successfully is discarded by
  the invariant's check. `DiscardIfStale` and `m_update_generation`
  go away.

### D7. The typed facade: `PoeApiClient`

New class (suggested home `src/poe/poeapiclient.{h,cpp}`), owning the
boundary between domain and network:

- Methods (mirroring today's five rate-limited calls): `listStashes`,
  `getStash`, `listCharacters`, `getCharacter`, `getLegacyStashIndex`.
  Each takes the caller's `std::stop_token` (D2; callers with no abort
  story pass a default never-stopped token), builds the request
  (absorbing `poe::MakeStashRequest` et al.),
  submits to the limiter with the token attached, and chains parsing
  (absorbing the `json::read*Wrapper` calls and their error handling)
  into a `.then()` continuation. Returns
  `QFuture<std::expected<Payload, FetchError>>`.
- **The parse continuation is exception-tight (IR4):** the `.then`
  body wraps its work in a catch-all that converts any escaped
  exception into `FetchError{Parse}` (or `Internal` for a non-parsing
  failure) — otherwise a throwing continuation produces an
  exceptional `QFuture`, which crosses the value-only boundary and
  rethrows out of `co_await`.
- The worker never sees `QNetworkRequest` or `QNetworkReply`, and
  nothing above the boundary *interprets* bytes; networking never sees
  items. The compiler enforces the boundary. (Amended July 28, 2026,
  with the F62 fix — the original statement was "the worker never sees
  bytes": the facade captures each stash/character reply's sub-object
  losslessly (`glz::raw_json`), parses the typed payload from that
  same substring, and returns both; the worker couriers the bytes as
  one opaque `QByteArray` per reply to the datastore, which persists
  them verbatim so the cache is a faithful record of the API. The
  worker never inspects the blob; parsing bytes above the facade is
  still forbidden. Full decision in the F62 ledger entry,
  `docs/cleanup/findings.md`.)
- The facade is intentionally boring: no coroutines inside, no state
  beyond a reference to the limiter — account/realm/league are call
  parameters, so there is no `QSettings` reference and the facade is
  genuinely stateless (R7).
- **The facade owns the transfer-timeout invariant (R5-3):** every
  request it builds carries the 10 s transfer timeout — the gate's
  liveness depends on it (D5). Today only the OAuth builders set it;
  the legacy stash-index request is built bare (F60) — the facade
  closes that gap by construction, and a test covers every builder.
- **Everything runs on the main thread (R4 minor, stated
  explicitly):** submission is main-thread-only, pumps complete
  promises on the main thread, and the parse continuations attach
  with no threading context, so they run there too — where today's
  wrapper parsing already happens. This is an invariant, not an
  accident: giving a continuation a threaded context invalidates the
  single-threaded assumptions in D2–D6.
- `Shop`'s legacy stash-index call moves to the facade
  (`getLegacyStashIndex`); the forum-thread POSTs keep their own path
  entirely — scrape-based protocol and sequential pacing unchanged
  (N22), outside the gate (D5). `Shop` itself stays callback-style —
  no coroutines: it consumes the future with a **context-bound
  continuation** (`.then(this, …)`), which Qt discards if the
  context is destroyed — the future-flavored equivalent of the
  `this`-context signal connection that makes it safe today
  (`shop.cpp:199`) (R6-2).
- **Ownership (R6-2):** `PoeApiClient` is a `UserSession` member
  declared immediately after `rate_limiter` — destroyed after every
  consumer (worker, `Shop`), before the limiter, by the same member
  order shutdown rides on. Its parse continuations capture values
  and free functions only, never the facade itself, so the facade's
  lifetime cannot matter to an in-flight chain.

### D8. Response classification and header validity

Added July 18, 2026 (group 2 — ER6, ER8). Simplified July 19 (tiers
collapse to Full vs. not-Full; the steady-state threshold machinery
deleted), then **corrected in the IR round: steady-state anomalies
are strict errors, and validation and parsing are one total
function.**

**Total parsing (IR simplification).** Header validation *is* the
parser: a total factory in the shape
`RateLimitPolicy::parse(headers) → std::expected<RateLimitPolicy,
error>` replaces the current throwing/UB constructors. A separate
validator in front of the existing parser would implement the same
grammar twice and let the two drift. The arithmetic methods
(`GetNextSafeSend` and friends) are untouched — empirically
validated for the captured, saturated policies (N25, N26; Q4's
classification risk stands apart, R7). **Full** means the parse
succeeds, which requires (tightened R7): a nonempty policy name; a
nonempty rules list; every rule name nonempty, with at least one
item and limit/state lists of equal length; every triplet exactly
three in-range integers — limit: hits > 0, period > 0, restriction
≥ 0; state: hits ≥ 0, period > 0, restriction ≥ 0 — and each state
period matching its limit period. (Limit hits must be positive: a
zero-hit limit is meaningless as a lookback and was a live
divide-by-zero in the now-deleted `EstimateDuration`; state hits
legitimately start at zero, N24.)
(July 19: the former `NameOnly` tier and its join-an-existing-pump
shortcut are deleted — the observed topology is strictly 1:1, N23.
The policy name, when present in a failed parse, is logged and
captured; that is the detection the containment principle asks for.)

**Only a Full reply whose policy name matches the pump's updates the
policy**, and nothing downstream ever indexes an unvalidated triplet.
This deletes a live UB path found while verifying ER6: today a missing
header parses to a one-element `[""]` list (Qt's `split` on an empty
array), passes `RateLimitRule`'s only size check, and `RateLimitData`
reads `parts[1]` out of bounds (`ratelimitpolicy.cpp:52`) — so the
ground-truth ledger's N20 claim that a degraded HEAD "runs unpaced"
was wrong; it crashes. (Ledger corrected in place, July 18, 2026.)

**Observation before classification (R6-3).** Header parsing is
attempted **independently on every landed response**, whatever its
status: a Full set with a matching policy name updates pacing state —
a 429, a 403/500, and a 2xx followed by a transport error all keep
the policy current (the server counted them, N25; today's code
updates on every headered reply — a design that observed only 2xx
replies would let pacing state go stale across a run of errors while
history grows). The classification below is status/network-driven
and decides only the caller's outcome; missing or malformed headers
become `Protocol` **only** where the response would otherwise be a
clean 2xx success. Observation never changes an outcome;
classification never blocks an observation.

**Classification precedence** (ER8; amended in round 2 — a 2xx status
must not override a transport error, because Qt can report a status
and then fail mid-body: `RemoteHostClosedError` is defined as the
remote closing "before the complete response was received"):

1. Status 429 → the rate-limit path (D3), regardless of any reply
   error Qt also raises for it (`Retry-After` is validated either way).
2. Any other non-2xx status → `FetchError{Http}` carrying the status
   (Qt reports many of these as reply errors too; the status governs).
3. Any remaining Qt error — **including alongside a 2xx status** (the
   truncated-body case), and the no-status-at-all transport failures
   (connection refused, timeout, SSL, …) → `FetchError{Network}`,
   carrying the Qt error code and string.
4. Clean 2xx → success if the observation above parsed Full with a
   matching policy name; otherwise `Protocol`:

**Steady-state anomalies are strict errors (IR1 — reversing the
simplification pass's log-and-continue rule).** A clean 2xx whose
rate-limit headers fail to parse, or parse to a policy name that does
not match the pump's, records its history event and capture record,
logs loudly, and completes `FetchError{Protocol}`. The
log-and-continue rule was unsound: `GetNextSafeSend` is
*state-driven*, not history-driven — it returns "send now" whenever
the stored status is below BORDERLINE (`ratelimitpolicy.cpp:274`) and
consults history only for rules the stored state says are saturated.
With updates frozen at an OK state, the pump would send at gate speed
until the server 429s, retry, succeed with another degraded reply,
and resume unpaced — *repeated* violations (N10), not P-A's priced-in
mistimed request. Strictness is also the only honest boundary:
`expected<Payload, FetchError>` cannot deliver a payload and flag an
anomaly at once. The worker's existing first-failure `request_stop()`
ends the update cleanly; the endpoint stays `Established` and
self-heals (D4). No threshold, no counters, no reset — this is
containment at its simplest: one anomalous reply, one clean error.

**429 under the tiers:** `Retry-After` is the retry authority — a
steady-state 429 whose rate-limit headers fail to parse still retries
per D3 (the pump has a policy for the deadline formula) but does not
update the policy; a successful retry whose reply then fails to parse
completes `Protocol` per the rule above. A 429 with no acceptable
`Retry-After` (missing, non-numeric, negative, or above D3's
product cap) is non-retryable: `FetchError{RateLimited}`, violation
recorded — **one kind for every terminal 429 (R7)**, carrying the
status, the attempt count, and whether an acceptable `Retry-After`
was present; the R4-documented `Http{429}`/`RateLimited` split is
deleted rather than apologized for. HEAD 429s are handled in D4.

**`FetchError` shape:** kind (`Network` / `Http` / `Parse` /
`Protocol` / `RateLimited` / `Internal` / `Canceled`), endpoint, URL,
HTTP status (when present), Qt error code and string (when present),
retry-attempt count and Retry-After acceptability (`RateLimited`,
R7), message. F50's diagnostics rewording rides along here.

## Shutdown and task ownership

Added July 18, 2026 (ER4); rewritten in the IR round (IR3, IR4);
rewritten July 19 (R5-2: the asynchronous teardown choreography is
deleted); **mechanism corrected by the phase-0 spike (S1-1..S1-4):
shutdown safety comes from detach-plus-no-delivery, not from
destroying suspended frames.** Rev 4 required queued resumptions and
a live event loop during shutdown — but `Application` is destroyed
after `a.exec()` returns (`src/main.cpp`), when the loop is already
gone, and `UserSession`'s member order destroys `Shop` and
`ItemsManagerWorker` *before* `RateLimiter` (`src/application.h`), so
a hub-driven drain of worker tasks was unreachable. Rather than build
an asynchronous shutdown subsystem for consumers that are already
destroyed, shutdown is now **destruction order, not choreography** —
which the existing member order already implements.

**What destroying a task handle actually does (S1-1):** QCoro 0.13
tasks are reference-counted — the promise holds its own reference
until `final_suspend`, the handle holds a second. Destroying the
handle of a suspended coroutine *detaches* it: the frame survives,
its locals' destructors do not run, and it resumes normally if its
awaited event is later delivered, self-destroying at completion.
There is no API to destroy a suspended coroutine from outside.
Handle ownership therefore controls the lifetime of *completed*
frames only; it neither cancels nor stops a live one (D2).

**Ownership chain:** the hub owns the gate and the pumps; each pump
owns its deque and its drain-task handle (a member — never
fire-and-forget); the worker owns its per-fetch handles as members, and
its root orchestration is a synchronous method that owns no handle (D6,
rev. 10); the facade owns nothing stateful.
The hub lives for the application session. There is **no shutdown
`std::stop_source`** — every wait takes the entry's token only, one
token in every signature (R5-2 simplification; rev 4's second token
existed solely for the deleted choreography).

**Shutdown = destruction, in the existing order:**

1. Consumers die first (`UserSession` member order): `Shop`, then
   `ItemsManagerWorker`, with the facade following (R6-2), still
   ahead of the hub. `Shop` has no tasks — its context-bound
   continuation dies with it (D7, R6-2). The worker destroys its
   task handles: completed frames are destroyed outright, suspended
   frames are **detached** (S1-1). Neither ever resumes, because
   nothing delivers events after `a.exec()` returns — see below.
   Within each destructor, task handles are destroyed before any
   member they might observe.
2. The hub dies next: its destructor destroys every drain-task and
   setup-task handle (detaching any suspended frame — nothing
   resumes into a dying pump). A still-in-flight reply is cleaned up
   by its `QNetworkAccessManager` parent, destroyed after the hub —
   the **only** shutdown reply cleanup (S1-3): the detached frame's
   dispatch-time RAII wrapper never runs, and post-loop `deleteLater`
   is inert anyway. **Nothing is `abort()`ed and the hub tracks no
   in-flight replies** (R6-1 deleted the abort step: its only
   purpose was waking awaiters, which nothing can resume by this
   point). Pumps and the gate are destroyed last.
3. Promises die unfinished, **safely by construction**: the
   every-future-finishes guarantee (D1/D2) exists for awaiters that
   can still resume, and steps 1–2 plus the dead loop guarantee none
   can — every consumer coroutine is completed-and-destroyed or
   detached-and-undeliverable before the promise it awaited dies.
   `.then()` continuations on broken promises do not run (Qt cancels
   the chain).

**Why nothing resumes (S1-2):** every awaiter we use delivers
resumption only through the event loop — the reply awaiter connects
`finished` via `Qt::QueuedConnection` to a context object owned by
the frame, the `QFuture` awaiter resumes through `QFutureWatcher`
event delivery, the timer awaiter rides timer events, and the gate
and stop-interruptible sleep are built on the `QFuture` path (D5).
After `a.exec()` returns no loop iteration ever runs, so a detached
frame is inert no matter what signals fire during destruction —
spike-verified for the full analog (handles destroyed while
suspended on timer/future/reply, then QNAM destroyed with an
in-flight reply: nothing resumed, nothing crashed). The cost is
bounded and accepted: each detached frame plus its locals leaks a
few hundred bytes at process exit. Consequently **this shutdown is
only valid after the event loop has stopped** — destroying owners
while the loop still runs would let detached frames resume into
freed memory. No live-session path destroys owners (the invariant
below), and `Application` teardown runs after `a.exec()` returns.

**Invariant: no coroutine resumes through a destroyed `this`.**
During a live session the invariant holds because owners (hub,
worker) outlive their tasks and no live-session path destroys them;
at shutdown it holds because nothing delivers a resumption once the
loop has stopped (S1-2). QCoro ≥ 0.13 stays a hard floor: the
`QFuture` awaiter's `QFutureWatcher` lives inside the frame
(spike-verified at 0.13), so a completed-and-destroyed frame can
never be resumed by a later finish, and the FetchContent
include-path fix landed in 0.13; earlier releases are unverified.

**Exception policy (IR4; rewritten R5-1).** Boundary payloads are
`expected`, so an exception crossing a layer boundary is a bug — but
coroutine bodies can still throw (Qt, stdlib). Rev 4's rationale —
"handles are owned members, so no exception can vanish" — was
unsound: QCoro stores an unhandled exception in the task and
rethrows it only to an awaiting consumer or `.then()` continuation;
a retained-but-never-awaited handle observes nothing. A drain that
escaped would never run the code that sets the terminal state, and a
per-fetch task that escaped would never count its completion — a
wedge (F57's ghost), with the exception sitting silently in the
handle. Logging alone is not containment either, and destroying a
`QPromise` unfinished mid-session is the resultless state D2 forbids.
Therefore:

- **No exception ever escapes a root frame.** Every root task — the
  pump drain and each per-fetch task — wraps its entire body in a
  catch-all and finishes non-exceptionally; the worker's root
  orchestration is a synchronous method rather than a task (rev. 10),
  but it likewise wraps its body in a catch-all so nothing escapes into
  the caller. QCoro's stored-exception machinery is deliberately never
  exercised; handle ownership is for lifetime only and supervises
  nothing (D6).
- The drain's catch-all is what *implements* the terminal failed
  state: it completes the active entry `Internal` (the guard below
  already guarantees that), transitions the pump, and fails the
  remaining deque entries fast. A pump in the terminal failed state
  stays there — the event is logged loudly and captured, later
  submissions complete `FetchError{Internal}` fast, and no restart
  is attempted (restart-on-throw risks a tight crash loop;
  terminal-and-loud is the containment answer — the app keeps
  running, the next session starts clean).
- A per-fetch task's catch-all takes the same first-failure path as a
  `FetchError{Internal}` result: a direct, counter-independent
  `AbortUpdate()` that requests the stop and returns the worker to idle
  on the first failure — the update aborts cleanly instead of waiting
  forever on a counter that will never move. It deliberately does **not**
  count the throwing fetch as a completion (rev. 11): the terminal
  transition is independent of the completion counters, because feeding a
  failed fetch back through the success-equality counters drove `needed`
  below `received` and made reported progress non-monotonic (P-STATUS).
  The anti-wedge guarantee is stronger for the direct abort — it does not
  depend on every straggler counting, only on the first failure reaching
  `AbortUpdate()`, which is idempotent and guards on `WorkerState`.
- Every dequeued or parked entry keeps its **scoped completion
  guard**: if the promise has not been completed when the guard
  unwinds — any exceptional path included — it completes
  `FetchError{Internal}`.
- The facade's parse continuations are exception-tight (D7).

## What gets deleted

`RateLimitedReply` and the worker-facing reply boundary
(`RateLimitedRequest` is **not** deleted — it is the limiter's internal
queue entry, transformed in place to carry a `QPromise`; see D1), the
worker's `m_queue` /
`m_queue_id` / `SubmitNextItemRequest` / `FetchItems`'s queue walk, the
five error-path queue clears with counter-snapping, `DiscardIfStale`
and the generation-tag plumbing outright (D2/D6), the `SetupEndpoint`
nested event loop, the hub's `FatalError` paths (D4: setup failures
fail the affected requests cleanly instead of killing the app), F58's
dead `last_send` block, the dead `RateLimitPolicy::EstimateDuration`
(zero callers; contains the one reachable `/ max_hits` divide — R7),
and `tests/fakenetwork.h`'s `FakeRateLimiter`
(replaced by a facade fake). The pinball machine
(`ActivateRequest`/`SendRequest`/`ReceiveReply` state smear) is
replaced by the drain loop.

## Dependency: QCoro

- QCoro **v0.13.0** (Feb 2026), pinned exactly, fetched via CMake
  FetchContent (whose include-path handling was fixed in this
  release). Requires Qt ≥ 6.8 — satisfied by our 6.11 floor; MIT
  license. Used for: awaiting `QFuture`/`QNetworkReply`/timers,
  `QCoro::Task<>` in the worker and pumps. **0.13 is a hard floor, not
  a preference**: the semantics the shutdown contract leans on —
  the `QFuture` awaiter's `QFutureWatcher` living inside the frame,
  queued reply-awaiter delivery, refcounted detach behavior — are
  spike-verified at 0.13 exactly (S1); earlier releases are
  unverified, and the FetchContent include-path fix landed in 0.13.
  (The spec's first draft said 0.11.x was current — stale; caught by
  ER9.)
- **FetchContent hygiene (IR round; spike-verified S1-10):** QCoro's
  examples default ON and its tests follow `BUILD_TESTING`, which
  acquisition enables globally — set `QCORO_BUILD_EXAMPLES=OFF` and
  `QCORO_BUILD_TESTING=OFF`, and **do not touch the global
  `BUILD_TESTING`** (that would disable acquisition's own test
  suite; QCoro maps `QCORO_BUILD_TESTING` onto a directory-scoped
  `BUILD_TESTING` of its own, so the parent's flag stays intact).
  The flags are pinned in the root `CMakeLists.txt` (phasing step 2),
  which is the working reference; the spike that validated them is
  retired (July 2026) and lives in git history.
- Qt has **no native** `QFuture` coroutine support as of 6.11; an
  upstreaming effort exists targeting 6.12 at the earliest. If it
  lands, the future-awaiting uses of QCoro can migrate; the
  reply/timer awaitables and `Task<>` keep QCoro relevant regardless.
  Because D2 never cancels futures, even QCoro's naive canceled-future
  handling (`await_resume` calls `result()` unconditionally — verified
  at v0.13.0) is safe by construction; no design decision depends on
  which awaiter implementation is underneath.
- Compiler floor: `CXX_STANDARD 23` in CMake does **not** enforce a
  compiler version (ER9). CMake must check and
  reject explicitly — GCC ≥ 13 (earlier GCC miscompiles coroutines
  with captures in temporary lambdas) plus the Clang/MSVC minimums the
  pinned QCoro release documents — and `BUILD.md` must state the
  floors.

## Testing plan (harness-first, non-negotiable)

Ordering is part of the spec, because the pump rewrite touches the most
battle-tested code in the app. (Revised in the IR round: pins added
for IR1–IR6; primitives are now the gate and the stop-interruptible
sleep.)

1. **Manager harness before any rewrite.** A `RateLimitManager`-level
   test rig with a fake sender serving synthetic `X-Rate-Limit-*`
   headers (the F57 entry's demand — this layer has zero coverage today,
   which is exactly how F57 shipped). Pin current behavior: pacing
   timing against synthetic policies, saturation waits, non-retryable
   error surfacing, and the F57 wedge itself (as a failing-behavior
   pin, then flipped by the pump).
2. **The pump must pass the same harness**, minus the pinned bugs, plus
   new pins: 429 → retry with N19 padding → caller sees exactly one
   final completion; retries exhausted → `FetchError{RateLimited}`
   **completed immediately after the final 429, with no retry sleep**;
   stop while queued → completes `Canceled`, never sent; stop during
   the pacing sleep, a gate wait, or a retry sleep → wakes promptly,
   completes `Canceled`, nothing sent; stop while in flight → the
   reply lands, history and capture record it — **and a stopped
   in-flight 429 still records its violation** — then `Canceled` (and
   `QNetworkReply::abort()` is never called at all — N25, R6-1);
   **the ER1 regression pin**: two awaited fetches, one fails
   and stops the update while the sibling is still suspended — the
   sibling must resume safely into a finished `Canceled` future;
   a steady-state reply with unparseable headers or a mismatched
   policy name records history and capture, leaves the policy
   byte-for-byte un-updated, and completes `Protocol` — and the pump
   sends nothing unpaced afterward (IR1); `Retry-After` vectors
   (R7): 0 is accepted and retries with the pad dominating; missing
   / negative / non-numeric / above-the-cap are terminal
   `RateLimited`; **drain
   lifecycle**: submit while a drain is running joins it, submit
   after it finished starts a new one, the drain exits when the deque
   empties; **exception pins (IR4)**: an exception escaping the drain
   completes the active entry `Internal`, puts the pump in its
   terminal failed state, and later submissions fail fast `Internal`;
   a throwing facade parse continuation yields a `Parse`/`Internal`
   *value*, never an exceptional future; **permit-free retry
   (R4-1)**: a retrying entry holds no permit during its retry sleep
   — a sibling pump acquires the gate and sends while that sleep
   runs; **reply ownership (R5-4/R6-1; corrected S1)**: every reply
   the fake sender hands out is destroyed exactly once on every
   live-session path — success, non-retryable error, each retry
   attempt, cancel-after-landing, drain exception — via the
   dispatch-time wrapper when the frame completes; destroying a
   task handle mid-flight detaches the frame (S1-1), which still
   completes when the reply lands and releases it through the same
   wrapper — leak and double-deletion pins; **observation (R6-3)**: a 429 and a 500 carrying Full
   matching headers update the policy while completing their
   status-driven outcomes, and a 2xx-plus-transport-error updates
   from its headers and completes `Network`.
3. **Setup and validation pins**: a total-parse suite over malformed
   headers — missing rules list, missing per-rule headers, short
   triplets, non-numeric fields, mismatched periods, and the R7
   vectors: empty policy/rule names, zero-rule and zero-item
   replies, zero-hit limits (the current parser's UB inputs become
   ordinary test vectors); each
   setup-failure flavor (transport error, non-2xx, unparseable 2xx
   headers) fails every parked entry with the right `FetchError` kind
   and sets the cooldown; a caller resubmitting from its completion
   handler cannot provoke a probe inside `SETUP_RETRY_COOLDOWN`; a
   Full HEAD 429 with a valid Retry-After establishes the pump with
   the first send held past `Retry-After + pad + buffer`, consuming
   no attempt; a Full HEAD 429 **without** a valid Retry-After, and a
   non-Full HEAD 429, are setup failures whose cooldown honors
   Retry-After when validly present; an established endpoint keeps
   its topology through an ordinary timeout (no HEAD re-probe);
   setup cancellation — canceled parked entries complete `Canceled`
   without disturbing the endpoint state, an in-flight probe's reply
   still establishes the endpoint after all entries cancel, and
   cancellation alone never installs a cooldown; concurrent endpoint
   setups serialize their HEADs at the gate; an idle pump persists to
   shutdown without leaking or waking.
4. **Primitives tested standalone** with an **injected monotonic
   clock/scheduler** (group 4) so timing behavior is deterministic
   and the tests never sleep: the stop-interruptible sleep (one
   token per R5-2; wakes on stop, completes on deadline, and resumes
   via the event loop — `request_stop()` returns before the waiter
   resumes, R4-2)
   and the gate — cap, HEAD exclusivity **with writer preference** (a
   waiting HEAD blocks new ordinary permits; a busy pump cannot
   starve setup), **ordinary-waiter FIFO** (permits grant in arrival
   order; two contending pumps alternate and neither starves — R7),
   spacing floor, and **permit span** (a permit is
   held until — and released at — reply-finish; the cap genuinely
   bounds in-flight requests, and no permit survives into a retry
   sleep).
5. **Worker tests move to a facade fake** ("when asked for stash X,
   return this tab / this error") — simpler than today's byte-crafting
   `FakeRateLimiter`, and it makes the worker suite's unit boundary
   explicit. (Corrected in 4b: the earlier wording claimed this closed
   a fake-bypasses-the-managers blind spot, which was wrong — a
   `FakeRateLimiter` overrides `SubmitFuture`, so no `RateLimiter`,
   hub, gate, or pump code runs under it either. Both fakes bypass the
   managers equally; manager behavior is covered by the dedicated
   manager and hub harnesses and by item 6's integration coverage,
   never by `tst_workerupdate`.) The coverage split is: worker suite —
   real worker over a fake typed facade; `tst_poeapiclient` — real
   facade over a fake limiter, which is where request building and
   endpoint labels are pinned; limiter and manager suites — real
   networking layers over their own lower-level fakes.
   **The F56 regression shape is an acceptance requirement:** for an update
   requiring both lists and pending fake futures, both list calls exist before
   the harness settles either; each arriving list launches its complete content
   batch before any content reply is delivered; character calls are in flight
   while stash calls remain outstanding; Folder children and reply-discovered
   Map/Unique children join complete batches; and call order within each policy
   lane follows D6's source traversal order. Partial updates still request only
   their required list type. Every list/content/child call in one update carries
   the same non-default token, a terminal failure stops it, and the next update
   carries a distinct unstopped token.
   Existing worker-update behavior pins (`tst_workerupdate.cpp`)
   are ported, not weakened. In particular, reordered completion must preserve
   M1's atomic replacement/rebase boundary, F53's reconciliation signals,
   F55's `m_contents_known` parent/child rules, exactly one terminal status
   transition, and D6's active-`Update()` refusal policy. **IR2 invariant
   pins**: an
   already-finished success and an already-finished error future
   (fail-fast submit) run their continuations synchronously during
   the batch-submit loop without corrupting counters
   (initialize-before-launch); a fetch that completed successfully
   just before `request_stop()` resumes its consumer afterward and
   mutates nothing (post-await identity); **R5-1 pin**: a per-fetch
   task whose body throws triggers the first-failure abort — a direct,
   counter-independent `AbortUpdate()` (rev. 11) — so the update finishes
   (aborted), never wedges, without counting the throwing fetch;
   the handle sweep runs outside any completing coroutine (the last
   completion does not destroy its own frame); **R6-2 pin**: a
   context-bound `Shop` continuation does not run after `Shop` is
   destroyed.
6. **Integration coverage** (group 4 + IR3; shutdown pins rewritten
   R5-2, corrected S1): cancellation races across layers (abort
   mid-pacing-sleep, mid-gate-wait, mid-flight); **destruction
   reaches every suspension** — destroying worker-then-hub in the
   `UserSession` member order, with no further event-loop
   iterations, while a pump is mid-pacing-sleep, mid-retry-sleep,
   mid-gate-wait, and mid-flight resumes nothing and crashes
   nothing; suspended frames detach and leak by design (S1-1/S1-2)
   — the leak check pins that the leak is bounded to the **transitive
   closure of the detached frames**: the frame allocations plus what
   only they retain (the awaiter objects, their `QFutureWatcher` and
   context QObjects, retained `QFuture` handles — which necessarily
   keep their promise shared state alive, since QCoro's awaiter
   stores a `QFuture` inside the frame — and sleep timers). Outside
   that closure nothing may leak: not a reply (the QNAM parent must
   free it), not an owner object (hub, pump, worker, facade
   members), not anything reachable other than through a detached
   frame; promises die unfinished — no awaiter can be resumed to
   observe them, and destroying an unfinished `QPromise` cancels
   its future and its `.then` chain without running a parse
   continuation or resuming the detached awaiter of the chained
   child future (the production topology: the worker awaits
   `.then(parse)`'s child, not the parent — S1 follow-up 2);
   destruction mid-update; completed-future retention (memory does
   not grow with completed fetches across a long update).
7. Every commit compiles and passes `ctest` (working rule #2). The
   capture instrument's tests (`tst_networkcapture.cpp`) must keep
   passing — captures are live research data (Q5, Q9).

## Phasing sketch (an implementation plan derives from this)

0. **QCoro integration spike (IR round; gates everything
   QCoro-dependent). Executed July 19, 2026** — code in
   `spikes/qcoro/` (a standalone CMake project, not part of the
   acquisition build), findings recorded as round S1 in the review
   history and folded into D2, D6, the shutdown section, and the
   dependency section in the same commit. The question list it
   exercised: task-handle ownership and destruction while suspended
   on each awaitable we use (future, reply, timer); awaiting
   already-finished futures; `result()` vs `takeResult()`;
   promise-completion re-entrancy; stopped waits;
   queued-vs-synchronous resumption on stop; destroy-while-suspended
   as the shutdown mechanism; continuation self-destruction vs the
   deferred sweep; unawaited-task exceptions; the FetchContent
   hygiene flags. Headline result: R5-2's mechanism was falsified
   (handle destruction detaches, S1-1) and the shutdown contract now
   rests on detach-plus-no-delivery (S1-2); everything else held.
   **The step's one open measurement — batch submission at the
   2,000-tab scale the items-pipeline doc names — executed July 19,
   2026** (round S2, `spikes/qcoro/batch.cpp` in the same spike
   project): the full promise/future/frame/token population costs
   ~6.5 KB per entry (~12.4 MB at 2,000 tabs, linear to 10,000), a
   full abort drains in ~2–4 ms with the per-pump ~22 ms sleep wake
   dominating, and memory unwinds to baseline with zero leaks after
   the sweep (S2-1..S2-4). The numbers did not demand flow control,
   so none is designed; step 0 is closed.
1. Manager test harness against current code (no behavior change).
   **Executed July 19, 2026** — `tests/tst_ratelimitmanager.cpp` plus
   the fake sender (`tests/fakesender.h`), pinning pacing timing,
   saturation waits, non-retryable error surfacing, and the F57 wedge
   (as current, failing behavior — the pin shows the retried
   completion dropped on the floor), along with the F58 dead-spacing
   and F59 synchronous-destruction behaviors the pump will change
   deliberately.
2. QCoro dependency + primitives (stop-interruptible sleep, gate)
   with their tests. **Executed July 19, 2026** — QCoro v0.13.0
   pinned in the root build with the spike's FetchContent hygiene
   flags, plus ER9's compiler floors as explicit configure-time
   checks (GCC ≥ 13; Clang 15 / AppleClang 15 / MSVC 19.40 per the
   pinned QCoro release), stated in `BUILD.md`. The injected
   monotonic clock/scheduler the primitives — and the phase-3 pump —
   are built against is `RateLimit::Scheduler`
   (`src/ratelimit/scheduler.{h,cpp}`; `TimerScheduler` is the
   production implementation on precise timers). The primitives are
   `RateLimit::SleepUntil` (`src/ratelimit/stopsleep.{h,cpp}`) and
   `RateLimit::Gate` (`src/ratelimit/gate.{h,cpp}`), pinned
   standalone per testing-plan item 4 by `tests/tst_stopsleep.cpp`
   and `tests/tst_gate.cpp` against a fake scheduler
   (`tests/fakescheduler.h`) — deterministic, never sleeping; the
   production `TimerScheduler` adapter is real-timer code with its
   own condition-driven smoke tests (`tests/tst_timerscheduler.cpp`),
   kept out of the primitive suites so item 4's never-sleep property
   stays true of them as stated. Post-review correction (P1, July
   20): the gate's spacing floor is measured from each permit's
   dispatch stamp (recorded at waiter resume), not from grant time —
   a queued resume delayed by a busy main thread would otherwise let
   two sends land back-to-back inside the floor.
3. Pump rewrite inside `RateLimitManager`; hub gains the gate and async
   HEAD setup. Boundary still the old `Submit` shape via a thin adapter
   so the worker compiles unchanged. Resolves F57, F58, F5-modernization.
   **Executed July 20, 2026**, in four green commits: the gate's
   dispatch stamp moved to the real send site
   (`Permit::MarkDispatched()`; a no-send release charges no spacing);
   the D8 total parse (`RateLimitPolicy::Parse`, `tst_ratelimitpolicy`
   — pulled forward from phase 4 because the pump's Full-check and
   D4's unparseable-HEAD detection both need it, and the spec forbids
   a separate validator); the drain-loop pump
   (`tst_ratelimitmanager` rewritten onto the injected scheduler —
   exact fake-clock pins, no ±6% slack, no real sleeping); and the
   D4 hub (probe coroutine behind the exclusive HEAD permit, parking,
   every setup-failure flavor under the cooldown, `tst_ratelimiter`
   with a `createRequest`-level fake network). **Phase-3 boundary
   notes — what deliberately waits for phase 4's `FetchError`:**
   entries carry no stop token (nothing can cancel them; every pump
   wait passes an explicit never-stopped token), so the stop pins of
   testing-plan item 2 land in phase 4; a steady-state name mismatch
   or unparseable 2xx leaves the policy un-updated (the D8 policy
   half, pinned) but completes the clean 200 to the caller — the
   old boundary can only express failure as an errored reply, and
   fabricating an error onto a clean 200 would misreport the server,
   so the `Protocol` outcome half lands with the phase-4 boundary
   (IR1's frozen-pacing residual is accepted for the one-phase
   window: never-observed event, gate-bounded rate, loud logs);
   setup failures and the fail-fast cooldown are expressed as errored
   synthetic replies (`SetupFailureReply`, phase-4 scaffolding);
   the drain's terminal failed state drops the active entry's
   `RateLimitedReply` without a completion (the only expressible
   shape — becomes a clean `Internal` value in phase 4); and the
   F59 ownership pin is kept byte-for-byte.
4. `QFuture` boundary + `PoeApiClient`; `Shop` and worker call sites
   move over; `RateLimitedReply` and the worker-facing reply boundary
   deleted (`RateLimitedRequest` transformed in place into the pump's
   promise-carrying entry — see D1). Resolves F59.

   **Sequencing refinement (July 20, 2026): phase 4 splits into 4a and
   4b.** The split follows from the test harness, not from the design:
   `tests/fakenetwork.h`'s `FakeRateLimiter` overrides the legacy
   `Submit()`, so deleting `RateLimitedReply` forces the worker-test
   fake to migrate in the same commit as the boundary change — two
   large, unrelated diffs landing together. Keeping the legacy `Submit`
   as a thin wrapper over `SubmitFuture` for one phase lets
   `tst_workerupdate` survive 4a untouched.

   **4a — executed July 20, 2026** (three green commits): `FetchError`
   and `FetchOutcome` (`src/ratelimit/fetcherror.h`); the queue entry
   gains a `stop_token` and a `QPromise`, and the free
   `CompleteRequest()` settles it exactly once, which is what makes the
   drain's scoped completion guard safe; the pump's stop checkpoints and
   the D8 classification order; `Update()` returns an observation, which
   lands the deferred `Protocol` outcome half (both flavors) and F50's
   log rewording; the hub's parked entries become owned entries with a
   scheduled stop-callback prune, `SetupFailure` carries a
   `FetchError::Kind`, and `SetupFailureReply` is deleted;
   `PoeApiClient` with the transfer-timeout invariant (F60) and `Shop`
   moved onto it. **F59 is narrowed, not resolved:** the pump hands out
   no reply object, but the legacy `Submit()` adapter still returns one
   under the same contradictory ownership contract (the declaration says
   the caller frees it; the adapter deletes it after emitting) —
   deliberately preserved so the worker call sites are untouched, and
   resolved in 4b when the adapter is deleted. No production caller can
   cancel yet (every call site passes a default token per D7), so the
   stop behavior is harness-pinned only.

   **4b — executed July 20, 2026.** The worker's call sites moved to the
   facade: `ItemsManagerWorker` takes a `PoeApiClient` instead of a
   `RateLimiter`, the queue carries a fetch intent (`StashFetch` /
   `CharacterFetch`) rather than a prebuilt `QNetworkRequest`, and every
   handler takes one already-classified
   `std::expected<T, FetchError>` — the transport, status, and parse
   checks each handler used to do collapse into a single error branch.
   The callback pyramid stays (coroutines are phase 5); `DiscardIfStale`
   became the generation-only `IsStale`. `tst_workerupdate` moved to a
   typed facade fake (`FakePoeApiClient`, testing-plan item 5) that
   records the domain arguments the worker asked with rather than
   crafting bytes; its pins were ported, not weakened, except the F28
   stale-arrival half, which the future boundary makes unreachable (each
   fetch is completed once by the pump; the worker never has a request in
   flight when an update aborts) — `IsStale` is kept for phase 5 and the
   pin records why. `tst_ratelimiter` lost its legacy-wrapper pins; the future
   boundary's fail-fast completes synchronously, which the ported pins
   assert. Then `RateLimitedReply` (`.h`/`.cpp`), the `Submit()` adapter,
   and its synthetic reply were deleted together — F59 resolved. Item 5's
   claim that a facade fake closes a manager blind spot was corrected in
   the same pass (it does not; neither fake runs manager code).
5. Worker rewrite: batch submission, coroutine orchestration, abort via
   cancellation; worker queue and generation machinery deleted. Resolves
   F56. The completion-evidence IDs live in
   `network-redesign-phase5-verification.md` (the transient execution plan that
   carried the packages was removed once phase 5 landed); it does not override
   this spec.

Each phase is independently shippable; 3, 4, 5 each land with their
tests in the same PR.

## Interaction with the items-pipeline plan

- This spec formally amends the items-pipeline **"Rate limiter
  redesign" non-goal** (anticipated by F56's pause note): the redesign
  is now in scope, specified here. The F5 property is preserved (D4/D5).
- For the M2 spec: scheduling lives in the limiter; the worker owns
  selection and ordering. M2's per-tab delta signal, durable progress,
  and UI coalescing are orthogonal to this spec and compose with it —
  per-tab completion is exactly a facade future resolving.
- N26's even-pacing idea stays a research experiment, not a design
  input: padded honestly for buckets, even pacing is
  throughput-identical to burst-then-stall
  ((300+60)/30 = 12.0 s/request vs 12.03 observed), and burst wins the
  everyday small-update case outright. The doc's ~20% claim compared
  padded burst against unpadded pacing; unpadded pacing is exactly the
  N15 violation trap. (Recorded here so the ground-truth note doesn't
  get re-litigated.)
- Q4 (initial-vs-sustained classification, the stash-list under-padding
  suspect) lives entirely inside `RateLimitPolicy` and is deliberately
  **not** part of this redesign — it composes as a small independent
  change whenever Tom confirms the positional hypothesis with GGG.

---

## Review history

The full review history — the finding tables (ER, IR, R4-\*, R5-\*)
whose IDs this spec cites inline, the round narratives, and the
revision log — lives in `docs/archive/network-redesign-reviews.md` (split out of
this file's Appendix B at revision 5; the appendix's earlier
evolution is in this file's git history). New review rounds and
revision-log entries append there; this spec records only current
decisions, and its status line is updated in the same commit as any
new round.
