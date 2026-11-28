# Items Pipeline M2 Spec — Review History

**Companion to `items-pipeline-m2.md`** (split out per the convention
`network-redesign.md` arrived at when its review appendix outgrew it —
here from round 1, before any appendix accumulated). This file
preserves the M2 spec's decision history: review-round finding tables
(`R1-*`, `R2-*`, …), round narratives, and the revision log. The spec
cites these IDs inline and records only current decisions; new review
rounds and revision-log entries append here, with the spec's status
line updated in the same commit. Where a later round supersedes an
earlier resolution, both are recorded.

`R<round>-<n>` means a finding from that numbered review round of this
spec — deliberately distinct from the cleanup register's permanent
`F` numbers and from the network redesign's `ER`/`IR`/`R4`+ series.

## Round 1 — external review (July 27, 2026)

Read-only design/code review of spec revision 1 (commit `aaa70f1e`) by
an external reviewer (different model). Overall verdict: direction
sound — the two shaping decisions (D1 F62-first sequencing, D2
fetch-source atomic unit) were not challenged — but not freezable:
several contracts would have either violated M2's own constraints or
forced design choices during coding.

Every code-level claim was re-verified against `d995840b` before
acceptance (anchors noted per finding below). All nine findings were
accepted. Eight were resolved as the reviewer recommended; R1-2 was
resolved by the alternative the reviewer offered (relax-and-bound)
rather than their preferred option (indexed representation), with the
reasoning recorded.

| ID | Group | Finding | Status |
|---|---|---|---|
| R1-1 | Failure semantics / shop | D5's completed-with-skips outcome reaches the unchanged D8 final cascade, so `Application` auto-submits the forum shop containing stale contents for the skipped tabs — and `RefreshFinished` (ordered after `ItemsRefreshed`) arrives too late to suppress it. Today a parse failure aborts without posting. | Resolved in D8/D4: automatic submission moves to `RefreshFinished`, gated on clean completion. Skips get a user-visible surface (D5). Pinned `shopSubmitsOnlyOnCleanCompletion`. |
| R1-2 | Published-state storage | The spec claims O(delta) work in `ItemsManager`, but its only storage is a flat `Items` vector, so finding a replacement's old items is O(all items) per delta — an unacknowledged contradiction with D8 and the acceptance criteria. | Resolved in D3 by the reviewer's second option: the flat vector stays and one linear predicate-only erase pass per delta is explicitly permitted, bounded by worker parity (`RemoveItemsFetchedBy` is the identical per-reply pass, shipped in M1). The source-keyed map with a lazily rebuilt flat vector is the named fallback if measurement disagrees, and the natural M3 direction. |
| R1-3 | Replacement key | The M2 contract keys replacements by (type, fetch id) but the worker's `RemoveItemsFetchedBy` erases by fetch id alone (`itemsmanagerworker.cpp:451`); a cross-type id collision would make worker and published state diverge, breaking the snapshot-plus-deltas invariant. | Resolved in D3: a named `FetchSourceKey{type, fetch_id}` keys application in both the worker and `ItemsManager`; the worker's erase predicate gains the type field as part of M2. |
| R1-4 | Scoped pricing | D7's safety argument ("final pass overwrites divergence") only holds on success; D4 deliberately allows deltas followed by failure, where no final pass runs. Tab-name auto-pricing would then have mutated persistent tab-buyout state keyed by unpublished metadata, and the lock exclusion means a game-priced item can remain unlocked and drop out of the next checked refresh (locks feed `GetRefreshChecked`, `buyoutmanager.cpp:193`, which drives the worker's Checked selection, `itemsmanagerworker.cpp:398`). | Resolved by rewriting D7 to the reviewer's policy: scoped pass = note-based pricing + inheritance from published tab buyouts only; tab-name auto-pricing stays final-only; locks add monotonically per delta and are cleared only by the successful final pass. Pinned `scopedPricingIsFailSafeAcrossFailedUpdate`. |
| R1-5 | UI state machine | D9's two-tier wording marks only non-intersecting-branch searches dirty (every delta invalidates every search), and has a transition hole: an intersecting delta's pending tick can be orphaned by a tab switch, because `Search::FilterItems` skips refiltering on `TabChanged` unless a dirty flag is set (`search.cpp:212`). "Flush pending" was unspecified. | Resolved by replacing the two tiers with the reviewer's five-rule state machine (all searches dirty on every delta; timer owned by the current search; refilter clears own flag; tab switch/delete cancels the timer, the flag preserves the work; final snapshot cancels and clears through the full path) and adding five automated `MainWindow` acceptance tests against the existing fixture (`mainwindowfixture.h` — verified present). The S1-M2 spike narrows to UX-feel judgment. |
| R1-6 | Terminal event shape | `RefreshOutcome` permitted invalid states (both `skipped` and `error` always present), `SkippedTab` had no fields, `AbortUpdate()` receives no error today so first-error preservation was unstated, `Canceled` was omitted from D5's classification, and "accepted update" was undefined. | Resolved in D4: sum type `std::variant<CompletedRefresh, FailedRefresh>`; skipped entries reuse `FetchSourceKey`; first-error capture at `StopUpdateForFailure` with explicit mappings for catch-alls and unstopped-token `Canceled`; "accepted" defined as the Idle→Updating transition. |
| R1-7 | Terminal event ordering | `FinishUpdate` sets Idle *after* emitting (`itemsmanagerworker.cpp:1279`); a terminal event inserted before that assignment means a synchronous observer reacting to "finished" by starting an update is refused as still updating. | Resolved in D4: pinned ordering — final `ItemsRefreshed` → Idle → `RefreshFinished` on success; Idle → `RefreshFinished` on failure. *Partially superseded in round 4 (R4-4): the ordering pin stands, but the synchronous-restart entitlement was renegotiated — a restart from inside the fan-out must be queued.* |
| R1-8 | Internal contradiction | D3 allows a parent reply to emit its replacement delta plus empty ghost-drop deltas, while the first acceptance criterion demanded "exactly one" `TabRefreshed` per accepted reply. | Resolved: criterion reworded to one primary replacement delta followed by zero or more empty reconciliation deltas, order pinned. *Superseded in round 5 (R5-2): ghost drops became one aggregate `SourcesRemoved` per parent reply.* |
| R1-9 | F46 | The spec touches `ItemsManager::OnItemsRefreshed`'s debug scan ("stays on the final snapshot") without resolving or explicitly deferring F46, whose register entry asks that M2/M3 work on this function absorb it. | Resolved in D8: M2 absorbs F46 — the scan is gated behind `spdlog::should_log` (or deleted) as part of the `OnItemsRefreshed` rework; the register's F46 entry moves to the resolved ledger when that lands. |

