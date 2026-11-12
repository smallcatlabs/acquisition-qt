# Items Pipeline Milestone 2: Streaming Refresh Signal

Status: **FROZEN at revision 9** (July 29, 2026). Review rounds 1–7
are incorporated (rounds 1–3 and 5–7 external, round 4 an in-repo
audit; round 7 closed the review series at revision 8). Revision 9
records the pre-freeze evidence and makes no new arguments: F62 is
merged to master (PR #183, `fdf530b5`), and the S1-M2 spike ran
July 29, 2026 on the non-production branch `spike/s1-m2-throttle`
with Tom judging cadence and restore fidelity by hand — **D9's
outcome (a) is selected and S is confirmed at 60 seconds**, with the
R6-3 restore-fidelity contract validated for the exercised intra-tab
cases (global cross-tab reselection remains a production obligation
— see the amendment) and reset latency recorded
(evidence archived in `s1-m2-spike-result.md`, retired to git
history August 1, 2026 — the numbers survive in D9 and the
open-items entry). **Amended July 29,
2026** after a post-freeze external review of the spike evidence
(six findings, none changing a decision): the supported envelope and
the exact freshness bound are now stated in D9's result, the spike's
evidence claims are corrected in the archived result document, and
R6-3 gains the cross-tab reselection pin — see the reviews file's
amendment entry. **IMPLEMENTED July 29–30, 2026** (stages 0–8 on a
single branch; **PR #185 merged July 30**): M2-M2 ran at its stage-4
checkpoint as required (R7-3), missed every budget, and was remedied
by the source-keyed stores with a passing rerun (`m2-m2-result.md`).
**Superseded in part by M3** (implemented July 31, 2026): D9's
consumer-side machinery — the throttled reset, its pins, and the
restore path — is retired by the delta-native items model, with
exactly three stated renegotiations recorded there
(`items-pipeline-m3.md`, D3); D1–D8 and D10 remain the live
authority for the worker, manager, and shop sides. This spec
consumed the M2 inbox in `items-pipeline.md` (trimmed to a
consumed-pointer August 1, 2026) and its four hard constraints; the
traceability table at the end maps every input to the decision,
deferral, or acceptance criterion that consumed it.

Citation convention: bare D-numbers (D1, D2, …) in this document are
this document's decisions. Decisions of the network redesign are always
cited qualified ("network-redesign D6"). F-numbers are the findings
register; `R1-*` (and later `R2-*`, …) are this spec's review-round
findings, recorded with verdicts and resolutions in
`items-pipeline-m2-reviews.md`; pinned test names are quoted in
`camelCase`.

## Staleness preamble

Written against commit `d995840b` (master, July 27, 2026). The load-
bearing code assumptions; re-verify these anchors before implementing
against any section, and re-verify the whole list if the worker or the
refresh path has been touched since:

- **Worker signal surface** (`itemsmanagerworker.h:111-140`): snapshot
  signal `ItemsRefreshed(Items, tabs, initial_refresh)`; cosmetic
  `StatusUpdate`/`NotifyUser`; persistence signals `stashReceived`,
  `characterReceived`, `stashListReceived`, `characterListReceived`,
  and the authoritative-list signals `stashListReplaced`,
  `characterListReplaced`, `stashChildrenReplaced` (F53). No
  presentation delta signal exists yet.
- **Per-reply emit ordering** (`OnStashReceived`,
  `itemsmanagerworker.cpp:896-1026`; `OnCharacterReceived` symmetric):
  failure branch (`StopUpdateForFailure` + `AbortUpdate`) → persistence
  emit `stashReceived` → atomic replace (`RemoveItemsFetchedBy(
  location.fetch_id())` then `ParseItems`) → counter increment +
  `SendStatusUpdate` → Map/Unique/Folder child-batch discovery → the
  parent-reply ghost-child reconcile (`stashChildrenReplaced` emit and
  the `std::erase_if` over unexpected fetch ids) → `LaunchContent` →
  `CheckUpdateFinished`.
- **Success terminal** (`FinishUpdate`,
  `itemsmanagerworker.cpp:1227-1281`): status emit →
  `RebaseItemLocations` (deferred to success by design; see the M1
  section of `items-pipeline.md`) → sort tabs → sort items → emit
  `ItemsRefreshed(m_items, m_tabs, false)` → Idle.
- **Failure terminal** (`AbortUpdate`, `itemsmanagerworker.cpp:525`):
  idempotent, returns the worker to Idle immediately, no
  `ItemsRefreshed`; stopped stragglers settle later and apply nothing
  (post-await invariant, network-redesign D6, W-IDENTITY).
- **Initial cached load** (`itemsmanagerworker.cpp:274`): the parse
  thread's result is published as one snapshot emit
  (`ItemsRefreshed(..., true)`), not as deltas.
- **ItemsManager** (`itemsmanager.cpp:121-147`): `OnItemsRefreshed`
  copies the vector, runs the debug uncategorized scan (F46), then
  `SetStashTabLocations` → `MigrateBuyouts` → `ApplyAutoTabBuyouts` →
  `ApplyAutoItemBuyouts` → `PropagateTabBuyouts` → re-emits
  `ItemsRefreshed(bool)`. `PropagateTabBuyouts` opens with the global
  `ClearRefreshLocks()` (`itemsmanager.cpp:93`).
- **Application** (`application.cpp:404-414`): currency snapshot, shop
  expiry, and (non-initial, auto-update on) forum shop submission on
  every `ItemsRefreshed`.
- **MainWindow** (`ui/mainwindow.cpp:1090-1104` and `736-775`):
  `OnItemsRefreshed` refilters every non-current search
  (`Search::FilterItems` — a whole-collection scan per search), then
  `ModelViewRefresh()` for the current search;
  `ItemsModel::beginUpdate()` is literally `beginResetModel()`
  (`items_model.h:28`), and the reset is followed by the restore
  machinery (`RestoreViewExpansion`, `ReselectCurrentItem`,
  `ScheduleResizeTreeColumns`).
- **Search dirty mechanism** (`search.h:82-85`): a filter state changed
  while a search is in the background sets `m_states_dirty` and forces
  a refilter when the search is next shown. D9 extends this pattern.
- **ItemLocation** (`itemlocation.h:53-62`): `fetch_id()` is the id of
  the stash/character actually fetched; for Map/Unique children it is
  the child's id while `id()` stays the parent (display) tab.
  Deliberately excluded from `operator==`, `operator<`, and
  `GetLegacyHash` — anything keyed on location equality collapses
  sibling child replacements.
- **FetchError taxonomy** (`ratelimit/fetcherror.h`): `Network`,
  `Http`, `Parse`, `Protocol`, `RateLimited`, `Internal`, `Canceled` —
  already the deterministic/transient vocabulary D5 needs.
- **F62 is decided but NOT implemented** (findings register): the
  persistence lane will carry raw wire bytes
  (`stashReceived`/`characterReceived` gain an opaque `QByteArray`).
  The code anchors above are pre-F62; D1 sequences it.

## Scope

M2 makes refresh progress visible incrementally without triggering the
snapshot cascade: a per-fetch-source delta signal from the worker, a
streamed published copy in `ItemsManager` with scoped pricing, a typed
terminal outcome event, and conservative, freshness-bounded delta
application in `MainWindow`. The final `ItemsRefreshed` snapshot
contract is unchanged (D8). Out of scope, each with reasons: per-tab
retry mechanism and durable resume, whole-update replacement/cancel,
reprioritization (D11); delta-native model operations (M3).

## Decisions

### D1. Sequencing: F62 lands before M2 implementation begins

The F62 fix (raw bytes through the persistence lane, decided July 26)
is implemented and merged **before** M2's code starts. This spec is
therefore written against the post-F62 worker: `stashReceived`/
`characterReceived` carry an opaque `QByteArray` alongside the typed
payload, and network-redesign D7 reads "nothing above the boundary
*interprets* bytes".

Reasons:

- F62 changes the signatures of exactly the signals M2's new delta
  signal sits next to, and the facade return types the worker's
  handlers consume. Landing it first means M2's wiring is written once,
  against the final shape of the persistence lane, instead of being
  rebased mid-milestone.
- D3's two-lane split (persistence vs. presentation) is only crisp once
  the persistence lane has its final payload: API-domain object plus
  wire bytes. Confirming the split against a lane that is about to
  change shape would be confirming a fiction.
- F62 is independently targeted at "before final 0.18" and is not
  blocked by anything in this spec; ordering it first costs nothing.
- **F62 also precedes the M2-M2 measurement (R6-6),** not just M2
  code: M2-M2's unit includes persistence cost, and F62 changes the
  persisted payload (raw wire bytes). A pre-F62 benchmark would be
  obsolete by construction. (M2-M2 is now an early implementation
  checkpoint, R7-3; the ordering constraint is unchanged.)

M2's deltas inherit F62's boundary rule as a constraint: **a delta
never carries wire bytes or `poe::*` API objects** — the persistence
lane owns those. Deltas carry pipeline-native `Items` only (D3).

### D2. The atomic unit is the fetch source, not the display tab

The delta's unit of atomicity is one fetch source — one (location type,
fetch-source id): a stash tab, a character, or one Map/Unique child.
The alternative (publish a Map/Unique parent once all its enabled
children settle) is rejected.

Reasons:

- **It is the unit the worker already has.** The atomic replace is
  keyed by `fetch_id()` (`RemoveItemsFetchedBy`); emitting after it
  requires zero new worker state. Per-display-tab publication would
  reintroduce per-parent deferred-completion accounting — a join
  counter per parent, a withheld-buffer for settled children, and a
  failure story for "child failed, parent never publishes" — which is
  precisely the apparatus the F55 revision deleted.
- **Coalescing gives the settling for free.** Downstream application is
  coalesced anyway (D9); children of one parent that arrive within a
  coalescing window collapse into one visible update in the common
  case, without any worker-side accounting.
- **Failure behavior stays simple.** Under no-rollback (D4), per-fetch-
  source publication keeps the published copy a pure function of
  applied replacements. A withheld parent bucket would make worker
  memory run ahead of published state by an unbounded amount on an
  update that fails mid-parent.

Accepted cost, stated deliberately: **a Map/Unique display tab can
transiently mix old and new child data** while its children stream in.
This is qualitatively the same partial-refresh mixing the whole
collection already exhibits between per-tab updates (some tabs fresh,
some stale), confined to one display tab; each fetch source is
internally consistent at all times. Recorded as an acceptance criterion
(`parentBucketMayMixChildGenerationsMidRefresh` — a documented-behavior
pin, not a bug tripwire). The mixing accepted here is item *data*
generations; how mixed-*metadata* items are rendered mid-refresh is
governed by D6's stable-identity bucket rule (R5-1).

### D3. The delta signal: `TabRefreshed(location, items)`

A new worker signal, working name kept from the plan:

```
void TabRefreshed(const ItemLocation &location, const Items &items);
```

- **Key (R1-3).** Replacement application is keyed by a named type
  used identically on both sides of the signal:

  ```cpp
  struct FetchSourceKey {
      ItemLocationType type;
      QString fetch_id;
  };
  ```

  The signal still carries the full `ItemLocation` — it covers both
  ids (`fetch_id()` differs from `id()` only for Map/Unique children)
  and gives empty deltas a display anchor — but consumers derive the
  key as `{location.type(), location.fetch_id()}` and apply strictly
  by it, never by location equality, which deliberately ignores
  `fetch_id` (staleness preamble). The worker's own erase currently
  keys by fetch id alone (`RemoveItemsFetchedBy`,
  `itemsmanagerworker.cpp:451`); M2 adds the type to its predicate so
  the worker's and the published copy's erases can never diverge on a
  cross-type id collision — improbable, but the invariant in D6 is
  only sound if both sides use one key. Stated boundary (R6-5): the
  typed key governs exactly the predicates M2 introduces — the
  replacement erases and the child reconciliation. Legacy stores
  keyed by bare id (refresh selection `m_tabs_to_update`,
  `itemsmanagerworker.cpp:397`; refresh locks and tab buyouts;
  `ItemLocation::operator==`) are pre-existing surfaces M2 does not
  rekey; their cross-type collision exposure is recorded as **F66**
  in the findings register rather than expanded into M2.
- **Payload.** The complete pipeline-native `Items` replacement for
  that fetch source — the exact set of `Item` objects the worker just
  parsed and appended, sharing the same `shared_ptr`s. An **empty
  `items` means an emptied fetch source, never a deletion** — tab
  deletion is a list-reconciliation effect and stays snapshot-boundary
  (D6). No wire bytes, no `poe::*` objects (D1).
- **Emit point.** In `OnStashReceived`/`OnCharacterReceived`,
  immediately after the atomic replace (`RemoveItemsFetchedBy` +
  `ParseItems`) and before the counter increment and
  `CheckUpdateFinished` — so every delta of an update precedes its
  terminal event (D4) by construction.
- **Ghost-child drops stream as one aggregate child reconciliation
  (R5-2, R6-2).** The parent-reply reconcile erases items fetched
  from children the parent no longer lists in **one** collection
  scan (`itemsmanagerworker.cpp:1007-1019`). Revisions 2–5 expressed
  this as one empty `TabRefreshed` per dropped fetch id — an
  O(k × all items) synchronous burst downstream (R5-2). Revision 6
  sent the keys the worker actually erased — which round 6 showed
  can miss published ghosts: worker and published state diverge
  across a failed update (a list deletion mutates worker memory and
  datastore at list arrival, `itemsmanagerworker.cpp:781-791`, but
  stays snapshot-boundary downstream), so a child the worker no
  longer holds can survive in the published copy with nothing to
  name it. The signal therefore carries the **authoritative expected
  set**, not the erased set:

  ```cpp
  void ChildrenReconciled(const ItemLocation &parent,
                          const std::vector<FetchSourceKey> &expected);
  ```

  `expected` is the parent's own fetch source plus its currently
  listed children — exactly what the worker's own reconcile
  predicate uses. Every consumer applies that predicate to its
  **own** baseline: items under the parent's stable id whose key is
  not in `expected` are erased, in one pass with set lookup — exact
  worker parity, the same bound as the primary erase, and correct
  against any published baseline, however it diverged. Zero-item and
  worker-absent ghosts need no enumeration because nothing is
  enumerated. `ItemsManager` re-emits the same shape for the UI,
  where it takes the removal half of D9's intersection test for all
  keys at once. An empty `TabRefreshed` still means an emptied
  *fetched* source and nothing else; the published copy stays a pure
  function of applied mutations (D6).
- **The cached initial load does not stream.** `OnParseCompleted`
  publishes one `ItemsRefreshed(..., true)` snapshot as today: it is a
  single in-memory atomic load with nothing incremental about it.
  *(Post-freeze amendment, November 12, 2026 — #294: the load is now
  `LoadCachedItems` in `src/cacheload.cpp`, and
  `ItemsManagerWorker::Seed` publishes the snapshot `OnParseCompleted`
  did.)*
- **Two-lane split, confirmed.** Persistence signals
  (`stashReceived`/`characterReceived` + list signals) carry API-domain
  payloads plus, post-F62, wire bytes, and fire **before** the atomic
  replace; the presentation delta carries pipeline-native `Items` and
  fires **after** it. The lanes never share payload types — that is the
  drift guard: any future field that tempts one lane to import the
  other's types is a spec change, not a convenience.

`ItemsManager` applies each delta to its published copy — erase items
whose `FetchSourceKey` matches, append the delta's items — runs the
scoped pricing pass (D7), and re-emits a light signal with the same
shape for the UI:

```
void TabRefreshed(const ItemLocation &location, const Items &items);
```

**Published storage stays the flat `Items` vector, and the erase is a
permitted linear pass — gated by measurement (R1-2, R2-3, R3-3).**
Revision 1 claimed O(delta) work on this path while implying an
O(all items) erase — an inconsistency the review caught. Resolved by
legitimizing the erase rather than indexing: one predicate-only
`erase_if` over the flat vector per delta is explicitly allowed. The
precedent is **worker parity** — it is the identical operation the
worker itself performs per reply (`RemoveItemsFetchedBy`), shipped in
M1 — but precedent is not a bound (R2-3): the pass dereferences a
heap object and compares type plus `QString` per entry, M2 doubles
the per-reply scans, and the codebase itself acknowledges users at
the "hundreds of thousands or millions of items" scale
(`search.cpp:243`). The choice therefore carries a **blocking
measurement** (M2-M2, open-items list; R3-3, R4-1, R5-4, R7-3),
which separates two questions revision 5 conflated. The **storage
question** — does `ItemsManager` need indexed storage? — is answered
by the manager's marginal erase cost: **< 2 ms at 100k and < 16 ms
at 1m** on representative datasets. A miss makes the manager remedy
**required, not discretionary**: a source-keyed map
(`FetchSourceKey → Items`) with a lazily rebuilt flat vector for
`items()` consumers — also the natural M3 representation; M2 does
not build it speculatively. The **frame question** — does one reply
stall the UI? — cannot be answered by erase scans alone: the
synchronous reply path also includes persistence writes, parsing and
append, scoped pricing (D7), and the intersection test and signal
fan-out (D9). M2-M2's measured unit is therefore the **complete
synchronous reply application**, worker handler through UI fan-out,
budget **< 2 ms at 100k and < 16 ms (one frame) at 1m**, reported
with per-component attribution. A frame miss requires a **real
remedy** for the dominant component — the manager map above, a
symmetric worker-side source-keyed store (measurement-gated, the
same M3 shape), or a named fix for whichever component dominates —
never a deferred paper-trail finding (revision 5's half-threshold
arithmetic and remedy-by-finding are dropped, R5-4). The measurement
runs as the **first checkpoint of production implementation** — as
soon as the post-F62 vertical reply path (worker handler →
persistence → manager → pricing → UI fan-out) exists, and before M2
is considered complete (R7-3, reversing revision 6's pre-freeze
placement). R5-4's rule — a frozen spec must not leave the central
storage choice open — is satisfied without a pre-freeze run: what
freezes is a determinate conditional (flat vector; a budget miss
makes the named remedy required, not discretionary), the choice is
internal to the manager and alters no delta, terminal, or
published-state contract, and measuring the real path replaces the
throwaway reconstruction a pre-freeze run had grown to require —
whose miss branch would itself have implemented a remedy before
freeze, violating the doc-first rule the exception was meant to
protect. Record a Release build and
the measurement environment (hardware, OS, compiler, Qt, allocator),
**fixed** representative datasets and reply-size/removal shapes —
recorded so reruns are comparable — repetitions, and the reported
statistic with the result. The measurement runs against a
**post-F62 build** (D1, R6-6), and if a remedy is selected, the
measurement is **rerun with the remedy in place** to validate it
meets budget before M2 is considered complete — naming a remedy is
not evidence (R6-6). No *other* whole-collection work runs on this
path (hard constraint; acceptance criterion below).

### D4. A typed terminal event, and no rollback

A new worker signal, forwarded by `ItemsManager`. The outcome is a
sum type so invalid states (an error on a completion, skips on a
failure) are unrepresentable (R1-6):

```cpp
struct SkippedSource {
    FetchSourceKey source;          // same key as the deltas (D3)
    RateLimit::FetchError error;    // the deterministic failure (D5)
};
struct CompletedRefresh {
    std::vector<SkippedSource> skipped; // empty on a clean completion
};
struct FailedRefresh {
    RateLimit::FetchError error;    // the FIRST terminal error
};
using RefreshOutcome = std::variant<CompletedRefresh, FailedRefresh>;

void RefreshFinished(const RefreshOutcome &outcome);
```

- Emitted **exactly once per accepted `Update()`**, where "accepted"
  is defined as the worker's Idle→Updating transition (R1-6) — a
  refused `Update()` (already updating) emits nothing, and an update
  deferred while the worker is still initializing counts when it
  actually starts. Emission sites: `FinishUpdate` on success, the
  first `AbortUpdate` on failure (later straggler/second-failure calls
  already return early and must not emit it twice).
- **The terminal event observes an idle worker (R1-7, revised by
  R4-4).** Pinned ordering — success: final `ItemsRefreshed` → state
  set to Idle → `RefreshFinished`; failure: `AbortUpdate` sets Idle →
  `RefreshFinished`. State queries during the terminal fan-out see
  Idle — "refresh finished" and a busy worker are never presented
  together — but starting the next update from *inside* the fan-out
  is refused (next bullet); an observer that wants to chain queues
  its restart to the next event-loop turn. (Implementation note:
  `FinishUpdate` today sets Idle *after* its emit,
  `itemsmanagerworker.cpp:1279` — the terminal emit goes after that
  assignment.)
- **Terminal fan-out refuses reentrant updates (R2-2, R3-2;
  simplified by R4-4).** Idle-before-terminal invites a synchronous
  observer to start N+1 — but `RunUpdate` launches synchronously and
  a fail-fast future completes inline during the launch loop
  (`itemsmanagerworker.cpp:502-508`), so an update started mid-fan-out
  can emit N+1's signals — including its own terminal event, e.g. on
  a setup-cooldown fail-fast, which is production-reachable — *nested
  inside* N's fan-out, delivering N+1 events to later observers before
  their `RefreshFinished(N)`. That would defeat the
  ordering-is-identity contract above. The worker therefore holds a
  delivering-terminal flag across the `RefreshFinished` emit, and
  every `Update()` arriving while it is set is **refused** exactly as
  if an update were active. Rounds 2–3 instead accepted-and-deferred
  the first such request (a reservation, a queued start, a guard on
  the gap before the queued turn, a destruction case); round 4
  renegotiated the requirement that machinery served: no production
  caller starts an update synchronously from a completion signal —
  every `Update()` originates from a user action or the auto-refresh
  timer (`itemsmanager.cpp:29`), both of which arrive via the event
  loop and cannot land inside the synchronous fan-out. The contract
  is one rule: an observer that wants to chain a restart from the
  terminal event queues it (a queued invocation or zero-length
  single-shot); the queued request finds a genuinely idle worker and
  is accepted normally. Pinned:
  `terminalFanOutRefusesReentrantUpdate`.
- **First-error preservation (R1-6, R2-4).** `AbortUpdate()` currently
  receives no error; M2 states the plumbing: every value-level failure
  branch hands its `FetchError` to `StopUpdateForFailure`, which
  stores the update's first terminal error **before firing the
  throwable test fault hook** — the stopped-but-still-active window
  the catch-alls must recognize already has the error recorded; the
  per-fetch and orchestration catch-alls store an `Internal`-kind
  error the same way; `AbortUpdate` emits `FailedRefresh` with the
  stored error, which **resets at the next accepted update**. Later
  failures and settling stragglers cannot overwrite it (the
  already-terminal guard returns early). Every terminal path has a
  defined error: the one branch that previously held none — a 200
  wrapper missing its stash/character payload — is reclassified at
  the facade and leaves the terminal set entirely (D5, R2-4).
- **`Canceled` mapping (R1-6, completing D5's classification).** A
  `Canceled` result whose update token is stopped never reaches a
  handler — the post-await check discards it (network-redesign D6).
  A `Canceled` that arrives with an *unstopped* token (a stop driven
  from outside the update, e.g. shutdown) takes the ordinary terminal
  path and maps to `FailedRefresh` with kind `Canceled`; it is a
  failure of the update without being anyone's error.
- **Ordering invariant:** all `TabRefreshed` and `ChildrenReconciled`
  emits of an update precede its `RefreshFinished`; nothing of that
  update follows it. A delta
  arriving after a `RefreshFinished` belongs to a later update. This
  holds by construction: emits are synchronous in reply handlers, a
  failure branch aborts before any delta of that reply, and stopped
  stragglers never reach their handlers (post-await invariant). No
  update-id field is added to the signals — the ordering guarantee is
  the identity, matching the worker's token-is-identity design
  (network-redesign D6); pinned instead of parameterized.
- **Overlap tolerance:** the first terminal failure idles the worker
  immediately, so update N+1's deltas may interleave with update N's
  *settling* stragglers — but stragglers emit nothing, so consumers
  observe a clean `… deltas(N), RefreshFinished(N), deltas(N+1) …`
  sequence. Consumers must not assume the worker is quiescent after a
  `FailedRefresh` outcome; they only get the sequence guarantee.
- **No-rollback policy, stated explicitly:** deltas applied before a
  later terminal failure stay applied — in worker memory, in the
  published `ItemsManager` copy, in the datastore, and on screen
  (pinned: `appliedReplySurvivesLaterFailureInMemoryAndDatastore`; M2
  extends the pin to the published copy). A `FailedRefresh` outcome
  means "the refresh did not finish; everything you see is real but the
  update is incomplete" — the UI may say so (status bar text via existing
  `StatusUpdate` plus this typed event for anything that needs to
  branch), but nothing is undone. `StatusUpdate` remains cosmetic and
  is demoted from any semantic role (it never had a contractual one;
  now the typed event exists, nothing may grow one).

This is the concrete resolution of the emit-on-failure non-goal's
"revisit at M2", together with D5.

### D5. Per-tab failure policy: deterministic failures skip, transient failures stay terminal

Content-fetch failures (stash or character fetches; **not** list
fetches) are classified by `FetchError::Kind`:

- **`Parse` — skip and continue.** A payload that fails the facade's
  parse is deterministic: retrying is futile, and one bad tab must not
  abort an hours-long update (the 3.29 `flags: []` incident aborted
  whole updates on exactly this). The handler's failure branch, for
  `Parse` on a content fetch only: log, record the fetch source in the
  update's skipped list, count the fetch as received (the counter join
  must still reconcile; monotonicity preserved, P-STATUS), emit **no**
  delta — the atomic replace never ran, so the fetch source's previous
  items remain, in memory and datastore, exactly per M1's
  non-destructive semantics. The update completes; the terminal event
  reports `CompletedRefresh` with the skipped list (D4), and the final
  `ItemsRefreshed` publishes as usual (the skipped tab's list metadata
  is still upserted, and a successful final rebase freshens its
  surviving items' embedded metadata; the tab is **listed with stale
  contents** — not F55's listed-but-cold state, since its contents
  survive rather than being absent). **Skips are user-visible
  (R1-1):** the final status message states the count ("Received N
  tabs, M skipped") and the skipped sources are named in the log at
  warn level; the typed event carries the details for anything that
  needs to branch — including the shop gate (D8). A
  completed-with-skips refresh must never be indistinguishable from a
  clean one.
- **Everything else — unchanged: first failure is terminal.**
  `Network`, `Http`, `RateLimited`, `Protocol`, `Internal` keep the
  M1/network-redesign semantics (stop token, `AbortUpdate`, no final
  emit). Reasons: transient transport failures are exactly what a
  retry mechanism should absorb, and that mechanism is deferred (D11)
  — skipping instead of retrying would churn through a dead network
  for hours marking every tab skipped; `Protocol` is systemic (the API
  changed under us — continuing risks violating limits); `Http` mixes
  per-tab cases (404) with systemic ones (401/403) and is deferred to
  the retry design rather than split speculatively; `Internal` is a
  bug. `Canceled` reaches a handler only with an unstopped token and
  maps per D4's mapping. List-fetch failures of any kind stay
  terminal — the update cannot even define its batches without lists.
- **Missing-wrapper payloads become facade `Parse` errors (R2-4).**
  Today a 200 whose parsed wrapper lacks its stash/character
  sub-object aborts from worker branches that hold no `FetchError`
  (`itemsmanagerworker.cpp:901/1033`) — undefined inputs for D4's
  first-error plumbing. Resolved through D1 rather than by
  synthesizing an error in the worker: the post-F62 facade extracts
  that sub-object anyway (it must, to capture the raw bytes), so an
  absent payload is classified at the facade as `Parse` and the
  worker branches are deleted. Stated consequence, decided
  deliberately: the missing-wrapper case thereby moves from terminal
  into this D's skip set — it is deterministic per tab, retrying is
  futile, and one such tab must not abort hour ten. Pinned:
  `missingStashWrapperSkipsTab` / `missingCharacterWrapperSkipsTab`.

The scope here is deliberately minimal: one kind, on one fetch class,
with the proven incident behind it. Extending skip-and-continue (or
retry) to other kinds is the deferred per-tab-retry design's decision
(D11), and it inherits D4's outcome vocabulary ready-made.

### D6. The published-state contract: what streams and what stays snapshot-boundary

`ItemsManager`'s published copy is, at every instant during an update:

> the pre-update snapshot, with each applied per-fetch-source
> replacement (including empty ones) substituted in, in arrival order.

Exhaustively, by worker mutation:

| Worker mutation | Published when |
|---|---|
| Content replacement (atomic replace, incl. emptied source) | **Streamed** — `TabRefreshed` (D3) |
| Ghost-child drop on parent reply | **Streamed** — one aggregate `ChildrenReconciled` (D3, R5-2/R6-2) |
| List reconciliation: tab metadata upsert (renames/moves/colors, F15) | Snapshot-boundary — final `ItemsRefreshed` |
| List reconciliation: deleted tabs dropped with their items | Snapshot-boundary — final `ItemsRefreshed` |
| New-tab discovery (listed, unfetched) | Snapshot-boundary — final `ItemsRefreshed` |
| `RebaseItemLocations` of surviving items | Snapshot-boundary — success only (below) |
| Tab/item sorting | Snapshot-boundary — final `ItemsRefreshed` |

- **The rebase point stays `FinishUpdate` (success only).** This
  resolves the plan's fourth hard constraint by choosing its first
  branch: per-delta consumers tolerate stale embedded tab metadata on
  *surviving* (unrefetched) items until the final emit. The
  failed-update-mutates-published-state problem M1 solved does not
  return. What makes this tolerable: streamed items are **new** `Item`
  objects built from the update's reconciled (fresh) tab list — the
  delta lane never mutates a shared published object; only items
  untouched by the update keep old metadata, which is exactly today's
  between-refreshes reality. Mid-refresh, the UI may therefore show a
  mix of old and new tab labels across buckets — accepted, under the
  rendering rule below.
- **Buckets key on stable identity and render canonical metadata
  (R5-1).** Revision 5 claimed the metadata mix was invisible ("M2
  renders nothing new that juxtaposes one tab's old and new
  metadata") — false once D9 refilters mid-refresh, because `Search`
  buckets by `std::map<ItemLocation, Bucket>` (`search.cpp:239`) and
  `ItemLocation` *orders* stash locations by positional index while
  its *identity* is the stable id (`itemlocation.cpp:155`,
  `itemlocation.h:47`). Naive mid-refresh bucketing can therefore
  split one tab into old- and new-position buckets (moved tab), file
  fresh items under a stale header (renamed tab), or — found while
  verifying the review, and affecting regular tabs, not just
  Map/Unique parents — **merge two different tabs into one bucket**
  when a fresh delta's position collides with an unrefreshed tab's
  stale position. None of these exist today, where rebase precedes
  the single emit. The rule: **bucket identity is the stable
  `(type, id)`, and each bucket renders exactly one location's
  metadata — the freshest the UI has seen for that key** (a delta's
  location supersedes embedded item metadata; the final snapshot
  supersedes everything). The mechanism (e.g., a canonical-location
  map maintained across refilters) is implementation detail; the
  invariant and the three failure shapes are the requirement, pinned
  by `bucketsKeyOnStableIdDuringRefresh`. The inventory behind this
  rule ingests **every** delta's location anchor, empty deltas
  included (R6-1), and the empty-bucket source list —
  today built from the published tab list alone when unfiltered
  (`search.cpp:280-286`) — resolves through it, so an empty tab
  renamed, moved, or newly discovered mid-refresh renders correctly
  at the next refilter. This is what the parent plan's "must not
  render anything that would expose the mismatch" constraint
  concretely demands of M2.
- **Tab-list publication stays final-only.** `SetStashTabLocations`,
  and thus tab-level buyout keys and shop tab indexing, update at the
  final emit as today. Consequence: a delta may arrive for a display
  tab absent from the published tab list (a newly discovered tab in a
  full refresh). Buckets are built from item locations, so a delta
  **with items** materializes its bucket immediately with the delta's
  fresh location; an **empty** delta cannot (R6-1) — no item carries
  the location, and D9 deliberately has no metadata-only intersection
  trigger — so metadata carried only by an empty delta (a new,
  renamed, or moved empty tab) becomes visible at the current
  search's next refilter or the final snapshot, via the canonical
  inventory above. Accepted cost, the same shape as background
  captions. Tab-level buyouts for a new tab default to none until
  the final pass. Accepted.
- **Ordering of the published vector is unspecified mid-refresh.**
  Erase+append leaves the vector unsorted between deltas; the final
  emit restores the deterministic sorted order. No consumer may
  assume sorted published items mid-refresh (R7-4): D9's refilter
  does not care, and the one consumer that can observe mid-refresh
  state — a manual shop submission's value capture (D8) — renders
  from its own capture and sorts during generation (`shop.cpp:304`),
  so published order never reaches the forum post.

### D7. Per-delta scoped pricing: yes — item-local, fail-safe, final pass stays authoritative

Streamed items visibly unpriced for hours are a real cost at the
hours-long-refresh scale, and pricing the delta is O(delta items) with
no whole-collection component — so M2 prices per delta, scoped.

Revision 1's scoped pass argued its safety entirely from the success
path ("the final pass overwrites divergence"), which D4's no-rollback
design does not guarantee: an update can end without a final pass.
The pass is therefore restricted to steps that are safe **on both
outcomes** (R1-4):

On each applied delta, `ItemsManager` runs, **for the delta's items
only**:

1. Note-based item buyouts, mirroring `ApplyAutoItemBuyouts`'s
   per-item rule (including its clear-when-stale branch). Fail-safe
   because it is re-derivable: the price comes from the item's own
   note and the identical rule reproduces it on any later pass.
2. Tab-inheritance propagation, mirroring `PropagateTabBuyouts`'s
   per-item rule, reading the **currently published** tab-buyout
   state (`GetTab` as it stands — no fresh-metadata writes to the tab
   table). Fail-safe for the same reason: derived, re-derivable.
3. **Monotone refresh-lock additions**: if the delta item's buyout or
   its tab's published buyout `RequiresRefresh()` (and the tab is not
   remove-only), `SetRefreshLocked` is called — mirroring the lock
   *setting* half of `PropagateTabBuyouts` only. Locks are never
   cleared per delta; `ClearRefreshLocks` remains exclusive to the
   final pass. Monotone additions are fail-safe in the right
   direction: after a failed update the worst case is one redundant
   tab in the next checked refresh, never a game-priced tab silently
   dropped from it (locks feed `GetRefreshChecked`,
   `buyoutmanager.cpp:193`, which drives the worker's Checked
   selection, `itemsmanagerworker.cpp:398`). Revision 1 excluded all
   lock bookkeeping — exactly backwards for the failure case (R1-4).

**Tab-name auto-pricing (`StringToBuyout(tab_label)` → `SetTab`) is
final-pass-only.** Revision 1 ran it per delta; R1-4 showed that
mutates persistent, global tab-buyout state keyed by metadata whose
publication D6 deliberately keeps snapshot-boundary — after a failed
update the mutation would persist with no final pass to reconcile it
and no published tab list that explains it. Cost of the restriction —
the one real rename transient: a *renamed-to-a-price* tab's streamed
items inherit the old published tab price (or none) until a successful
final pass runs the auto-tab step.

Also excluded, as before: `MigrateBuyouts`, `CompressTabBuyouts`, and
any `Save()` scheduling beyond what `BuyoutManager`'s existing dirty
flag already does.

Two safety properties, both pinned:

- **Success — convergence:** the final whole-collection pass at
  `ItemsRefreshed` is unchanged and remains authoritative; any scoped
  divergence is overwritten within the same update.
  (`scopedPricingConvergesToFinalPass`: scoped passes followed by the
  final pass produce `BuyoutManager` state identical to the final pass
  alone, modulo `last_update` timestamps.)
- **Failure — fail-safety:** after deltas followed by a terminal
  failure, every published item whose buyout requires a refresh has
  its tab locked, and the tab-buyout table is unchanged from its
  pre-update state. (`scopedPricingIsFailSafeAcrossFailedUpdate`.)

Inheritance itself is rename-proof: `GetTab` keys on the stable
`location.id()`, not on label metadata (`buyoutmanager.cpp:101`), so
a renamed tab's streamed items find their existing tab buyout per
delta. (Revision 2 claimed a label-keyed lookup transient here; round
2 corrected it — no such transient exists.)

### D8. The final-emit contract: unchanged, with one deliberate exception — the shop gate

The worker's `ItemsRefreshed(m_items, m_tabs, initial_refresh)` and
everything downstream of it keep today's semantics and ordering —
`ItemsManager`'s copy + migration + three whole-collection buyout
passes + light re-emit; `Application`'s currency snapshot and shop
expiry; `MainWindow`'s full refilter of every search — with one
exception:

**Automatic forum submission moves from `ItemsRefreshed` to
`RefreshFinished`, gated on clean completion (R1-1).** Today a parse
failure aborts without posting; D5's skip-and-continue would have
silently converted that into auto-posting a shop containing stale
contents for tabs the user explicitly asked to refresh, and the
terminal event — ordered after `ItemsRefreshed` — arrives too late to
suppress a submission wired to the snapshot. So `Application`'s
auto-submission (`application.cpp:410-413`) reconnects to
`RefreshFinished` and fires only on `CompletedRefresh` with an
**empty** skipped list. A completed-with-skips or failed refresh
never auto-posts; manual submission stays available regardless. For
calibration, not comfort: partial refreshes already post stale
contents for *unselected* tabs today — the gate closes the new
hazard, a selected tab going silently stale into a post. Pinned:
`shopSubmitsOnlyOnCleanCompletion`.

**Submission input is captured by value at request time (R2-1).** The
gate governs when submission *starts*, not what it reads:
`SubmitShopToForum` is asynchronous — it first fetches the legacy
stash index (`shop.cpp:175-194`) and only the continuation reads
`ItemsManager::items()` and the buyouts (`shop.cpp:289`). Update N+1,
legally started the moment N's terminal event lands (D4), can stream
deltas into the published state N's "clean" submission would then
read — a hazard that is new under M2, because today `items()`
changes only at final snapshots. The shop therefore captures an
immutable snapshot of its submission input — the postable items'
identity, location, and buyout fields, **by value** — at the moment
submission is requested, and applies the stash index to that capture
when the index arrives. Value capture, not retained `shared_ptr`s:
N+1's successful `FinishUpdate` rebases the shared `Item` objects in
place, so a pointer capture would mutate under the submission.

**Automatic submission is a latest-eligible desired state, not one
post per clean refresh (R3-1).** At most one immutable submission job
is active and at most one automatic capture waits behind it. If clean
N+1 completes while N is active, N+1 becomes the waiting eligible
capture; if clean N+2 then completes, it replaces N+1. Intermediate
clean snapshots are deliberately coalesced — after a successful
active-job completion, the pipeline converges to the newest clean
eligible snapshot rather than making the forum observe every clean
refresh generation. A terminal submission failure deliberately halts
that automatic convergence per the failure policy below. A completed-
with-skips or failed item refresh never becomes eligible (the clean
gate above still applies).

Automatic-request admission therefore captures before applying the
busy policy: the busy path replaces the waiting automatic capture
instead of returning as today's `SubmitShopToForum` does. Manual
admission is deliberately different — it captures and starts when no
job is active, but may still refuse while a forum job is active.

Each active job owns all of its transport state: its immutable capture
(including every output-affecting item/buyout field, template,
realm/league, and target thread list), force/manual bit, returned
legacy stash index, rendered thread data and hash, request counter, and
thread progress. The existing shared `m_shop_data`, `m_shop_hash`, and
`m_requests_completed` must not be mutable transport state that a
waiting job can overwrite while the active job is still reading it.
After the legacy index arrives, **every job renders its own capture**;
submission correctness never depends on a cached-data boolean.
`m_shop_data_outdated` is replaced by monotonic input/cache revisions
for the preview/clipboard cache: `ExpireShopData()` advances the input
revision, and completing N can mark only N's revision rendered or
clean — never a newer revision. A rendered job may publish the preview
cache only if it is not older than the cache already there.

One accepted blind spot, stated deliberately (R4-2): deltas do not
advance the input revision — nothing may connect `Shop` to
`TabRefreshed` (design-review criterion below) — so mid-refresh the
preview/clipboard cache can report current while published state has
streamed past it. The clipboard then serves the same last-rendered
text it serves today; what M2 changes is only that published state
moves earlier, and the manual-submission rule below already
guarantees any *submission* re-renders. Accepted cost: the
`CopyToClipboard` outdated warning (`shop.cpp:627`) is
snapshot-granular, not delta-granular.

All terminal exits converge on one completion path. Success includes
the unchanged-hash no-post case: it releases the active job, advances
only that job's clean revision, and drains the newest waiting
automatic capture. Failure releases the active job but does **not**
drain automatically and does not advance any clean revision; discard
the waiting capture while leaving the current input revision dirty, so
the next clean automatic request or explicit manual request recaptures
the latest published state instead of retrying a possibly stale
snapshot or hammering an auth/network failure. The existing
rate-limit/security-token delayed retries remain inside one active job
and are not terminal exits. The failure policy deliberately does not
discriminate by kind (R4-3): a transient network blip and an auth
failure both halt automatic convergence until the next clean refresh
or a manual request. This is parity with today — a failed submission
has never retriggered before the next refresh — and kind-aware
draining belongs to the same future design that owns per-tab retry
(D11).

Two waiting-capture transitions, pinned (R5-6): **disabling shop
auto-update drops the waiting automatic capture** — the active job
finishes normally, but no new automatic job may start after the user
turned automation off. **A completed-with-skips or failed item
refresh arriving before the waiting clean capture drains does not
invalidate it** — keep-and-drain: the gate's invariant is
cleanliness, not recency; the capture is still the newest clean
published state, and posting it later is parity with today, where an
aborted refresh posts nothing and the last clean state stands.

One more transition (R6-4): **an output-affecting local mutation
drops the waiting automatic capture.** `ExpireShopData()` — buyout
edits (`mainwindow.cpp:506`), template or thread changes
(`shop.cpp:131/115`) — while an automatic capture waits invalidates
it: draining a pre-edit capture after the user's edit would post
data older than their intent while telling them the shop updated.
The edit reaches the forum through the next clean automatic capture
or a manual submission, exactly as today. The active job is
deliberately unaffected — its capture is immutable (R2-1) and posts
as captured; the edit follows in a later submission.

**Manual submission during an active item refresh captures and submits
the current published state** — deliberately accepted: deferring it to
a terminal outcome would block manual submission for the whole of an
hours-long refresh. The visible model may lag that state under D9, so
the precise contract is "the current published state is what the
manual request captures," not "what you see is what you post." A
manual job always renders its capture regardless of preview-cache
revision or the `force` hash-bypass flag. A manual request while a
different forum submission job is already active may continue to be
refused; M2 does not add a second manual queue. Pinned by the staged
shop tests below.

Restating the plan's hard constraints as design:

- **No per-delta forum submission** — `Application` invokes the shop
  from `RefreshFinished` as above; nothing connects `Shop` to
  `TabRefreshed`.
- **No per-delta whole-collection scans** — the delta path in
  `ItemsManager` (D3, D7) and `MainWindow` (D9) touches O(delta) items
  plus D3's permitted linear erase pass (R1-2, worker-parity bound)
  and, at most, one coalesced current-search refilter per freshness
  window (D9's explicitly budgeted exception, removed in M3).
- **No per-delta uncoalesced model reset** — every model reset on the
  delta path goes through D9's throttle; `TabRefreshed` never reaches
  `beginResetModel()` directly.

**F46 is absorbed by M2 (R1-9):** the debug uncategorized-items scan
in `ItemsManager::OnItemsRefreshed` (`itemsmanager.cpp:129-138`) is
gated behind `spdlog::should_log` (or deleted outright) as part of
M2's rework of that function, per the register entry's own request;
the F46 entry moves to the resolved ledger when that lands. The scan
never runs on the delta path either way.

M3 renegotiates the rest of the final-emit cascade; M2 does not.

### D9. UI application: intersection-gated, throttled, with a stated freshness bound

**Retired by M3 (July 31, 2026).** This decision and its machinery —
the throttle, its pins, and the restore path — are superseded by the
delta-native items model (`items-pipeline-m3.md`, D3, and the
supersession record in `m1-m3-result.md` §S8); the text below is the
record of what M2 shipped and why. The stable `(type, id)` keying
introduced here carries forward as M3's bucket keying.

`MainWindow` consumes `ItemsManager::TabRefreshed` under an explicit
five-rule state machine (R1-5 — this replaces revision 1's two-tier
description, which under-marked dirtiness and left a pending tick
orphanable by a tab switch):

1. **Every delta marks every search items-dirty** — aggregate
   reconciliations (`ChildrenReconciled`) included, the current
   search included, regardless of intersection. A new flag beside
   `m_states_dirty`, cleared per search by that search's own
   successful refilter, consumed by the same
   refilter-on-next-activation gate (`search.cpp:212`, extended to
   test either flag). Rationale: every delta changes the underlying
   `items()` for every search; intersection (rule 2) only decides
   *urgency* for the visible one, never *whether* a search is stale.
   A non-intersecting delta leaves the current search dirty too — its
   next activation-refilter is wasted work if nothing visible changed,
   accepted for the simplicity of one unconditional rule.
2. **If the delta intersects the current search, its throttled
   refilter is scheduled.** The timer is owned by the current search:
   a **non-resetting trailing throttle with period S** (the freshness
   bound). The timer is started by the first intersecting delta and is
   **not** re-armed by later arrivals — that is the anti-starvation
   half: under steady one-reply-per-20-seconds arrivals a resetting
   debounce would starve forever; this throttle guarantees the
   visible view is never more than **S plus one reset-plus-restore
   duration** behind the applied state (the tick fires at ~S and the
   view is current when its reset completes — ~0.5 s at the driving
   scale; post-freeze amendment), and
   resets at most once per S. One narrow, stated exception to that
   bound (R7-2): metadata carried only by an empty delta — a
   renamed, moved, or newly discovered empty tab (D6) — starts no
   timer, because no item intersects; it lands at the next refilter
   or final snapshot, and after a terminal failure may wait for user
   action indefinitely. Accepted: a metadata-only intersection class
   is heavier than the artifact (R6-1), and the change is invisible
   to any filtered search regardless (`search.cpp:277-286`).
   **S = 60 seconds**, chosen
   to dominate the ~20 s/tab arrival cadence by a small integer
   factor and **confirmed by the S1-M2 spike** (revision 9; the
   value was hands-judged, not argued — see the spike result below).
3. **When the timer fires, the current search refilters** — the model
   resets once, the existing restore machinery runs — **and clears
   only its own items-dirty flag.** Background searches stay dirty
   until their own refilter. Symmetrically, **any** successful
   refilter of the current search — user-initiated or a form-edit
   write-through (`ModelViewRefresh`, `mainwindow.cpp:736`) — clears
   its flag and **cancels its pending timer** (R5-5): the work is
   paid for, and the next intersecting delta starts a fresh S window
   rather than inheriting a stale deadline.
4. **A tab switch or search deletion cancels the old search's pending
   timer.** Nothing is lost: rule 1 already marked the old search
   dirty, and the dirty flag carries the update to its next
   activation. The newly shown search refilters on arrival iff it is
   dirty (the extended gate) — which also answers what "flushing" a
   user-initiated refilter means: the user just paid for a refilter,
   and the flags make it pick up every applied delta for free.
5. **The final `ItemsRefreshed` cancels any pending timer** and runs
   the existing full path, which refilters every search and clears
   all items-dirty flags.

Background tab captions are **not** recomputed per delta (each caption
requires that search's whole-collection refilter — exactly the cascade
M2 exists to avoid); they refresh when the search is activated or at
the final emit, as today. Accepted cost: background captions may be
stale during a long refresh.

Intersection is decided on the delta alone, O(delta items): a delta
intersects the current search iff any of its items matches the current
filter set, **or** any item currently in the visible filtered result
was fetched from the delta's fetch source (the removal half — an empty
or shrunken replacement must count as a visible change); a
`ChildrenReconciled` intersects iff any visible item under its parent
carries a key outside the expected set (R5-2/R6-2). The mechanism
for the removal test (e.g., a `FetchSourceKey` set maintained per
refilter) is implementation detail; both halves are the requirement.

**Automated acceptance tests (R1-5), against the existing fixture
(`mainwindowfixture.h` / `tst_mainwindow.cpp`):** the scenarios in
the criteria list below — removal-only intersection, throttle
non-rearming, tab switch before the tick, deletion with a pending
timer, final-snapshot cancellation, refilter cancellation (R5-5),
failure-survival of a pending tick (R5-3), and stable-identity
bucketing (R5-1). The throttle period is
injectable (constructor parameter or test hook) so the suite drives
it at milliseconds — `throttleDoesNotRearm` must not wait wall-clock
S (R2 minor). The state machine's correctness is pinned by tests; the
spike below judges only feel.

**Restore fidelity is a contract, not a hope (R6-3).** The parent
plan requires scroll and selection to survive a background tab
landing; the existing machinery cannot deliver that under a per-S
reset, on four verified counts: expansion is keyed by mutable header
text (`GetHeader()`, `mainwindow.cpp:709`), so a rename orphans it;
the refresh path restores expansion without saving it first
(`ModelViewRefresh`, `mainwindow.cpp:736-774` — only the tab-switch
and form-change paths save), so each reset restores stale state;
reselection matches by `shared_ptr` identity (`search.cpp:181`), so
every replaced item — exactly what M2 streams — loses selection; and
no tree scroll capture exists at all. Outcome (a) therefore includes
a fidelity contract: (1) expansion keyed by stable `(type, id)`;
(2) selection restored by stable item identity when the item still
exists in the view — a **global** identity lookup, not scoped to the
item's previous bucket, so an item moved to another tab mid-refresh
keeps its selection (the spike prototype fell short of this;
post-freeze amendment, `reselectionSurvivesCrossTabMove` below);
(3) scroll preserved across a throttled reset
(scrollbar value or an anchor — mechanism is implementation detail);
(4) the throttled path captures expansion, selection, and scroll
immediately before every reset. S1-M2 prototypes this contract along
with the throttle, so the spike judges cadence with fidelity in
place instead of measuring the old machinery's losses; the work is
productionized only under outcome (a). Under outcome (b), resets
remain at final emits and user actions, with today's machinery and
today's accepted losses. The stable-identity keying is not throwaway
either way it lands: M3's bucket-scoped operations need the same
keys.

**Spike, not paper (S1-M2):** whether one reset-plus-restore per
S = 60 s under the user's feet is acceptable steady-state UX cannot be
settled by argument. Before freezing this D's constants:
drive a scaled refresh (harness or live) with the throttle prototyped,
and judge scroll/selection/expansion survival by hand. Outcomes: (a)
acceptable → S is confirmed or tuned; (b) not acceptable → rule 2
degrades to "current search updates only at final emit or on user
action" for M2 (the freshness bound then applies only to intersection
*detection* — the staleness affordance contracted below — and
true in-place freshness waits for M3's bucket-scoped model ops); rules
1, 4, and 5 hold in either outcome. Either outcome ships M2; the spike
only picks between two already-specified behaviors. Under outcome
(b) the restore machinery (F23, F31, F32) is reused as-is; outcome
(a) requires the fidelity contract above (R6-3), whose
stable-identity keying carries forward into M3 — M3 retires only the
reset-based application.

**Result (revision 9, July 29, 2026): outcome (a); S confirmed at
60 s.** The spike ran on branch `spike/s1-m2-throttle` (post-F62
master) against the real `MainWindow` with the throttle and the
R6-3 fidelity contract prototyped — all of it except the global
cross-tab reselection, which the prototype scoped to the item's
previous bucket (a recorded narrowing; the
`reselectionSurvivesCrossTabMove` pin below is the production
obligation); Tom drove the harness by hand at
the driving scale (~101k items / 2,000 tabs) and judged the cadence
and fidelity acceptable. Expansion (including across tab renames,
via stable keys), stable-identity reselection, and scroll anchoring
survived ticks in hand-driving and in automated scenarios (rename
into/out of an active Tab filter — both intersection halves —
expansion surviving a rename of the expanded tab itself, and
selection following the stable item id to a moved row with the
replacement object adopted; the full pin list stays a
production obligation, R1-5).
Reset latency, Release build, worst-case unfiltered search:
~455 ms/reset at 101k, ~5.4 s at ~976k — dominated in both cases by
the post-reset whole-model re-sort (~86% / ~94%), with the fidelity
machinery collectively negligible (bounded by the few-ms residual
after filter and sort; not individually timed). **Supported
envelope (post-freeze amendment, Tom's decision, July 29, 2026):**
outcome (a)'s constants are supported at the driving scale, and the
freshness guarantee is S plus one reset-plus-restore duration. At
~1m items each mid-refresh reset costs ~5.4 s — a recurring cost
today's final-only path does not pay — and this is **explicitly
accepted for M2**: outcome (b) would not cure that scale
(user-initiated refilters pay the same sort), and M3's bucket-scoped
application retires the streaming reset entirely. The ~1m sort cost
is recorded as an input for the M3 spec in the parent plan
(bucket-scoped ops retire the streaming reset; the sort levers —
precomputed sort keys, lazy sorting of visible buckets, born-sorted
buckets — are model-layer choices M3 owns; the sort was timed as a
whole, so comparator cost is the suspected, unprofiled driver). The outcome-(b) affordance contract (R7-1) below
is retained as record but **not selected**; the outcome-(a)
acceptance criteria bind. Full evidence: `s1-m2-spike-result.md`
(retired to git history August 1, 2026).

Stated honestly (R5-3): outcome (b) is a **renegotiation of the
parent plan's freshness-bound input**, not a fulfillment of it —
after streamed deltas end in terminal failure there is no final
emit, so under (b) the visible model stays stale until user action,
unboundedly. Choosing (b) therefore updates the parent plan's inbox
note in the same commit and replaces the timer-dependent acceptance
criteria below (marked outcome-(a)) with the outcome-(b) affordance
criteria below (R7-1). Under outcome (a) no such gap exists for
item-bearing deltas: a pending tick survives a terminal failure and
fires (`pendingTickSurvivesTerminalFailure`), so the S bound holds
across failed updates too — metadata-only empty deltas remain the
narrow R7-2 exception in either outcome.

**Outcome (b)'s affordance contract (R7-1)** — defined now so the
spike chooses between two fully specified behaviors rather than one
behavior and a sketch: each search tracks, beside its items-dirty
flag, the set of distinct display tabs (stable `(type, id)`) whose
deltas intersected it since its last successful refilter. The
affordance renders the current search's set. It **appears** when the
first intersecting delta lands after that search's last successful
refilter; it **counts** those distinct tabs ("view is behind — N
tabs updated"); it **updates** synchronously with delta application,
so S degrades to a trivially met detection bound and no timer exists;
and it **clears**, per search, with the same successful refilter that
clears that search's items-dirty flag — user action, activation,
form edit, or the final-snapshot path. After a **terminal failure**
it persists until a user-driven refilter clears it: that visible
persistence *is* the renegotiated staleness outcome (b) accepts,
surfaced instead of silent. Rules 1, 4, and 5 are unchanged; rule
3's timer never runs. Presentation (statusbar text, a badge on the
search tab, or similar) is implementation detail; the trigger,
count, clear, and failure semantics above are the contract.

**Pre-freeze evidence exception (R3-4, R7-3).** S1-M2 is the single
named exception to the parent plan's doc-first production rule: it
resolves a genuine product decision that argument cannot settle. Its
prototype lives on a dedicated non-production branch or in an
isolated harness, is discarded or left unmerged, and cannot become
production M2 code before freeze. Revision 4 authorized it; revision
6 moved M2-M2 pre-freeze beside it (R5-4); revision 8 moved M2-M2
back out to the first implementation checkpoint (R7-3) — D3 now
freezes a determinate conditional, so nothing stays open, and the
pre-freeze run had grown into a throwaway vertical slice whose miss
branch would have implemented a remedy before freeze. S1-M2 runs
against a post-F62 build (R6-6) with the restore-fidelity contract
prototyped (R6-3), uses a realistically large collection, and
records reset latency alongside the subjective cadence and
restore-fidelity judgment (R7-3). Revision 9 records its result,
selects D9's behavior and S (if applicable), and freezes the spec
before production implementation begins. This is a narrow
evidence-gathering exception, not a general license to implement an
unfrozen milestone. **Discharged:** the spike ran July 29, 2026 as
authorized — non-production, on `spike/s1-m2-throttle`, post-F62,
fidelity prototyped, realistic collections, latency recorded — and
this revision records its result. The branch stays unmerged; M2-M2
reuses its dataset generator (`tests/spikedataset.h` on that branch,
ported fresh — deterministic seeds make the datasets reproducible
reply/removal shapes).

### D10. Status-burst coalescing: measurement-gated

Network-redesign D6 left the batch-submit `QueueUpdated` burst
(thousands of synchronous emissions in one loop turn at the 2,000-tab
scale) to "coalesce on the UI side if the status dialog measurably
stutters". M2 keeps that as a **measurement gate, not a decision**:
instrument a 2,000-entry submission (the offline harness can drive it)
and measure status-widget frame time. If it stutters, the fix is a
UI-side coalesce of the existing signal (same throttle pattern as D9,
much smaller S); the limiter is not touched either way
(network-redesign is frozen). If it does not stutter, nothing is
built. Recorded as measurement M1-M2 in the open-items list.
**Discharged:** the measurement ran July 31, 2026 (post-M3, as the
recorded follow-up) and the gate fired — ~23 ms of synchronous
status-widget handler time per 2,000-entry burst, over one 60 Hz
frame, dialog visible or not. The prescribed coalesce was built in
`RateLimitDialog` (latest-value-per-policy map, non-resetting
single-shot flush timer, S = 100 ms) and the rerun passed with the
burst at ~0.9 ms. Evidence and rerun: `m1-m2-result.md`.

### D11. Deferrals, each with its reason

- **Per-tab retry mechanism and durable resume.** D5 took the
  classification decision (the load-bearing part per the inbox) and D4
  built the outcome vocabulary a retry design needs; the mechanism —
  where bounded retries live without violating the frozen network
  spec's retry semantics, which `Http` kinds join the skip set, and
  resume-what-wasn't-fetched across aborts/restarts — is its own
  design. It is not required to ship streaming, it touches the frozen
  network layer, and datastore per-tab persistence already gives
  restart durability of applied content. Deferred to a follow-on spec;
  nothing in M2 forecloses it.
- **Whole-update replacement/coalescing and user cancel.** An
  `Update()` during an active update stays refused (network-redesign
  D6's update-state policy). Streaming actually sharpens the future
  design: once progress is visible and durable, "cancel" is just
  request_stop with a `FailedRefresh{Canceled}`-flavored outcome and
  everything applied stays — but the semantics of *replace* (which
  selection wins, what happens to in-flight batches) expand M2
  considerably, exactly as the plan warned. Deferred; D4's outcome
  event is the hook it will need.
- **Reprioritization.** Deferred with its network-redesign rationale
  intact (D6/R7: the stop token is per-update; per-entry cancellation
  does not exist and would need a mechanism deliberately not designed
  now). Nothing in M2 makes it harder; D2 keeps the fetch source as
  the natural unit a future prioritizer would reorder.
- **Guardian/skills ingestion (F63)** stays deferred by its own
  decision; noted here only because a future ingestion adds fetch
  sources — the delta vocabulary (D2/D3) absorbs new sources without
  spec change.

## Acceptance criteria

Worker-level (offline fake-network harness, extending the M1 suite):

- `deltaMatchesAppliedReplacement` — every accepted content reply
  emits exactly one **primary** replacement `TabRefreshed` whose key
  is the reply's `FetchSourceKey` and whose items are exactly the
  applied replacement, followed by at most one `ChildrenReconciled`
  carrying the reply's expected-child set, in that order (R1-8,
  wording superseded by R5-2/R6-2) — all after the replace and before
  `CheckUpdateFinished`.
- `emptyDeltaEmptiesFetchSourceOnly` — an empty replacement empties
  that fetch source in the published copy and removes nothing else;
  no tab disappears from the published tab list mid-refresh.
- `parentReplyReconcilesChildrenAgainstExpectedSet` (R5-2, R6-2) — a
  parent reply that stops listing children yields one
  `ChildrenReconciled` whose expected set is the parent plus its
  listed children; the published parent bucket loses exactly the
  items whose keys fall outside that set, in one erase pass.
- `reconcileErasesGhostsAcrossFailedUpdates` (R6-2) — update N's
  list deletes parent P (worker and datastore drop P and child C);
  N fails, so the published copy keeps both. P is listed again in
  update N+1, whose parent reply no longer lists C: applying N+1's
  `ChildrenReconciled` erases C's published items even though the
  worker had nothing left to erase — the expected set reconciles the
  published baseline, not worker memory.
- `deltasNeverFollowTerminalEvent` — per update: all `TabRefreshed`
  and `ChildrenReconciled` emits precede `RefreshFinished`; after a
  `FailedRefresh` outcome, settling stragglers emit nothing (extends
  W-IDENTITY to both delta signals).
- `terminalEventExactlyOncePerUpdate` — one `RefreshFinished` per
  accepted `Update()` (accepted = the Idle→Updating transition, R1-6),
  on both success and first failure; second failures and stragglers
  emit no second event, and `FailedRefresh` carries the **first**
  terminal error even when later failures occurred (R1-6). The worker
  is Idle when the event is observed, on both paths (R1-7).
- `parseFailureSkipsTabAndUpdateCompletes` — a deterministic `Parse`
  failure on one content fetch: no delta for that source, its previous
  items survive in memory and datastore, counters reconcile, the
  update completes, and the outcome lists the skipped source with its
  error. A `Network` failure on the same fixture stays terminal.
- `missingStashWrapperSkipsTab` / `missingCharacterWrapperSkipsTab`
  (R2-4) — a 200 whose wrapper lacks its stash/character sub-object
  surfaces as a facade `Parse` error and takes the skip path, with the
  source and error in the outcome's skipped list.
- `terminalFanOutRefusesReentrantUpdate` (R2-2, R4-4) — with two
  terminal observers where the first calls `Update()` synchronously
  from inside `RefreshFinished(N)`: the call is refused, the second
  observer still receives `RefreshFinished(N)` with no nested N+1
  signal before it, and a restart the first observer queues to the
  next event-loop turn is accepted and runs normally.
- `publishedStateIsSnapshotPlusAppliedDeltas` — at any point
  mid-update, `ItemsManager::items()` equals the pre-update snapshot
  with applied replacements (keyed by `FetchSourceKey`) substituted
  and applied `ChildrenReconciled` reconciliations replayed (keys
  under the parent outside the expected set erased);
  after a mid-update failure it stays there (extends
  `appliedReplySurvivesLaterFailureInMemoryAndDatastore` to the
  published copy).
- `scopedPricingConvergesToFinalPass` — scoped per-delta pricing
  followed by the final pass yields `BuyoutManager` state identical to
  the final pass alone, modulo `last_update` timestamps; no scoped
  pass ever calls `ClearRefreshLocks`, and its only lock mutations are
  additions (D7).
- `scopedPricingIsFailSafeAcrossFailedUpdate` (R1-4) — after deltas
  followed by a terminal failure: every published item whose buyout
  `RequiresRefresh()` — items in remove-only tabs excluded, matching
  D7 rule 3's deliberate exclusion (R5-7) — has its tab
  refresh-locked, and the tab-buyout table equals its pre-update
  state (no tab-name auto-pricing leaked).
- `parentBucketMayMixChildGenerationsMidRefresh` — documented-behavior
  pin for D2's accepted transient, so the mixing is asserted
  deliberate rather than rediscovered as a bug.

Shop-level (extending the existing `tst_shop` suite):

- `shopSubmitsOnlyOnCleanCompletion` (R1-1) — with shop auto-update
  enabled: a clean `CompletedRefresh` triggers exactly one automatic
  submission; a completed-with-skips outcome and a `FailedRefresh`
  trigger none.
- `shopSubmissionUsesCapturedSnapshot` (R2-1) — staged: request
  submission for update N, hold the stash-index future, begin N+1 and
  apply a delta (including a rebase-visible change), resolve the
  future — the generated shop reflects N's captured input, not N+1's
  partial state.
- `newestCleanSnapshotSubmitsAfterActive` (R3-1) — hold N's active
  submission, complete clean N+1, finish N, and assert N+1's captured
  state is then rendered and submitted rather than refused or cleared.
- `automaticSubmissionCoalescesLatestEligible` (R3-1) — while N is
  active, complete clean N+1 and then clean N+2; after N succeeds,
  exactly N+2 waits/runs and N+1 is deliberately superseded.
- `manualSubmissionRendersCapturedPublishedState` (R3-1) — after a
  streamed delta with a preview cache whose revision still appears
  current, a forced manual request during the item refresh renders its
  captured published state instead of reusing cached pre-delta text.
- `olderSubmissionCannotCleanNewerInput` (R3-1) — expire revision N+1
  while N is active, then finish N; N's completion advances only N's
  revision and the cache remains dirty until N+1 renders.
- `failedSubmissionDoesNotDrainPendingAutomatic` (R3-1) — terminally
  fail N while a newer clean automatic capture waits; no automatic
  forum request starts from the failure exit, no clean revision
  advances, and a later request recaptures current published state.
- `disablingAutoUpdateDropsWaitingCapture` (R5-6) — with a clean
  capture waiting behind an active job, disable shop auto-update,
  then finish the active job successfully; no new automatic job
  starts.
- `skippedRefreshDoesNotInvalidateWaitingCapture` (R5-6) — with a
  clean capture waiting behind an active job, complete a later item
  refresh with skips, then finish the active job; the clean capture
  still drains and submits (keep-and-drain).
- `expireDropsWaitingCapture` (R6-4) — a template or buyout change
  while an automatic capture waits drops the capture; finishing the
  active job drains nothing, and the next clean refresh recaptures
  with the edit included.
- `activeJobUnaffectedByLocalEdits` (R6-4) — a buyout change while a
  job is active: the job posts its captured state unchanged; no
  re-render occurs.

UI-level (against the existing `MainWindow` fixture,
`mainwindowfixture.h` / `tst_mainwindow.cpp` — R1-5). **S1-M2
selected outcome (a) (revision 9), so the timer-dependent and
outcome-(a) criteria below bind; the outcome-(b) affordance criteria
at the end of this list are retained as record only and do not
apply** (R5-3, R7-1):

- `backgroundDeltaLeavesModelUntouched` — a delta not intersecting the
  current search performs no model operation and marks every search
  items-dirty, including the current one.
- `removalOnlyDeltaIntersects` — an empty delta whose fetch source has
  items in the visible filtered result schedules the throttled
  refilter (the removal half of the intersection test).
- `throttleDoesNotRearm` — deltas arriving faster than S produce at
  most one refilter per S; the first delta's deadline is not pushed
  back by later arrivals.
- `tabSwitchBeforeTickPreservesDirty` — switching searches with a tick
  pending cancels the timer; the old search refilters on its next
  activation via its items-dirty flag; nothing is lost.
- `searchDeleteCancelsPendingTimer` — deleting the current search with
  a tick pending fires nothing against the dead search.
- `finalSnapshotCancelsPendingTick` — the final `ItemsRefreshed`
  cancels a pending tick and the full path clears all items-dirty
  flags.
- `successfulRefilterCancelsPendingTick` (R5-5) — a user-initiated
  or form-edit refilter of the current search with a tick pending
  cancels the timer and clears the flag; no redundant reset follows.
- `pendingTickSurvivesTerminalFailure` (R5-3, outcome (a)) — deltas
  followed by a terminal failure with a tick pending: the tick still
  fires and the view catches up within S plus one reset-plus-restore
  duration despite no final snapshot (post-freeze amendment: the
  bound includes the reset itself).
- `bucketsKeyOnStableIdDuringRefresh` (R5-1) — after mid-refresh
  deltas for a moved tab, a renamed tab, and a tab whose fresh
  position collides with an unrefreshed tab's stale position: the
  refiltered view holds exactly one bucket per stable tab id,
  rendered with the freshest metadata seen for that key, and two
  distinct tabs never merge into one bucket.
- `emptyDeltaMetadataLandsAtNextRefilter` (R6-1) — empty deltas for
  a renamed, a moved, and a newly discovered empty tab trigger no
  refilter (no intersection), but the next refilter of an
  **unfiltered** search — whatever its cause — renders those buckets
  from the canonical inventory's fresh anchors. Filtered searches
  hide empty buckets by design (`search.cpp:277-286`, R7-2); the
  fresh anchors reach their view when the filter clears.
- `expansionSurvivesRenameByStableKey` (R6-3, outcome (a)) — expand
  a bucket, rename its tab via a delta, let the throttled reset run;
  the bucket is still expanded under its new header.
- `selectionSurvivesReplacementByStableIdentity` (R6-3, outcome
  (a)) — select an item, refresh its tab (new `Item` objects), let
  the throttled reset run; the logically same item is reselected,
  and a genuinely removed item clears the selection.
- `scrollAndCaptureSurviveThrottledReset` (R6-3, outcome (a)) —
  scroll position survives a throttled reset, and expansion changed
  after the last explicit save survives too (capture happens
  immediately before every reset). When the anchored top row was
  removed by the delta, restoration falls back to the raw scrollbar
  value — never to scrolling the anchor's bucket header to the top
  (post-freeze amendment; the spike prototype initially got this
  wrong).
- `reselectionSurvivesCrossTabMove` (R6-3, outcome (a), post-freeze
  amendment) — with an item selected, one delta removes it from its
  tab and another lands it in a different tab; after the tick the
  selection follows the item by stable identity. The lookup must be
  global (index-backed, not a bucket-scoped or whole-model scan per
  reselect).
- `staleAffordanceCountsIntersectingTabs` (R7-1, outcome (b)) —
  deltas for N distinct display tabs intersecting the current search
  raise its indicator to N synchronously, with no model operation; a
  delta intersecting only a background search leaves the visible
  count unchanged (rule 1 still marks every search dirty).
- `staleAffordanceClearsPerSearch` (R7-1, outcome (b)) — a
  successful refilter of the current search clears its count and
  indicator; a background search's count survives until its own
  refilter, exactly like its items-dirty flag.
- `staleAffordanceSurvivesTerminalFailure` (R7-1, outcome (b)) —
  deltas followed by a terminal failure: the indicator persists with
  its count until a user-driven refilter clears it; no timer fires
  and no final emit rescues it.

Design-review criteria (checked in review, not runnable):

- No `TabRefreshed` connection reaches `Shop`, forum submission,
  currency, `MigrateBuyouts`, or any whole-collection buyout pass;
  automatic submission is connected to `RefreshFinished` and gated per
  D8.
- Shop submission transport state is job-local; no active job reads
  live `ItemsManager`/buyout state after capture or shares mutable
  rendered data, hash, index, or progress with a waiting job. At most
  one newest clean automatic capture waits, and terminal failure never
  drains it automatically (R2-1/R3-1).
- The delta path performs O(delta) work everywhere except D3's
  permitted linear erase passes — the primary replacement plus at
  most one aggregate reconciliation per reply (worker parity,
  R1-2/R5-2) — and D9's single budgeted coalesced refilter.
- The persistence and presentation lanes share no payload types.
- Worker and `ItemsManager` erase by the same `FetchSourceKey`
  predicate (R1-3).
- No path starts an update synchronously from inside terminal
  fan-out — chained restarts are queued (R4-4) — and the M2-M2 report
  attributes worker and manager costs separately before selecting a
  remedy (R3-3/R4-1).

## Open items requiring spike or measurement (not argument)

- **S1-M2 (spike, pre-freeze — RESOLVED in revision 9):** ran
  July 29, 2026, post-F62, on `spike/s1-m2-throttle`, with the
  restore-fidelity contract prototyped (R6-3) and realistic
  collections (101k and ~976k items). **Outcome (a); S confirmed at
  60 s**; fidelity held hands-on and under automated scenarios;
  reset latency recorded (~455 ms at 101k, ~5.4 s at ~976k, the
  post-reset whole-model re-sort dominant in both). Evidence:
  `s1-m2-spike-result.md` (retired to git history August 1, 2026);
  full result in D9.
- **M1-M2 (measurement, blocks nothing) — RESOLVED July 31, 2026:**
  2,000-entry `QueueUpdated` burst vs. status-widget frame time;
  builds the D10 coalesce only if it stutters. Not run during M2
  implementation (it blocks nothing); recorded as a post-M2
  follow-up and run after M3. **It stuttered** — ~23 ms of
  synchronous handler time per 2,000-entry burst (~95% the status
  dialog's per-emission `setText`, paid even with the dialog
  hidden) — so the prescribed coalesce was built and validated by
  rerun (burst now ~0.9 ms). Evidence: `m1-m2-result.md`; pins in
  `tst_ratelimitdialog`.
- **M2-M2 (measurement, first implementation checkpoint — blocks M2
  completion, R2-3/R3-3/R4-1/R5-4/R7-3) — RESOLVED July 29, 2026:**
  the complete synchronous reply application on representative 100k
  and 1m datasets in a recorded Release environment, with
  per-component attribution (worker erase, manager erase,
  persistence, pricing, UI intersection/fan-out). The manager's
  marginal erase cost (< 2 ms at 100k / < 16 ms at 1m) gates the
  required manager remedy; the whole-path budget (same thresholds,
  one frame at 1m) gates a required real remedy for the dominant
  component. Ran at the stage-4 checkpoint of production
  implementation on the real reply path, reusing S1-M2's harness
  datasets with fixed recorded reply/removal shapes. **Every budget
  MISSED** (whole path 7.38 ms @100k / 74.93 ms @1m; four
  O(all-items) erase passes were ~99% of the path, split evenly
  worker/manager). Tom selected the named remedy PAIR at the hold
  point — the required manager source-keyed map plus the symmetric
  worker-side store (`SourceKeyedItems`, both sides) — and the R6-6
  validation rerun passes every budget with more than an order of
  magnitude of headroom (0.234 ms @100k, 1.225 ms @1m). Full
  addendum, environment, attribution, and rerun tables:
  `m2-m2-result.md` (beside this spec).

## Input traceability

| Inbox item / constraint (`items-pipeline.md`) | Consumed by |
|---|---|
| Hard constraint: no per-delta forum submission | D8 |
| Hard constraint: no per-delta whole-collection scans | D8, D7, D9 |
| Hard constraint: no per-delta uncoalesced model reset | D8, D9 |
| Hard constraint: rebase does not compose with streaming | D6 (rebase stays at success-only `FinishUpdate`; stable-identity bucket rule, R5-1) |
| Persistence signal surface partly exists; confirm the split | D3 (two-lane split + drift guard) |
| D6 deferrals: whole-update replacement, reprioritization, per-tab retry/durable progress, `QueueUpdated` coalescing | D5 (classification decided), D10 (measurement-gated), D11 (reasoned deferrals) |
| Overlap: new update active while stragglers settle | D4 (ordering invariant + overlap tolerance) |
| F62 decided-not-implemented; deltas never carry bytes/API objects | D1 (sequencing + inherited constraint), D3 |
| Delta carries Items keyed by (type, fetch-source id); atomic-unit choice | D3 (shape), D2 (unit: fetch source) |
| Typed terminal event + explicit no-rollback policy | D4 |
| Buyout scoping per delta; mind `ClearRefreshLocks` | D7 |
| Persistence/presentation split; deleted-tab expression | D6 (table; deletion is snapshot-boundary; empty delta ≠ deletion) |
| Freshness bound, not just coalescing | D9 (non-resetting throttle, S, both halves stated; metadata-only empty deltas are a stated narrow exception, R7-2; outcome (b) would renegotiate this input, R5-3/R7-1) |

Every inbox item is consumed; the emit-on-failure non-goal's "revisit
at M2" is resolved by D4+D5+D8's shop gate.

Review rounds 1–7 (R1-1…R1-9, R2-1…R2-4 plus four corrections,
R3-1…R3-4 plus one wording correction, R4-1…R4-4 plus one record
correction, R5-1…R5-7, R6-1…R6-6, R7-1…R7-4, July 27–28, 2026) are
incorporated throughout — verdicts and resolutions in
`items-pipeline-m2-reviews.md`, summarized in its revision log.
Round 4 (an in-repo audit) deleted the terminal-deferral machinery
by renegotiating the clause it served (R4-4); round 5 keyed
mid-refresh bucket rendering on stable identity (R5-1) and moved
M2-M2 pre-freeze (R5-4); round 6 made the child reconciliation carry
the authoritative expected set (R6-2) and added D9's restore-fidelity
contract for outcome (a) (R6-3); round 7 — the closing round —
specified outcome (b)'s affordance contract (R7-1), carved the
metadata-only empty-delta exception into D9's freshness bound
(R7-2), moved M2-M2 back out to the first implementation checkpoint
(R7-3, reversing R5-4's placement now that D3 freezes a determinate
conditional), and corrected D6's mid-refresh consumer note (R7-4).
The shaping decisions D1/D2 survived all seven rounds unchanged. The
pre-freeze sequence completed as planned: F62 merged (PR #183), the
S1-M2 spike ran July 29, 2026 (outcome (a), S = 60 s), and this
revision — revision 9 — records that result and **freezes the
spec**. The review series is closed: remaining risk is retired by
evidence — S1-M2 discharged pre-freeze, M2-M2 and M1-M2 in
implementation — not further argument. M1-M2 blocks
nothing. The legacy bare-id keying exposure is tracked as F66 in the
findings register (R6-5; renumbered from F64 at the July 28 master
merge), outside M2's scope.