**Round-1 narrative.** The two heaviest findings were R1-1 and R1-4,
both the same failure of the draft: revision 1 argued safety from the
success path (the final pass, the final emit) in a design whose whole
point (D4's no-rollback) is that updates can end without one. R1-1's
nuance, recorded for fairness: partial refreshes already submit with
stale *unselected* tabs today, so posting stale data is not itself
new — the regression was that a tab the user explicitly selected could
go silently stale into a post, where today's behavior is to not post
at all. R1-2 was accepted as a real internal inconsistency but
resolved toward simplicity: the erase the reviewer flagged is the
same operation the worker itself performs per reply, so M2 legitimizes
it with a stated bound instead of building an indexed store one
milestone early. R1-5's state machine replaced a vaguer two-tier
description outright — the reviewer's version is both simpler and
closes two holes the draft's wording created.

## Round 2 — external review (July 27, 2026)

Read-only design/code review of spec revision 2 (commit `5cc6db10`),
same external reviewer. Verdict: the revision closes the round-1
findings well; two new blocking interactions, two material gaps, four
minor corrections. Freeze gate stated: R2-1/R2-2 designed, R2-3 given
a concrete measurement gate, R2-4 mapped, and the S1-M2 spike
completed.

Every code claim was re-verified before acceptance (`shop.cpp`
submission chain, the inline fail-fast completion comment in
`RunUpdate`, `GetTab`'s id-only key, the `search.cpp` scale comment).
All four findings and all four corrections were accepted; none were
modified in substance. The round's pattern, recorded for future
rounds: both blockers were contracts that held at the signal layer
but broke one layer up (the shop's asynchronous submission pipeline)
or one nesting level down (synchronous reentrancy inside terminal
fan-out).

| ID | Group | Finding | Status |
|---|---|---|---|
| R2-1 | Shop / mid-refresh state | The clean-completion gate governs when submission *starts*, but `SubmitShopToForum` is asynchronous: it fetches the legacy stash index first (`shop.cpp:175-194`) and only the continuation reads live `ItemsManager::items()` and buyouts (`shop.cpp:289`). Update N+1, legally started after N's terminal event, can stream deltas into the state N's "clean" submission then reads — new under M2, since today `items()` changes only at final snapshots. | Resolved in D8: submission input is captured as an immutable **value** snapshot at request time (shared pointers are insufficient — N+1's successful rebase mutates shared `Item`s in place); the index is applied to the capture on arrival. Manual submission during a refresh captures and submits current published state, deliberately accepted. Pinned `shopSubmissionUsesCapturedSnapshot` (staged test). |
| R2-2 | Terminal fan-out reentrancy | D4's Idle-before-terminal invites a synchronous observer to start N+1 — but `RunUpdate` launches synchronously and fail-fast futures complete inline (`itemsmanagerworker.cpp:502-508`), so N+1's signals (even its terminal event, e.g. on a setup-cooldown fail-fast) can fire nested inside N's fan-out, reaching later observers before their `RefreshFinished(N)`. Defeats the ordering-is-identity contract that justified omitting an update ID; composes with R2-1. | Resolved in D4: a delivering-terminal guard around the `RefreshFinished` emit — an `Update()` in the window is accepted-and-deferred to the next event-loop turn (the deferral-while-initializing shape); a second request in the window is refused as if an update were active. Pinned `terminalFanOutDefersReentrantUpdate`. *Superseded in round 4 (R4-4): the guard stays, the deferral became a plain refusal.* |
| R2-3 | Storage performance claim | Revision 2's "sub-millisecond pointer-chase at 100k" was asserted, not measured; the erase dereferences a heap object and compares type + `QString` per entry, M2 doubles the per-reply scans, and the code itself acknowledges users at the "hundreds of thousands or millions of items" scale (`search.cpp:243`). Worker parity establishes precedent, not a bound. | Resolved in D3: the flat-vector choice stands but now carries a **blocking implementation measurement** (M2-M2) with stated thresholds (combined erases < 2 ms at 100k, < 16 ms at 1m) on representative datasets; exceeding them makes the source-keyed fallback required, not discretionary. |
| R2-4 | First-error coverage | The missing-wrapper branches (a 200 whose parsed wrapper lacks its stash/character sub-object, `itemsmanagerworker.cpp:901/1033`) are terminal but hold no `FetchError`, so D4's first-error plumbing had undefined inputs; hook ordering and reset were also unstated. | Resolved in D5/D4 via the facade rather than synthesis: post-F62 the facade extracts the sub-object anyway (bytes capture), so an absent payload is reclassified as a facade-level `Parse` error and the worker branches are deleted — deliberately moving the case into D5's skip set. First-error storage precedes the fault hook and resets at the next accepted update. Pinned `missingStashWrapperSkipsTab` / `missingCharacterWrapperSkipsTab`. |

**Minor corrections, all adopted:** (1) revision 2's trailing D7
"renamed tab keyed by fresh metadata" transient was wrong —
`GetTab` keys on stable `location.id()` (`buyoutmanager.cpp:101`), so
per-delta inheritance is rename-proof and the paragraph is deleted;
the only real transient is the renamed-to-a-price tab awaiting the
final auto-tab pass, already described. (2) A skipped source is
"listed with stale contents", not F55's "listed-but-cold" — its
contents survive, and a successful final rebase still freshens their
embedded metadata. (3) `Application`, not `Shop`, connects to
`RefreshFinished` (the D8 criterion said otherwise). (4) The D9
throttle period must be injectable so `throttleDoesNotRearm` does not
wait wall-clock S.

## Round 3 — external review (July 27, 2026) — resolved July 28

Read-only design review of spec revision 3 (**reviewed baseline:
commit `58d33480`**), same external reviewer. Verdict: revision 3
incorporates round 2 correctly but is not ready to proceed directly
to the spike — one new blocking shop-state issue, two material
state-machine gaps, one process contradiction, one wording
correction. Every code claim was re-verified against `d995840b`
(unchanged beneath the docs-only spec branch) before acceptance:
`Shop`'s busy refusal and asynchronous index continuation, every
`m_shop_data_outdated` clear, the job-global `m_shop_data` consumption,
the worker's synchronous launch and Idle-only admission check, and the
absence of an existing benchmark contract. All four findings and the
wording correction were accepted.

| ID | Group | Finding | Status |
|---|---|---|---|
| R3-1 | Shop submission coalescing | The immutable capture (R2-1) removes an *accidental coalescing* the live read provided: `Shop` refuses a submission while one is active (`shop.cpp:146` claimed), so if clean N+1 finishes while N's capture is still submitting, N+1's auto-submit is refused and `UpdateShopData()` completing N clears `m_shop_data_outdated` (`shop.cpp:355` claimed) — N+1 is neither posted nor left pending. Related: deltas deliberately do not call `ExpireShopData`, so a forced manual mid-refresh submission can find the outdated flag false and reuse cached shop data instead of rendering its fresh capture. Reviewer proposes an explicit submission state machine: every request captures; if busy, retain one pending latest auto snapshot; a monotonic shop-input revision replaces the boolean; completing N marks only N's revision clean; drain pending after the active submission ends; manual submission renders its capture regardless of the flag; centralized completion applies one pending policy on every exit; decide whether pending drains after a *failed* submission (probably not after auth failure, while staying outdated). Staged tests: N+1-completes-during-N, latest-wins coalescing, manual-after-delta with stale flag, newer expiry not cleared by older completion. | Resolved in D8 with a simpler latest-eligible desired-state contract: one immutable active job plus one replaceable newest clean automatic capture; intermediate clean generations deliberately coalesce. Every job owns its rendered data/hash/progress and always renders its capture, so the submission path never trusts the cache flag. Monotonic revisions replace the boolean for preview/cache freshness only. One completion path drains after success/unchanged-hash, never after terminal failure; failure leaves input dirty and a later request recaptures. Manual-during-refresh always renders the captured published state. Five staged tests pin coalescing, capture, revision, and failure policy. |
| R3-2 | Terminal-deferral reservation | The R2-2 guard protects the fan-out window only; between fan-out end and the queued turn, another `Update()` can observe an Idle worker and start first, so the promised deferred update is refused, delayed, or reordered. Reviewer proposes treating the deferred update as an active reservation until its Idle→Updating transition: later requests refused while reserved; selection arguments copied at reservation time; reservation cleared immediately before starting; worker destruction before the queued turn = "never accepted". Extend `terminalFanOutDefersReentrantUpdate` with a request arriving after fan-out but before the deferred turn. | Resolved in D4 as proposed, with terminology tightened: the copied request is reserved, not accepted, until its queued Idle→Updating transition. The reservation refuses every intervening request, clears immediately before start, and a context-bound queued callback makes destruction-before-start a never-accepted request. The existing test gains the post-fan-out/pre-turn case and destruction case. *Superseded in round 4 (R4-4): the whole deferral mechanism — reservation included — was deleted by renegotiating the requirement it served.* |
| R3-3 | M2-M2 contract precision | The combined worker+manager threshold can fail in a way the named fallback cannot fix: the fallback re-indexes only `ItemsManager` storage, so if the worker's own scan alone exceeds the budget, the combined measurement still fails. Reviewer recommends measuring worker and manager separately, gating the M2 storage choice on the manager's *marginal* cost (combined reported as context), and recording a separate worker-index finding if the worker independently blows its budget; alternatively a combined threshold with a both-sides fallback, or measuring the whole synchronous delta path if the threshold means a frame budget. Also record build mode and measurement environment. | Resolved in D3/M2-M2 as proposed: worker and manager measured separately, manager marginal cost gates the manager fallback, combined cost is context, and a worker-only miss creates a separate worker-index finding. Release mode, environment, dataset/removal shape, repetitions, and statistic are recorded. |
| R3-4 | Process: spike vs. freeze | The spec says implementation begins only after freeze (working rule 1) while S1-M2 needs a working throttle prototype and is the declared pre-freeze gate — a contradiction as written. Reviewer recommends a documented exception: a dedicated non-production spike branch/harness (discarded or left unmerged), result recorded in revision 4, then freeze, then production implementation; alternatively amend the working rule. | Resolved in D9 and the parent working rule: production implementation still waits for freeze; named S1-M2 alone may use a dedicated unmerged/discarded prototype or isolated harness. Revision 4 authorizes the bounded experiment; revision 5 records its result and freezes before production work. *Renumbered as later rounds landed: the spike results and freeze land in revision 8 (rounds 4–6 became revisions 5–7).* |

**Wording correction, adopted:** D8 now says a manual request captures
the current *published* state; it does not claim that state necessarily
matches D9's deliberately lagging visible model.

**Round-3 narrative.** R3-1 was the only finding whose proposed
mechanism was materially refined. The accepted requirement is
level-triggered rather than edge-triggered: automatic shop update
converges to the newest clean eligible snapshot, and intermediate clean
snapshots need not each reach the forum. Making rendered content and
progress job-local removes the shared-state race more directly than a
revisioned boolean could; revisions remain only for cache freshness.
R3-2 preserves the synchronous chaining feature but names its
pre-acceptance state accurately. R3-3 separates the decision a
manager-only fallback can make from existing worker cost. R3-4 keeps
doc-first intact for production and makes the one evidence-gathering
exception explicit.

## Round 4 — in-repo audit (July 28, 2026)

Audit of spec revision 4 (commit `26070240`) by an in-repo Claude
session — a different reviewer than rounds 1–3's external model —
asked to check the round-3 incorporation for bugs, gaps,
inconsistencies, and over-engineering. Every code anchor cited by the
round-3 resolutions was re-verified against the working tree
(unchanged beneath the docs-only branch): the shop busy refusal and
both `m_shop_data_outdated` clears, the shared transport members, the
worker's synchronous `RunUpdate` and Idle-only admission with the
initializing deferral, `FinishUpdate`'s Idle-after-emit, and the
`Application` wiring. Verdict: round 3 is faithfully incorporated and
the four documents are mutually consistent; one real contract hole
(R4-1), one unacknowledged behavior change (R4-2), one undocumented
policy flattening (R4-3), and one simplification-by-renegotiation
(R4-4), accepted by the user. D8's expanded shop machinery was
examined for over-engineering and deliberately kept as-is: the
simpler recapture-at-drain alternative would violate the clean gate —
the drain moment can land mid-refresh of a later update — so
capture-at-eligibility plus one waiting slot is the minimum that
satisfies D8's own contract.

| ID | Group | Finding | Status |
|---|---|---|---|
| R4-1 | M2-M2 contract | Revision 4 re-targeted the original combined thresholds at each side separately, silently doubling the per-delta budget: worker 1.9 ms + manager 1.9 ms at 100k passes both gates and requires nothing, where revision 3 would have forced the fallback; the "(one frame)" rationale for 16 ms also disappeared. | Resolved in D3/M2-M2: the thresholds bind the combined cost again; separate measurement is for attribution — on a combined miss, every side whose own cost exceeds half the threshold gets its remedy (manager: required source-keyed fallback; worker: mandatory worker-index finding). Arithmetically at least one side always qualifies. *Partially superseded in round 5 (R5-4): the combined-erase frame budget became a whole-path measurement with real remedies; the manager storage gate survives.* |
| R4-2 | Shop preview freshness | The input revision advances only via `ExpireShopData()` (final snapshots, settings changes); deltas never advance it and the design-review criterion forbids wiring `Shop` to `TabRefreshed` — so mid-refresh the preview/clipboard cache reports current while published state has streamed past it, and `CopyToClipboard`'s outdated warning (`shop.cpp:627`) is silently wrong in a window that is new under M2. Submission correctness was already protected (every job renders its capture). | Resolved in D8 by stating the accepted cost: the clipboard serves the same last-rendered text it serves today; the outdated warning is snapshot-granular, not delta-granular. No delta wiring added. |
| R4-3 | Shop failure policy | The round-3 resolution flattened the reviewer's kind hint ("probably not after auth failure") into no-drain-on-any-terminal-failure without recording why that is safe. | Resolved in D8 with the parity rationale stated: a failed submission has never retriggered before the next refresh; kind-aware draining belongs to the future design that owns per-tab retry (D11). |
| R4-4 | Terminal-restart requirement | R2-2/R3-2 form a patch-on-a-patch — guard the fan-out, then guard the gap after the guard, then define the destruction case — all serving R1-7's promise that a synchronous observer may start the next update from inside the terminal event. Code audit shows no production caller consumes that promise: every `Update()` originates from a user action or the auto-refresh timer (`itemsmanager.cpp:29`), both of which arrive via the event loop and cannot land inside the synchronous fan-out. | Resolved in D4 by renegotiating the requirement (user-approved): an `Update()` during terminal fan-out is refused exactly as if an update were active; observers that chain restarts queue them. The delivering-terminal flag stays; the reservation, queued start, gap guard, and destruction case are deleted. Supersedes the R2-2 and R3-2 resolutions and revises R1-7's (the Idle-before-terminal ordering pin is kept for state-query consistency). Pin renamed `terminalFanOutRefusesReentrantUpdate`. |

**Correction to the round-3 record:** R3-1's recorded mechanism
("`UpdateShopData()` completing N clears `m_shop_data_outdated`")
holds only when N's stash index arrives after N+1's refresh
completes; in the common interleaving N renders (clearing the flag)
before N+1's expiry re-sets it, so the flag survives set and N+1 is
lost because nothing *retriggers* a submission, not because the flag
is cleared. The finding's conclusion — N+1 neither posted nor
pending — stands in both interleavings; the resolution is unaffected.

**Round-4 narrative.** The round's theme was second-order review:
checking not the design but the previous round's incorporation of it.
R4-1 was the substantive catch — a reviewer-recommended refinement
(separate measurement) that silently loosened the contract it was
refining. R4-4 went the opposite way: two rounds of individually
correct fixes to a requirement nothing needed, resolved by deleting
the requirement. The pairing is the round's lesson: when a fix chain
grows at one spot, check whether the requirement at its root is real
before extending the chain.

## Round 5 — external review (July 28, 2026)

Read-only external review of spec revision 5 (commit `1dfb53e7`),
delivered by the user and evaluated claim-by-claim in-repo before
acceptance. Verdict: direction sound, revision 5 not freezable —
four blocking findings, three material contract holes. Code claims
verified: `Search` buckets by `std::map<ItemLocation, Bucket>`
(`search.cpp:239`); `ItemLocation::operator<` orders stash locations
by the positional `m_tab_id` while `operator==` compares the stable
unique id (`itemlocation.cpp:155`, `itemlocation.h:47`); the
worker's ghost reconcile is one collection scan
(`itemsmanagerworker.cpp:1008`). Verification *extended* R5-1:
beyond the reviewer's split and stale-header cases, a positional
collision can merge two different tabs' items into one bucket —
affecting regular tabs, not just Map/Unique parents. All seven
findings were accepted (R5-4 with a reframing); the review's
D10-removal suggestion was declined — D10 is ten measurement-gated
lines consuming a traceability input, and moving it to the register
is churn without simplification. User decisions recorded:
keep-and-drain for R5-6's second case, M2-M2 moved pre-freeze, and
the stable-identity bucket rule for R5-1.

| ID | Group | Finding | Status |
|---|---|---|---|
| R5-1 | Mid-refresh metadata rendering | D6 claimed M2 renders nothing juxtaposing one tab's old and new metadata, but `Search` buckets by `ItemLocation`, whose stash *ordering* is positional while *identity* is the stable id: a moved Map/Unique parent splits into two buckets, a renamed tab files fresh items under a stale header, and (found in verification) a positional collision merges two different tabs into one bucket. Contradicts D6 and the parent plan's fourth-constraint requirement. | Resolved in D6 (new bullet): bucket identity is the stable `(type, id)` and each bucket renders the freshest metadata seen for that key; mechanism left to implementation; pinned `bucketsKeyOnStableIdDuringRefresh` covering moved/renamed/collision. D2's data-generation mixing pin is unchanged — it governs contents, D6's rule governs rendering. *Extended in round 6 (R6-1): the inventory ingests empty-delta anchors; metadata carried only by empty deltas lands at the next refilter.* |
| R5-2 | Ghost-child fan-out | Per-dropped-id empty deltas turn the worker's single reconcile scan into k full-vector manager erases plus k intersection tests — an O(k × all items) synchronous burst invisible to a per-delta benchmark — and are unimplementable for zero-item children (the worker keeps no prior child-id inventory). | Resolved in D3: one aggregate `SourcesRemoved(parent, removed_keys)` per parent reply, carrying the distinct keys the reconcile actually erased (sources with published items only); the manager applies one set-lookup erase pass — exact worker parity. Supersedes R1-8's wording; criteria updated (`ghostChildDropStreamsAsAggregateRemoval`). *Superseded in round 6 (R6-2): the payload became the authoritative expected-child set — erased-keys could miss published ghosts after a failed update.* |
| R5-3 | D9 fallback vs. freshness bound | Outcome (b) leaves the visible model stale unboundedly after deltas end in terminal failure (no final emit, no timer), violating the parent's "freshness bound, not just coalescing" input, while the UI acceptance criteria unconditionally require throttle behavior. | Resolved in D9: outcome (b) is named a renegotiation of the parent input, updating the parent plan in the same commit if chosen; timer-dependent criteria are marked outcome-(a) and replaced by affordance criteria in revision 7 under (b). Under (a) the bound holds across failure — a pending tick survives terminal failure and fires (`pendingTickSurvivesTerminalFailure`). |
| R5-4 | M2-M2 scope and teeth | The erase-only benchmark cannot justify a frame budget (the synchronous path also includes persistence, pricing, intersection, fan-out), and revision 5's worker "remedy" was a paper-trail finding while the threshold was called binding. Reviewer recommended a whole-path measurement, run pre-freeze. | Resolved in D3/open items by separating the two conflated questions: the manager's marginal erase cost gates the manager storage choice; the complete synchronous reply application carries the frame budget with per-component attribution and **real** remedies (manager map, symmetric worker source-keyed store, or a named fix for the dominant component); half-threshold arithmetic dropped. M2-M2 moves pre-freeze as the second named evidence spike beside S1-M2 (shared harness/datasets); results in revision 7. *Amended in round 6 (R6-6): runs post-F62, fixed recorded shapes, remedy validated by rerun; results land in revision 8 after renumbering.* |
| R5-5 | D9 refilter/timer hole | Rule 3 leaves a pending tick alive after a user-initiated or form-edit refilter already paid for the work — a redundant later reset. | Resolved in D9 rule 3: any successful refilter of the current search clears its flag and cancels its pending timer; pinned `successfulRefilterCancelsPendingTick`. |
| R5-6 | Waiting-capture transitions | D8 leaves two transitions unpinned: disabling auto-update while a capture waits, and a skipped/failed refresh completing before an older clean capture drains. | Resolved in D8: disabling auto-update drops the waiting capture (active job unaffected); a later skipped/failed refresh does not invalidate the waiting clean capture — **keep-and-drain** (user decision): the gate's invariant is cleanliness, not recency, and posting the last clean state later is parity with today. Pinned `disablingAutoUpdateDropsWaitingCapture` / `skippedRefreshDoesNotInvalidateWaitingCapture`. |
| R5-7 | Criterion/decision mismatch | `scopedPricingIsFailSafeAcrossFailedUpdate` demands every refresh-requiring item's tab be locked while D7 rule 3 deliberately excludes remove-only tabs — the test as written fails a correct implementation. | Resolved: the criterion carries the same exclusion. |

**Round-5 narrative.** The heaviest findings attack the two places M2
touches scale — rendering (R5-1) and reconciliation fan-out (R5-2) —
both contracts that were correct per-delta and wrong in aggregate.
R5-1 also demonstrated that verification can strengthen a finding:
the positional-collision merge, arguably the worst of the three
artifacts, surfaced while confirming the reviewer's mechanics. R5-4
completes the M2-M2 arc (R2-3 → R3-3 → R4-1 → R5-4) by splitting the
question that kept the thresholds unstable: what gates the storage
choice (manager marginal cost) versus what a frame budget must
measure (the whole synchronous path). Moving it pre-freeze accepts
that a frozen spec should not leave its central storage decision
open.

## Round 6 — external review (July 28, 2026)

Read-only external review of spec revision 6 (commit `0781d7fe`),
delivered by the user and evaluated claim-by-claim in-repo before
acceptance. Verdict: four blocking findings, two material gaps. All
six code-evidence sets verified: empty buckets are created only from
the published tab list and only when unfiltered
(`search.cpp:280-286`); the worker's list reconciliation erases
deleted tabs' items at list arrival (`itemsmanagerworker.cpp:
781-791`), so worker and published state diverge across a failed
update; the restore machinery's four defects all confirmed —
header-text expansion keys (`mainwindow.cpp:709`), no
save-before-reset on the refresh path (`mainwindow.cpp:736-774`),
`shared_ptr`-identity reselection (`search.cpp:181`), no scroll
capture; refresh selection is a bare-id set
(`itemsmanagerworker.cpp:397`). Dispositions: R6-2/R6-4/R6-6 adopted
as proposed (R6-2 upgraded to the reviewer's strongest offered
shape), R6-1 adopted lean with its intersection-class remedy
declined, R6-5 contained to a register finding, R6-3 adopted per the
user's decision to fund restore fidelity for outcome (a).

| ID | Group | Finding | Status |
|---|---|---|---|
| R6-1 | Empty-delta metadata | D6 claimed a delta's bucket appears with its fresh location, but empty buckets come only from the stale published tab list, and D9 has no metadata-only intersection trigger — a new, renamed, or moved *empty* tab renders stale or not at all mid-refresh. | Resolved lean in D6: the canonical inventory (R5-1) ingests every delta anchor, empty deltas included, and the empty-bucket source list resolves through it; the over-claim is corrected — metadata carried only by empty deltas becomes visible at the next refilter or final snapshot, an accepted cost (same shape as background captions). The reviewer's new intersection class was declined as heavier than the artifact. Pinned `emptyDeltaMetadataLandsAtNextRefilter`. |
| R6-2 | Reconciliation baseline | The round-5 removal signal named keys the worker erased — but worker and published state diverge across a failed update (list deletions mutate worker/datastore at list arrival, snapshot-boundary downstream), so a published ghost the worker no longer holds can never be named: two failed updates leave it visible indefinitely. The R5-2 criterion overpromised. | Resolved in D3 by the reviewer's strongest shape: the signal became `ChildrenReconciled(parent, expected)` carrying the authoritative expected set (parent + listed children); every consumer applies the worker's own predicate to its own baseline — one pass, worker parity, correct against any divergence, zero-item case subsumed. Supersedes R5-2's payload. Pinned `parentReplyReconcilesChildrenAgainstExpectedSet` and the two-update `reconcileErasesGhostsAcrossFailedUpdates`. |
| R6-3 | Restore fidelity | D9 delegated scroll/selection/expansion survival to machinery that verifiably cannot deliver it: expansion keyed by mutable header text, no save-before-reset on the refresh path, reselection by `shared_ptr` identity (lost for every replaced item — exactly what M2 streams), no scroll capture. S1-M2 would judge cadence through the machinery's failures, and the parent plan's survival requirement would go unmet. | Resolved in D9 (user decision: fund fidelity rather than pre-commit to outcome (b)): outcome (a) includes a fidelity contract — stable `(type, id)` expansion keys, stable-identity reselection, scroll preservation, capture-immediately-before-every-reset — prototyped in S1-M2 so the spike judges cadence with fidelity in place, productionized only under (a). Under (b), today's machinery and losses stand. The stable keying carries into M3 either way. Three outcome-(a) pins added. |
| R6-4 | Shop captures vs. local edits | Nothing said what a buyout/template edit does to a waiting automatic capture: draining a pre-edit capture posts data older than the user's intent while reporting success — a regression vs. today's live render, which picks edits up. | Resolved in D8: `ExpireShopData()` while an automatic capture waits drops the capture; the edit reaches the forum via the next clean capture or manual submission, as today. The active job stays immutable. Pinned `expireDropsWaitingCapture` / `activeJobUnaffectedByLocalEdits`. |
| R6-5 | Cross-type key propagation | `FetchSourceKey` exists because cross-type id collision is possible, yet refresh selection, locks, tab buyouts, and `operator==` stay bare-id — one collision conflates two locations across types. | Contained (user-approved): the typed key governs exactly the predicates M2 introduces; the legacy bare-id exposure is documented as a boundary in D3 and recorded as **F66** (registered as F64 at the time; renumbered at the July 28 master merge — released master history had taken F64/F65) in the findings register (pre-existing surface, migration out of proportion to the risk, M3 the natural rework point). Not expanded into M2. |
| R6-6 | M2-M2 sequencing and evidence | Round 5 moved M2-M2 pre-freeze while D1 only ordered F62 before implementation — the benchmark could legally run pre-F62 and measure an obsolete persistence payload; shapes were not fixed for comparability, and a selected remedy was never validated. | Resolved in D1/D3/open items: F62 precedes the M2-M2 measurement, not just M2 code (sequence: F62 → spikes → freeze); datasets and reply/removal shapes are fixed and recorded; a selected remedy is validated by a rerun before freeze — naming a remedy is not evidence. |

**Round-6 narrative.** The round's center of gravity shifted from
the spec to the legacy code the spec streams into: R6-3 and R6-5 are
defects of existing machinery that M2's mid-refresh visibility
exposes, not of the design — R6-3 being the round's real finding,
since it showed S1-M2 would have evaluated a strawman. R6-2 closed
the loop the R5-2 fix opened: sending what the worker erased assumed
worker and published state agree, and the no-rollback design is
precisely a design in which they may not; sending the authoritative
expected set makes the consumer's baseline irrelevant. R6-6 caught a
sequencing hole created by round 5's own improvement — the same
lesson as R4-1, in the other direction: every relocation of a gate
re-opens the question of what must precede it. After this round the
review series is capped by decision: remaining risk is retired by
the two evidence spikes, not further argument.

## Round 7 — external review (July 28, 2026) — closing round

Read-only external review of spec revision 7 (commit `f21c9757`),
delivered by the user with an explicit instruction to guard against
over-reviewing and design-time scope creep, and evaluated
claim-by-claim in-repo before acceptance. Reviewer's verdict,
accepted: the spec is structurally sound; no further broad review
rounds; a short targeted amendment plus evidence, then freeze. All
four findings verified against the spec and code (including
`search.cpp:280-286` for the filtered empty-bucket behavior and
`shop.cpp:304` for shop-side sorting) and adopted. The reviewer also
enumerated seven areas that should wait for implementation
(canonical-map ownership, reselection key choice, scroll mechanism,
timer injection, shop job structures, benchmark instrumentation
boundaries, post-F62 line drift) — accepted as-is; no design-review
time is spent on them.

| ID | Group | Finding | Status |
|---|---|---|---|
| R7-1 | Outcome (b) underspecified | The spike claimed to choose between "two already-specified behaviors," but outcome (b)'s affordance was only an example and its acceptance criteria were deferred to the freeze revision — selecting (b) would invent and freeze a behavior in one step. | Resolved in D9 by defining the minimal affordance contract now (cheaper than the reviewer's alternative of a conditional extra review round): per-search intersected-tab sets beside the dirty flags, synchronous count updates, per-search clear on successful refilter, and visible persistence across terminal failure. Three outcome-(b) pins added (`staleAffordanceCountsIntersectingTabs`, `staleAffordanceClearsPerSearch`, `staleAffordanceSurvivesTerminalFailure`). Presentation stays implementation detail. |
| R7-2 | Freshness-bound contradiction | D6 deliberately gives metadata-only empty deltas no intersection trigger, yet D9 claimed the view is never more than S behind applied state, extended across terminal failure — an empty-tab rename followed by failure starts no timer and stays invisible until user action, contradicting the stated bound. | Resolved as a documented narrow exception in D9 rule 2 (per the reviewer's own anti-scope-creep recommendation; a metadata intersection class was already declined in R6-1): the bound covers item-bearing deltas; metadata-only empty-delta changes land at next refilter or final snapshot and may wait indefinitely after failure. The outcome-(a) no-gap claim is qualified accordingly, and `emptyDeltaMetadataLandsAtNextRefilter` now specifies an unfiltered search (filtered searches hide empty buckets by design, `search.cpp:277-286`). |
| R7-3 | M2-M2 timing | The pre-freeze M2-M2 gate had drifted into a throwaway production vertical slice: it required the complete post-F62 reply path plus remedy implementation and validation rerun before implementation officially begins — implementing before freeze in all but name, against the doc-first rule; its unpinned statistic and dataset shapes also weakened it as a binding gate. | Resolved by relocation (reversing R5-4's placement): D3 freezes a determinate conditional — flat vector, measured budgets, mandatory named remedy on a miss — satisfying R5-4's "nothing left open" rule without a pre-freeze run, and M2-M2 runs as the first checkpoint of production implementation on the real vertical path, with remedy validation by rerun before M2 is complete. The statistic/fixture looseness dies with the pre-freeze placement. S1-M2 keeps its pre-freeze exception (genuine product decision) and additionally records reset latency on a realistically large collection. |
| R7-4 | D6/D8 consumer note | D6 said "shop reads final-only per D8," but D8 deliberately lets manual submission capture mid-refresh published state — the sentence was inaccurate (though the behavior is safe). | Resolved in D6: the note now states the actual contract — the manual job renders from its own value capture and sorts during generation (`shop.cpp:304`), so unsorted mid-refresh published order never reaches the forum post. |

**Round-7 narrative.** A closing round in both senses: every finding
was a correction to the spec's own bookkeeping rather than to a
contract — the one relocation (R7-3) undoes a round-5 placement
whose rationale D3's conditional decision had already absorbed, and
resolves the quiet contradiction that the pre-freeze miss branch
would itself have violated doc-first. The round's instruction to
resist over-review shaped its resolutions: R7-2 became a documented
exception instead of machinery, R7-1 a four-sentence contract
instead of a future review round. Review is closed by decision;
what remains is F62, the S1-M2 spike, and revision 9's freeze.

## Revision log

- **Revision 1** (July 27, 2026, commit `aaa70f1e`): initial draft —
  staleness preamble against `d995840b`, D1–D11, acceptance criteria,
  input-traceability table covering the full M2 inbox from
  `items-pipeline.md`.
- **Revision 2** (July 27, 2026): round-1 incorporation — R1-1 through
  R1-9 resolved as tabled above. Substantive changes: forum submission
  re-gated on clean completion (D8), `FetchSourceKey` unifying both
  erase predicates (D3), the flat-vector linear erase legitimized with
  the worker-parity bound (D3), D7's scoped pricing rewritten to be
  fail-safe across a failed update (monotone locks, tab-name pricing
  final-only), D9 rewritten as an explicit five-rule state machine
  with automated acceptance tests, `RefreshOutcome` as a sum type with
  first-error preservation and pinned Idle-before-terminal ordering,
  and F46 absorbed. The shaping decisions D1/D2 are unchanged. This
  file split out in the same commit.
- **Revision 3** (July 27, 2026): round-2 incorporation — R2-1 through
  R2-4 resolved as tabled above, plus the four minor corrections.
  Substantive changes: shop submission input captured by value at
  request time with the manual-submission policy stated (D8),
  terminal fan-out reentrancy guard with accept-and-defer semantics
  (D4), the linear-erase choice gated by the blocking M2-M2
  measurement with a mandatory fallback (D3), missing-wrapper
  payloads reclassified as facade `Parse` errors joining the skip set
  with first-error hook ordering and reset stated (D5/D4), and the
  incorrect renamed-tab transient deleted from D7.
- **Revision 4** (July 28, 2026): round-3 incorporation — R3-1 through
  R3-4 and the wording correction resolved as tabled above.
  Substantive changes: shop auto-update becomes a latest-clean
  desired-state contract with one immutable active job and one
  replaceable eligible capture, job-local rendering/progress,
  revisioned preview-cache freshness, and centralized completion
  policy (D8); terminal deferral gains a reservation that survives
  fan-out until the queued acceptance transition (D4); M2-M2 measures
  worker and manager separately and gates only the manager's marginal
  cost (D3); and S1-M2 receives a narrow non-production pre-freeze
  exception, with its result and freeze deferred to revision 5 (D9).
- **Revision 5** (July 28, 2026): round-4 incorporation — R4-1
  through R4-4 and the round-3 record correction resolved as tabled
  above. Substantive changes: M2-M2's thresholds bind the combined
  erase cost again, with per-side attribution remedies (D3); the
  terminal-deferral reservation is deleted — an `Update()` during
  terminal fan-out is refused and chained restarts queue (D4,
  superseding the R2-2/R3-2 machinery and revising R1-7); and D8
  states the accepted preview-cache blind spot and the
  failure-policy parity rationale. D8's shop machinery was audited
  for over-engineering and deliberately kept. The S1-M2 spike result
  and freeze move to revision 6.
- **Revision 6** (July 28, 2026): round-5 incorporation — R5-1
  through R5-7 resolved as tabled above. Substantive changes:
  stable-identity bucket rendering with a canonical-metadata rule
  (D6); ghost drops as one aggregate `SourcesRemoved` per parent
  reply (D3, superseding R1-8's empty-delta wording); outcome (b)
  named a parent-input renegotiation, with timer criteria
  conditional on outcome (a) (D9); M2-M2 reframed — manager marginal
  cost gates storage, the whole synchronous reply application
  carries the frame budget with real remedies — and moved pre-freeze
  as the second evidence spike (D3); any successful refilter cancels
  the pending tick (D9); waiting-capture transitions pinned with
  keep-and-drain (D8); the remove-only exclusion carried into the
  pricing criterion. Pre-freeze gates are now S1-M2 and M2-M2;
  revision 7 records both results and freezes.
- **Revision 7** (July 28, 2026): round-6 incorporation — R6-1
  through R6-6 resolved as tabled above. Substantive changes: the
  child reconciliation carries the authoritative expected set
  (`ChildrenReconciled`, D3, superseding R5-2's erased-keys
  payload); D9 gains the outcome-(a) restore-fidelity contract
  (stable expansion/selection keys, scroll preservation,
  capture-before-reset), prototyped in S1-M2; the canonical
  inventory ingests empty-delta anchors with next-refilter
  visibility accepted (D6); `ExpireShopData` drops a waiting
  automatic capture (D8); the legacy bare-id keying boundary is
  documented and registered as F66 (D3, renumbered from F64 at the
  July 28 master merge); and M2-M2 runs post-F62
  with fixed shapes and a remedy-validation rerun (D1/D3). Six new
  or renamed pins. The pre-freeze sequence is F62 → S1-M2 + M2-M2 →
  revision 8 records results and freezes. External review is capped
  by decision; remaining risk retires through the spikes.
- **Revision 8** (July 28, 2026): round-7 incorporation — R7-1
  through R7-4 resolved as tabled above; the closing round.
  Substantive changes: outcome (b)'s affordance contract defined
  with three outcome-(b) pins (D9); the metadata-only empty-delta
  exception carved into D9's freshness bound, with the outcome-(a)
  claim qualified and the empty-delta pin scoped to unfiltered
  searches; M2-M2 moved out of pre-freeze to the first
  implementation checkpoint, with D3 freezing a determinate
  conditional and remedy validation moved to "before M2 is
  complete" (parent plan's working rule 1 updated to one named
  exception); S1-M2 gains the realistic-collection and
  reset-latency requirements; D6's shop consumer note corrected.
  The pre-freeze sequence is F62 → S1-M2 → revision 9 records the
  result and freezes. The review series is closed.
- **Revision 9** (July 29, 2026): **the freeze.** Not a review round
  — records the pre-freeze evidence and changes no decisions. F62
  merged to master (PR #183, `fdf530b5`); the S1-M2 spike ran
  July 29 on the non-production branch `spike/s1-m2-throttle` with
  the R6-3 fidelity contract prototyped and Tom hand-judging cadence
  at the driving scale: **outcome (a) selected, S confirmed at
  60 s**. Reset latency recorded (~455 ms at 101k items, ~5.4 s at
  ~976k, the post-reset whole-model re-sort dominant in both; the
  fidelity machinery ~0 ms) — evidence archived in
  `s1-m2-spike-result.md` (retired to git history August 1, 2026;
  the spec's D9 records the result). The outcome-(b) affordance contract
  (R7-1) is retained as record, not selected; the outcome-(a)
  acceptance criteria bind. The ~1m sort cost is carried into the
  parent plan as an input for the M3 spec. Production implementation
  may begin; its first checkpoint is M2-M2 (R7-3).
- **Revision 9 amendments** (July 29, 2026): post-freeze external
  review **of the spike evidence** (not a reopened design round —
  six findings, none changing a decision, all accepted). (1) The
  spike's scroll-anchor fallback was dead when the anchored item was
  churned away — fixed on the spike branch; hands-on judgment
  predated the fix, so the felt result only improves. (2) The
  global-S-vs-1m tension resolved by Tom as an explicit supported
  envelope at the driving scale, the accurate
  S-plus-one-reset-duration freshness bound (D9 rule 2 and
  `pendingTickSurvivesTerminalFailure` corrected), and explicit
  acceptance of the ~1m recurring reset cost for M2 (M3 retires it).
  (3) The result document's "mechanically pinned" overclaim
  corrected; two automated fidelity scenarios added on the spike
  branch (expansion survives rename of the expanded tab; selection
  survives a churn tick). (4) The spike's bucket-scoped reselection
  recorded as a prototype narrowing; R6-3's contract sharpened to a
  global identity lookup and the `reselectionSurvivesCrossTabMove`
  pin added. (5) Latency attribution restated as a collective bound
  for the fidelity work; the comparator claim downgraded to
  suspected-unprofiled here and in the parent plan. (6) Stale
  references corrected (`docs/README.md`, the parent plan's M2
  heading, the 1m preset description). The scroll-fallback fix and
  new scenarios also gained an explicit fallback clause on
  `scrollAndCaptureSurviveThrottledReset`. A residual pass the same
  day fixed three follow-ups: the spec's broad validation claims
  scoped to the exercised intra-tab cases (cross-tab reselection
  stays a production obligation); the parent plan's fidelity-cost
  line reworded to the collective bound; and the selection scenario
  strengthened into `selectionFollowsStableIdWhenRowMoves` (forced
  row movement, GGG-id and replacement-object assertions), which a
  same-row or same-display-name reselection cannot pass.
