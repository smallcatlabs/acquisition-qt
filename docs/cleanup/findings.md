# Findings Register

This is the living register of design and correctness problems, begun during
the July 2026 code investigation that motivated the (now completed) design
cleanup and carried forward by the items-pipeline redesign
(`docs/design/items-pipeline.md`). Findings are anchored to symbols rather
than line numbers so they survive drift.

Rules of the register:

- **F-numbers are permanent and never reused.** Code comments, commit
  messages, and design docs cite them. New findings continue from the
  highest number ever assigned, even though most earlier numbers now live
  only in the resolved ledger.
- Resolved findings are compacted to one ledger line below. Their full
  prose — mechanisms, decisions, amendments — is in git history
  (`git log -p -- docs/cleanup/findings.md`, before July 2026's register
  trim).
- Confidence levels: **Confirmed** means the defective code path was
  verified by reading it end-to-end; **Likely** means strong evidence with
  a gap needing a runtime check.

---

## Open findings

### F22. Dual persistence paths in `BuyoutManager` — Confirmed; won't fix unless touched

Buyouts persist through `BuyoutRepo` (signal-driven, newer) while
`refresh_checked_state` persists through `DataStore` JSON serialization
(older). Works, but the split is a trap for contributors. Decision (Phase 6
spec upgrade, July 2026): nothing forces storage changes here, so
unification was dropped; a comment at the `m_refresh_checked` declaration
documents the split. Unify only if other work touches this storage anyway.

### F54. The v4→v5 buyout migration never persists under the repo-backed store — Confirmed (mechanism); reachability unverified

Found July 17, 2026, while validating the F52 fix's map-mirrors-repo
assumption. Pre-existing on master; unrelated to PRs #162/#163.

`BuyoutManager::MigrateItem` moves `m_buyouts` entries from legacy hash
keys to item-id keys **in memory only**. The `m_buyout_manager.Save()`
call that follows the migration loop in `ItemsManager::MigrateBuyouts`
persists nothing but `refresh_checked_state` (the F22 split), and no
path writes the migrated rows to `BuyoutRepo` or deletes the old-keyed
ones. So a store that reaches `MigrateBuyouts` at `db_version <= 4`
migrates in memory, works for one session, then loses those buyouts at
the next restart: `Load()` refills the map with hash-keyed rows that
id-based `Get()` never reads, and the `db_version == 5` guard prevents
re-migration. The buyout data still exists in the repo under keys
nothing consults — silent, permanent-looking loss from the user's view.

Reachability: dormant for any install already at `db_version` 5. The
plausible live path was the legacy importer (`LegacyDataStore` reads
`db_version` from the old database) — but `LegacyDataStore` has no
callers outside `src/legacy/` (verified July 17 during the F55 work):
the importer is not wired into the application at all, so that path
cannot arm the migration today. Whether any other path can put a
`db_version < 5` into a live store remains unverified; check before
sizing the fix.

Fix shape: make the migration write through the repo (save the id-keyed
row, delete the hash-keyed one) inside `MigrateItem`, or have
`MigrateBuyouts` flush the affected entries after the loop. Related: F22
(dual persistence), F51 ledger entry (do not rekey `GetLegacyHash` — a
correct future v4→v5 migration depends on it).

Update (August 14, 2026): the `legacy-buyout-import` branch wires
`LegacyDataStore` into the application via `LegacyBuyoutImporter`
(menu-driven, writes id-keyed rows directly through `BuyoutRepo`), so
the "no callers outside `src/legacy/`" reachability note above is stale
on that branch. The importer never lowers `db_version`, so it still
cannot arm this migration; instead it supersedes it — once the importer
proves out in a release, the plan of record
(`design/legacy-buyout-import.md`, deferred list) is to delete
`MigrateBuyouts`/`hash_v4`/`old_hash`/`GetLegacyHash`, resolving this
finding by removal and retiring the F51 constraint.

### F63. `Character::guardian` and `skills` are modeled but never ingested — Confirmed; deferred by decision

Found July 27, 2026, during the 3.29 documentation reconciliation
(external review). `poe::Character` models six item containers; both
ingestion sites enumerate only four — `equipment`, `inventory`,
`rucksack`, `jewels` (`itemsmanagerworker.cpp`: the cached-parse
collection list and the live `OnCharacterReceived` list). When present,
`guardian` (PoE1 only) and `skills` (PoE2 only) are accepted by the
typed parser and — being modeled — persisted in `json_data` (even the
pre-F62 lossy re-serialization did not drop them), but items in them never
become visible `Items`, in memory or in any search.

Decision (Tom, July 27, 2026): deliberate deferral, not a release
blocker. `guardian` is new in 3.29 and exposes a previously opaque
inventory — the items equipped by a player's animate-guardian minion.
Seeing them would be useful, but those items are unsellable once the
guardian has equipped them, so there is no urgency; not 0.18 material.
`skills` stays deferred with PoE2 character ingestion generally.

For whoever implements this later: the unsellable nature is a design
input, not just trivia — guardian items must stay out of the pricing
and shop surfaces (auto-buyouts, tab-buyout propagation, forum shop
submission), so the fix is adding the container to both collection
lists *plus* a location/category decision for display *plus* an
exclusion from trade features. Cached characters whose payloads
contain either collection can backfill without a refetch because the
containers are already persisted.

### F66. Legacy stores key locations by bare id, ignoring the location type — Confirmed; contained by decision

(Originally registered as F64 on the M2 spec branch; renumbered at
the July 28 master merge — master's released alpha.3 history had
independently assigned F64 to the userstore schema repair and F65 to
the rate-limit policy-shape fix.)

Found July 28, 2026, during M2 spec review round 6 (R6-5,
`items-pipeline-m2-reviews.md`). Several long-standing stores key
stash/character locations by `ItemLocation::id()` alone, with no type
qualifier: the worker's refresh selection set
(`m_tabs_to_update.emplace(tab.id())`, `itemsmanagerworker.cpp:397`),
refresh locks and tab buyouts (`BuyoutManager`, keyed on
`loc.id()`), and `ItemLocation::operator==` itself
(`itemlocation.cpp:173`, `m_unique_id` only). A cross-type id
collision — a character whose identifying id equals a stash tab's
uid — would conflate the two: selecting one location could fetch
both types, and lock/buyout state would be shared between them.

No collision has ever been observed; stash uids are long
GUID-derived hex strings and character ids are player-chosen names,
so overlap is astronomically unlikely — but it is not provable,
since both namespaces are GGG-controlled.

Decision (Tom, July 28, 2026): contained, not fixed in M2. M2
type-qualifies the keys it *introduces* (`FetchSourceKey{type,
fetch_id}` for the replacement erases and the child reconciliation,
so the worker and the published copy can never diverge on a
collision), and documents this boundary in D3. Rekeying the legacy
stores is a migration project (persisted buyout keys included) out
of proportion to the risk; this finding is the hook if a collision
ever materializes or the stores are otherwise reworked. (M3 was
named the natural opportunity but completed July 31, 2026 without
reworking the stores — its spec's D7 records the deliberate
non-exercise; the hook stands.)

### F74. The POESESSID cookie is not host-scoped — Likely

Found August 8, 2026, during the credential-custody investigation
(`docs/redesign/topics/credential-custody.md`, `redesign` branch).
`NetworkManager::setPoeSessionId` installs the cookie domain-wide on
`.pathofexile.com` (`POE_COOKIE_DOMAIN`), and standard cookie
domain-matching sends it to every `*.pathofexile.com` host — the
intended consumers on `www.` (legacy stash index, forum), but also
`api.pathofexile.com` and the OAuth token endpoint. The assumed
guarantee "cookie never sent to `api.`" does not exist today — the
secret reaches hosts that never need it, and the API evidently
tolerates it. Inferred from the cookie domain plus
`QNetworkCookieJar`'s suffix matching; not verified with a packet
capture — hence Likely. Fix shape: scope the cookie to
`www.pathofexile.com` — both real consumers are on `www.`, so nothing
should break, but verify the legacy index still authenticates.

### F75. User-Agent does not follow GGG's documented format — Confirmed

Found August 8, 2026, during the same investigation. GGG's developer
docs require the prefix format
`User-Agent: OAuth {clientId}/{version} (contact: {contact})`; the app
sends `acquisition/<version> (contact: …)` without the `OAuth ` prefix
(the `USER_AGENT` constant in `networkmanager.cpp`, built from
`APP_NAME`/`APP_VERSION_STRING`/`APP_PUBLISHER_EMAIL`). Tolerated in
practice, but it is the one documented API-citizenship rule the client
visibly breaks — worth weighing given the project's history. Fix
shape: add the `OAuth ` prefix.

### F77. No OAuth de-arming: refresh failure and bearer rejection are log-only — Confirmed

Found August 8, 2026, during the same investigation.
`OAuthManager`'s failure signals land in slots that only write log
lines (`onRequestFailure`, `onServerError`): no de-arming, no
credential clearing, no UI event. Mid-session, an expired or revoked
bearer surfaces as per-request 401 `FetchError`s — no OAuth-side
401 classification exists anywhere (grep-verified; the only 401/403
handling is the shop's POESESSID path). This is asymmetric with the
POESESSID guard, which de-arms automation, clears the credential, and
notifies the user (shop-write-path §2, commit 8b761e2c). After login
nothing listens to OAuth state at all — `grantAccess` is consumed
only by `LoginDialog`. Fix shape: surface refresh failure to the UI
and add an `oauth_rejected` de-arming path symmetric with the
POESESSID one.

### F78. `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` was defined for only one target — Resolved; build and installer fixes verified on Windows

Found August 22, 2026, from a Sentry crash on 0.18.3
(`acquisition.sentry.io/issues/7687078638`, two events, one Windows 10
19044 machine). `EXCEPTION_ACCESS_VIOLATION_READ / 0x0` inside
`mtx_do_lock` (MSVCP140.dll), reached from `main` ->
`logging::init` (`src/util/logging.cpp:78`) ->
`spdlog::set_default_logger` -> `registry::set_default_logger`
(`registry-inl.h`) -> `std::lock_guard`. The process died on its very
first `std::mutex` lock, before `logging::init` returned.

Mechanism: MSVC 14.38 (VS 2022 17.8) made `std::mutex`'s constructor
`constexpr`, so the mutex's storage is constant-initialized to zero
instead of being set up by `_Mtx_init_in_situ`. An older
`msvcp140.dll` does not understand that representation and its
`mtx_do_lock` dereferences a null handle. The crash event's debug
images confirm it: `acquisition.exe` built 2026-08-20 against a
`C:\Windows\SYSTEM32\MSVCP140.dll` built 2021-02-11 (~14.28, the
VS 2019 redistributable). The bundled `vc_redist.x64.exe` had never
run or had failed; `installer.iss` makes it an optional Task.

`_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` was already in the build, but
as a `PRIVATE` `target_compile_options` entry on `acquisition_core`
alone. `src/main.cpp` (the `acquisition` target), `acquisition_filters`,
every test target, and all fetched dependencies compiled without it.
`std::mutex`'s constructor is inline, so a mutex inside an inline
function or a header-only library — spdlog's `registry` singleton is
exactly that — is emitted as a COMDAT by every translation unit that
uses it. Mixing macro and non-macro TUs makes those definitions
disagree and the linker keeps one arbitrarily: an ODR violation whose
observable symptom is this crash. Partial coverage was therefore worse
than none, because it read as protection.

Fix: the macro is now a global `add_compile_definitions()` in
`CMakeLists.txt`, placed ahead of the `FetchContent` block so the
fetched dependencies get it too, and removed from the per-target list.

Verified on Windows (August 22, 2026, MSVC 14.51 / Qt 6.11.1, fresh
Release tree): every one of the 55 compiled targets — app, core,
filters, all tests, spdlog, QCoro, QXlsx, sentry, crashpad — carries
the macro in its generated project, and none is without it;
`dumpbin /imports acquisition.exe` shows `_Mtx_init_in_situ` imported
from MSVCP140.dll, i.e. the runtime-initialized constructor that an old
CRT understands rather than the constexpr zero-init; `ctest` 39/39; the
app launches and runs past `logging::init`. Not reproduced against an
actual 14.28 `msvcp140.dll` — the workstation has a current CRT — so
the closing evidence is the import, not a crash-then-no-crash.

The remaining installer gap was closed August 27, 2026. `installer.iss`
now runs the `windeployqt`-supplied redistributable as a mandatory
prerequisite before modifying Acquisition, validates its exit code,
propagates restart-required results, and preserves its diagnostic log
under the user's local application data. Packaging fails if
`windeployqt` omits the redistributable or supplies one without a valid
Microsoft signature; the workflow does not download a fallback. The installer
skips the prerequisite when an equal or newer registered runtime is already
installed, preserving an unelevated per-user installation in that case. When
the runtime is missing or older, its machine-wide update requires administrator
approval, after which Acquisition can still be installed per-user. Every install
also deletes stale app-local `msvcp140*`, `vcruntime140*`, and `concrt140` DLLs
before copying application files. Redistributable logs are intentionally retained
under `%LOCALAPPDATA%\acquisition\installer-logs` for diagnosis after setup or
uninstallation. The existing stray-DLL runtime
check remains because users historically copied CRT DLLs into the
Acquisition directory themselves.

Verified on the Windows workstation in two isolated installs: a host test
with a newer runtime accepted redistributable result 1638, and a disposable
Windows Sandbox started without a registered x64 runtime, installed
v14.44.35211.00, launched Acquisition with an isolated data directory, and
uninstalled cleanly. Both removed seeded stale app-local CRT files and left
neither CRT DLLs nor `vc_redist.x64.exe` beside the application. The
checked-in Sandbox harness also covers synthetic restart-required and
prerequisite-failure exit paths.

Known diagnostic limitation retained by design:

- `checkMicrosoftRuntime()` runs at `src/main.cpp:164`, *after*
  `logging::init` at `:134`, so it can never fire for a crash in the
  CRT's first lock. It also only looks for stray DLLs beside the exe
  (`src/util/checkmsvc.cpp`) and never checks the system CRT's
  version. The global compatibility macro protects startup against an
  old system runtime, while the mandatory prerequisite upgrades it.

## Standing constraints and lessons

Rules distilled from resolved findings that remain binding on future work.
The F-numbers refer to the ledger below.

- **F5 — one HEAD at a time.** Users got Cloudflare-blocked when HEAD
  requests flooded, so at most one HEAD may ever be in flight. Amended
  by network-redesign phase 3 (July 20, 2026, per spec D4/D5): the
  property is now enforced at the gate — a HEAD probe holds the gate's
  exclusive permit, so concurrent endpoint setups serialize there — and
  the old nested-event-loop implementation is deleted. Still binding:
  the hub runs on the main thread (a `Q_ASSERT` in `RateLimiter` enforces
  the affinity), and any change to the gate's HEAD exclusivity must
  preserve one-HEAD-at-a-time deliberately.
- **F29 — logging teardown comes last.** Any log call after
  `spdlog::shutdown()` crashes from any thread. Shutdown lives in a
  `qScopeGuard` declared before `Application` in `main.cpp` so it runs
  after all threads are joined; keep it that way.
- **F30 — BORDERLINE is not an error.** The frequent "policy is
  BORDERLINE" rate-limit messages during refreshes are normal saturation
  pacing, not a failure signal (downgraded from `warn` to `info`,
  August 2026).
- **F31 — check acceptance criteria against non-goals.** A grep-shaped
  acceptance criterion once forced out a load-bearing guard the same
  spec's non-goals said to keep. Mechanical criteria are subordinate to
  stated intent.
- **F42 — never mutate `logger->sinks()` outside `logging::init`.**
  UI-lifetime sinks attach/detach through the permanent
  `dist_sink_mt` hub, whose mutex makes detach safe against worker-thread
  logging.

---

## Resolved ledger

Full prose for every entry is in git history (see the register rules
above). "PR #161" refers to the post-Phase-6 follow-ups branch
(`cleanup-followups`).

| F | Finding | Resolution |
|----|---------|------------|
| F1 | Detached parser thread mutated worker state | Fixed, Phase 2 |
| F2 | End-of-parse `Update()` ran network code on the parser thread | Fixed, Phase 2 |
| F3 | `QMessageBox` created inside the worker | Fixed, Phase 1 |
| F4 | Error paths left the update state machine inconsistent | Fixed, Phase 2 |
| F5 | `SetupEndpoint` nested event loop | Standing constraint (above) |
| F6 | Core included `ui/mainwindow.h` for `ProgramState` | Fixed, Phase 1 |
| F7 | Gratuitous `application.h` includes | Fixed, Phase 1 |
| F8 | Filters located `MainWindow` via the widget tree | Fixed, Phase 1 |
| F9 | Dialog UI defined inside business classes | Fixed, Phase 1 (worker) + Phase 6 (Shop, CurrencyManager, UpdateChecker) |
| F10 | Model consumers emitted the model's signals | Fixed, Phase 3 |
| F11 | `FilterItems` rebuilt the model's store with no reset | Fixed, Phase 3 |
| F12 | Sort emitted bare `layoutChanged` | Fixed, Phase 3 |
| F13 | `ImportBuyouts` was a stub behind a working menu action | Retired, Phase 1 |
| F14 | Clearing a buyout left a stale in-memory entry | Fixed, Phase 6 |
| F15 | Tab-signature machinery dead and incoherent | Deleted, Phase 1; its metadata-refresh sketch is absorbed by items-pipeline M1 |
| F16 | Leftover hardcoded debug probe | Deleted, Phase 1 |
| F17 | Signals declared with non-void return types | Fixed, Phase 1 |
| F18 | `Search` owned a `QTreeView&` | Fixed, Phase 4 |
| F19 | Filter classes were widgets-plus-logic | Fixed, Phase 5 |
| F20 | `MainWindow` owns workflow state | Scoped down and done, Phase 6 (opportunistic extraction only) |
| F21 | Every `Item` stores its raw JSON | Overtaken by events (glaze migration); dead persistence path swept, PR #161 |
| F23 | `ModelViewRefresh` accumulated duplicate connections | Fixed, Phase 3 |
| F24 | Dead update-cancellation members | Removed, Phase 2 |
| F25 | `ItemsModel` minted out-of-contract indexes | Fixed, Phase 3 |
| F26 | `MemoryDataStore` dead code | Deleted, Phase 1 |
| F27 | Re-entrant completions could finish an update early | Resolved by the Phase 2 network rework (single request in flight) |
| F28 | In-flight replies from an aborted update were misattributed to the next one, and updates began destructively — a terminal failure left `m_items` silently short, published by the next successful partial refresh (the likely "item missing until restart" mechanism) | Fixed, items-pipeline M1 (update generation tag + atomic per-reply replacement). Validated by the offline fake-network harness (mutation-verified stale-discard and fail-mid-update pins) and the July 16 live network-kill; the recorded missing-item repro was retired as moot once the destructive cull path was deleted. **Superseded mechanism (network-redesign phases 4b/5, July 2026):** the generation guard was first made unreachable by the phase-4b future boundary (each fetch is completed exactly once by the pump; the old duplicate-emission path the stale-discard pin relied on is gone), then deleted outright with the generation tag in phase 5 (D6). Update identity is the per-update `std::stop_token`: batch submission puts several fetches in flight at abort, but they resolve `Canceled` as accounted stopped siblings, and a straggler that resolved successfully is discarded by the mandatory post-await check. F28's protection is structural at the future boundary; no generation machinery remains. `tst_workerupdate`'s `failedUpdateDoesNotLeakIntoTheNext` asserts the observable half — a terminal failure loses nothing and the next update starts clean |
| F29 | `spdlog::shutdown()` raced logging threads | Fixed, Phase 2; standing lesson (above) |
| F30 | Rate limiter never surfaced failed replies | Fixed, Phase 2; BORDERLINE note (above) |
| F31 | Phase 3 spec forced out a load-bearing view-signal guard | Resolved after Phase 3 (coalesced resize); standing lesson (above) |
| F32 | Per-search view state not preserved across tab switches | Fixed, Phase 6 (items 6.5/6.6) |
| F33 | Filter activity flags shared across searches | Fixed, Phase 5 |
| F34 | `Bucket::Sort` inverted Qt sort-order semantics | Fixed, Phase 6 item 6.8 |
| F35 | Socket-color boxes never cleared across searches | Fixed, Phase 5 |
| F36 | Mods filter form-sync quirks (a–e) | Fixed, Phase 5 step 6 |
| F37 | No `MainWindow` end-to-end test | Fixture built, Phase 6 item 6.7 |
| F38 | "Influenced" filter matched fractured/synthesised items | Fixed, PR #161 |
| F39 | Current-bucket pointer could dangle or start null | Fixed, Phase 6 item 6.6 |
| F40 | `LogPanel` leaked dangling spdlog sinks | Fixed, Phase 6 item 6.7 |
| F41 | Fast tab switch left the outgoing tab's caption stale | Fixed, PR #161 |
| F42 | `LogPanel` sink teardown lifetime race | Fixed, PR #161; sink-hub rule (above) |
| F43 | Restored bucket selection not highlighted in the tree | Fixed, PR #161 |
| F44 | Item-path warning branches kept stale selection state | Fixed, PR #161 |
| F45 | Shop threads could not be cleared; no-threads warning unreachable | Fixed, July 2026 (own change) |
| F46 | `ItemsManager::OnItemsRefreshed` ran an O(items) uncategorized-items scan purely for logging, even with both log levels disabled | Fixed, items-pipeline M2 stage 2 (R1-9): gated behind `spdlog::should_log(debug)` in the same commit that reworked the function for streaming; the scan never runs on the delta path |
| F47 | `ItemLocation::FixUid()` was dead code | Deleted, items-pipeline M1 |
| F49 | Folder children suspected of being fetched twice via two paths | Closed by live observation, July 2026: the paths are complementary in the live API — folder children arrive only via the stash list (Standard, 16 child lists; the two individually fetched folders returned no `children` and no items), map/unique children only via the individual reply (Mirage, 73 children). The `OnStashReceived` tripwire warning stays in the code as a permanent guard should the API ever change. July 17 amendment: the warning's "fetched twice this update" claim can be false — during a partial refresh a known-but-unselected child is never queued from the stash list, so the parent path would be its only fetch. Reworded to "may be fetched twice" in the F53/F55 follow-up PR |
| F48 | Character-list skip-check compared names against an id-keyed index (never matched), so a partial update re-added and re-fetched every character in the league, duplicating their tab entries and items | Found and fixed, items-pipeline M1 (character entries rebuilt from the fresh list, keyed by id) |
| F51 | Unnamed stash tabs (~30 on the validating account, real in-game data) collapse the label component of the legacy item-buyout hash, suspected of shadowing item buyouts across tabs | Reframed and closed, July 2026: active item buyouts key on the API item id (`BuyoutManager::Set`/`Get` use `item.id()`); the label-based `hash_v4` is consumed only by the one-time v4→v5 migration, where colliding tabs made that migration ambiguous — an accepted legacy quirk. Do not rekey `GetLegacyHash`: it would break future v4→v5 migrations without improving live lookups |
| F52 | `PropagateTabBuyouts` issued one no-op buyout DELETE per item on every refresh (~17k per refresh on an 18.5k-item account) | Fixed, PR #163: the clear path touches the repo only when the in-memory map holds an entry; per review, `removeItemBuyout`/`removeLocationBuyout` report success and the map entry is erased only afterward, so a failed DELETE is retried on the next clear (pinned by a `BEFORE DELETE RAISE(FAIL)` trigger test). Accepted, test-pinned: a row written behind the manager's back survives an in-session clear and heals at the next `Load()`; save-path failures are still discarded by the signal connection (deliberate asymmetry). Drift note: `Compress*` drifts the map only toward orphan repo rows, which the guard leaves alone; `MigrateItem` rekeys in memory only and so drifts both ways at once (old row orphaned, new key rowless — see F54); a failed save also leaves a rowless map entry; clearing a rowless entry is healed by a zero-row DELETE. Standing M2 constraint: the per-tab delta path must scope buyout propagation to the delta, not rerun the loop per tab reply |
| F53 | Deleted stash tabs and characters resurrected from the cache at restart: the repos only upserted listed rows and could not even express "everything was deleted" (empty lists returned early) | Fixed, F53/F55 follow-up PR: authoritative-list signals (`stashListReplaced`/`characterListReplaced`, emitted only for fresh top-level lists — never for `ProcessTab`'s folder-children re-emits) drive `reconcileStashList`/`reconcileCharacterList`, deleting rows absent from the recursively flattened list (realm-wide for characters, matching the endpoint) with empty lists handled; children of surviving Map/Unique parents are preserved and reconciled by `stashChildrenReplaced` on the parent's reply instead — scoped to Map/Unique parents only, because live folder replies carry no children (F49) and keying off them would wipe legitimate child rows. With child fetching disabled the parent reply deletes cached child rows, so re-enabling the setting refetches instead of showing stale cache (documented policy). Pinned at repo level (`tst_reconcile`) and end-to-end through the fake network |
| F50 | Header-less and transport-failed replies were logged as rate-limit-header anomalies, framing plain network failures as protocol problems (they were misread that way during M1 validation) and conflating two distinct cases | Fixed in network-redesign phase 4a: `RateLimitManager::Update()` logs the parse failure at `debug` when the reply carried a transport error or no `X-Rate-Limit-Policy` header at all — neither indicates anything about the protocol, and GGG does not header every error response. The loud `error` is reserved for the case that genuinely does indicate a protocol change: a would-be-clean 2xx, which classification now also completes as `FetchError{Protocol}` rather than delivering it as a success (D8/IR1). The split the finding asked for is therefore structural, not just a rewording — the two cases now differ in outcome, not only in log level |
| F59 | `RateLimitedReply`'s ownership contract was contradictory: `RateLimiter::Submit`'s declaration told the caller to `deleteLater()` the reply, while the pump's entry owned it via `unique_ptr` and destroyed it synchronously after the completion emit — benign only because `complete` was a direct same-thread connection, one reordering or queued connection away from a use-after-free | Resolved in network-redesign phase 4b: the worker's call sites moved to `PoeApiClient`, and with no caller left holding a reply object the legacy `Submit()` adapter, `RateLimitedReply` (`.h`/`.cpp`), and the synthetic reply were deleted together — the contract no longer exists. (Phase 4a had narrowed it to the adapter only; an earlier claim that 4a resolved it by construction was wrong and corrected then.) `tst_workerupdate` moved to a typed facade fake and `tst_ratelimiter` lost its legacy-wrapper pins |
| F60 | The legacy stash-index request was built bare — no `setTransferTimeout` — so a stalled GET (or the endpoint's HEAD probe, which inherits the request) had no client-side bound and could hang until the OS gave up, leaving the shop update waiting forever; under the redesign it would also hold a gate permit indefinitely and, with a HEAD waiting under writer preference, stall the entire hub | Fixed in network-redesign phase 4a by construction: `PoeApiClient` owns request building for every API call including this one, and sets the 10 s transfer timeout the gate's liveness invariant depends on (D5/R5-3). `Shop` no longer builds a request at all. Pinned by `tst_poeapiclient`'s `everyRequestCarriesTheTransferTimeout`, which checks EVERY builder rather than only the one that was broken — verified to fail when the call is removed |
| F55 | A terminal failure between list receipt and a new tab's first fetch consumed the tab's newness durably (metadata persists at list receipt), so later partial refreshes published the tab empty — release-blocking for M1's always-fetch note | Fixed, F53/F55 follow-up PR: the always-fetch decision keys on a contents-known set — seeded in `ParseCachedItems` from rows whose stash/character json was actually saved, extended on successful replies — instead of list membership (`previously_known` removed). No schema change: the `listed_at` vs `json_fetched_at`/`json_data` split already existed, and no path writes json without its timestamp (`LegacyDataStore` is unwired). Rejected: skipping the list-receipt metadata save (regresses the absorbed-F15 metadata refresh and misses the in-session case). Pinned by `failedFirstFetchDoesNotConsumeNewness` (the ledger-specified scenario) and `listedButNeverFetchedTabIsFetchedOnNextUpdate` (the restart shape). Review follow-up (July 17): a Map/Unique parent counts as contents-known only once every enabled child fetch has landed — completion is deferred to the last child reply in-session, and a cached parent whose saved reply records children with a missing child row stays "new" at startup (special children never appear in a top-level list, so nothing else would retry them); pinned by `failedChildFetchKeepsParentNew` and `cachedParentWithMissingChildRowStaysNew`. Round 2: starting a child-fetch cycle also *evicts* an already-known parent from contents-known (chosen over per-child-id completeness tracking: eviction is uniformly conservative — worst case one redundant refetch after a mid-cycle failure — while stale in-memory child known-ness could re-strand a child after rows were cleared under a disabled setting), pinned by `knownParentWithNewFailedChildIsRetried`; and the `ParseCachedItems` settings reads were hoisted to the main thread (the parser thread must not touch the shared `QSettings` instance the UI writes — reentrant, not thread-safe). Known residual, accepted: re-enabling `get_map_stashes`/`get_unique_stashes` mid-session leaves the parent known until it is next fetched (full refresh or selection) or the next restart's seed check. Release-note wording narrowed to "any content refresh": `TabsOnly` records a new tab without fetching it and the next content refresh picks it up. **Superseded by F61 (PR #175 testing):** the always-fetch behavior and the entire contents-known apparatus this entry describes were reverted — a partial refresh now fetches strictly its selection — so the `failedFirstFetchDoesNotConsumeNewness`, `failedChildFetchKeepsParentNew`, `cachedParentWithMissingChildRowStaysNew`, and `knownParentWithNewFailedChildIsRetried` pins were removed and `listedButNeverFetchedTabIsFetchedOnNextUpdate` became `partialRefreshSkipsNeverFetchedTabs` |
| F56 | Single-lane item-request serialization (`m_queue` / `SubmitNextItemRequest`, at most one request in flight, stashes-first) starved the character policy: with the per-policy managers idle behind the worker's mixed FIFO, refresh time degraded from max(stash, character) to stash + character | Fixed, network-redesign phase 5 (July 21, 2026): the worker's queue and update-generation machinery are deleted. The root orchestration (`RunUpdate`, a synchronous counter-driven join) launches every required list without awaiting one another, and each list handler launches its whole content batch through `LaunchContent`, so all lanes are concurrently outstanding under the hub's per-policy coroutine pumps and the global gate (cap 2). One `std::stop_source` per update is the sole cancellation-and-identity channel — the post-await token check replaces the deleted generation guard (`IsStale`), and per-fetch coroutine handles are owned in `m_fetch_tasks` and reclaimed by a deferred, coalesced sweep. Pinned at worker level by `tst_workerupdate`'s staged-batching pins (`W-F56-*`: both lists submitted before either settles, each lane's whole content batch out before any reply lands, folder/Map/Unique child batches, lane-local source order, a stopped sibling that mutates nothing) and end-to-end by the `tst_workerintegration` full-chain runner (real worker → facade → hub with `FakeScheduler`/`FakeNetworkManager`): cross-layer cancellation at every pump checkpoint (`I-CANCEL-PACING/GATE/FLIGHT`), the post-event-loop destruction contract (`I-SHUT-PACING/GATE/FLIGHT/RETRY`), bounded detached-frame leaks (`I-LEAK-BOUND`), and non-accumulating completed-frame retention (`I-RETENTION`), each shutdown scenario in its own CTest process. `I-LEAK-BOUND` is enforced on Linux CI by `.github/workflows/sanitizers.yml`, which builds only the runner with `-DACQ_SANITIZE=address` and runs each scenario as its own process under LeakSanitizer, in two steps: the shutdown scenarios (`i_shut_*`, `i_leak_bound`) run with `tests/lsan.supp` (which matches the accepted detached-QCoro-frame coroutine names in the allocation stack), and the leak-clean scenarios (`i_cancel_*`, `i_retention`, `fullChainStashListSucceeds`) run with no suppression file at all so any leak fails the job. A leak whose stack falls outside the coroutine closure fails the job in either step (proven load-bearing: a deliberate out-of-closure leak turned the job red while every suppressed scenario stayed green). The suppression matches allocation-stack frames rather than object reachability, so within a suppressed scenario a leak allocated beneath one of those coroutines is silenced; the leak-clean/suppressed split bounds that to the scenarios that legitimately strand frames (see the verification contract's "Leak and retention interpretation") |
| F57 | A 429 retry destroyed the caller's `RateLimitedReply`, dropped the retried completion, and wedged the update until restart (reproduced offline by the phase-1 harness) | Fixed, network-redesign phase 3 (July 20, 2026): the pump retries invisibly inside the drain loop — bounded attempts, padded deadline, permit-free sleep — and the caller sees exactly one final completion; the phase-1 wedge pin flipped to `retry429CompletesCallerExactlyOnce` |
| F58 | The minimum-send-interval spacing was dead code (`last_send` never assigned) | Fixed, network-redesign phase 3 (July 20, 2026): the dead block deleted with `ActivateRequest`; the intent is implemented deliberately at the right scope as the gate's `MIN_SEND_SPACING` floor (250 ms across everything the hub sends, measured from dispatch stamps — spec D5), pinned exactly on the injected clock |
| F61 | The revised-F55 always-fetch rule keyed "new" on cached contents (`m_contents_known`), so a partial refresh (refresh selected / refresh checked) fetched every tab whose contents were not cached — turning it into a full refresh whenever the contents cache was cold: a fresh install, an upgrade from an older Acquisition (whose contents live in a separate `userstore-*.db` that is never migrated from the legacy store), or a datastore that had only ever stored tab lists. Reported during PR #175 testing (July 22, 2026): "refresh selected/checked refreshes all my tabs" | Fixed, PR #175: the per-tab fetch gate in `ProcessTab`/`OnCharacterListReceived` drops its contents-known clause and keys purely on the selection (`m_update_all || m_tabs_to_update.count(id)`), so a partial refresh fetches strictly the tabs it was asked for; a newly discovered tab is still added to the tab list (metadata surfaces in the UI) but waits for a full refresh or an explicit selection. Children of a selected Map/Unique parent are unaffected — they are discovered in the parent's reply (`OnStashReceived`, gated only by `get_map_stashes`/`get_unique_stashes`) and ride the parent's fetch decision, never appearing in a top-level list. With the selection now the sole gate, the whole contents-known apparatus is dead and was deleted: `m_contents_known`/`m_pending_children`, the `ParseCachedItems` seeding and its `get_*_stashes` parameters (the parser thread no longer reads any setting), and the Map/Unique deferred-completion accounting. Deliberate policy change superseding F55's always-fetch note: a newly created tab no longer auto-fills on a partial refresh. Pinned by `partialRefreshSkipsNeverFetchedTabs` (partial skips a never-fetched tab, a full refresh fetches it) and `partialRefreshFetchesChildrenOfSelectedMapParent` (a selected special stash still reaches its children); design doc "F55, revised" section and M1 release notes updated. **0.18 note (July 2026):** the `json_version` payload invalidation adds another concrete listed-but-cold state this policy governs — version-mismatched rows keep tab metadata but yield no contents (pinned by `staleRowsKeepMetadataButYieldNoJson`), so the 3.29 wire-format change puts every upgrader there; contents stay cold until a full refresh or an explicit selection refills them |
| F64 | Databases created by v0.16.0-alpha.2 through alpha.6 were permanently broken by an unversioned schema change: those alphas built `stashes`/`characters` with composite primary keys — `(realm, league, id)` and `(realm, id)` — and stamped `user_version` 1; alpha.7 switched to `id`-only keys and `ON CONFLICT(id)` upserts, and added the buyout tables, without bumping `SCHEMA_VERSION`. Every later release then rejected such a database's stash/character saves at prepare time ("ON CONFLICT clause does not match any PRIMARY KEY or UNIQUE constraint") and failed every buyout operation ("no such table: item_buyouts"); 0.18.0-alpha.1's 1→2 ALTER step even succeeded on the old tables and stamped them fully current. User-reported against v0.18.0-alpha.1 ("can't fetch tabs or update the shop"), July 27, 2026 | Fixed on `fix/userstore-schema-repair`: `SCHEMA_VERSION` 3 with a repair step that rebuilds `stashes`/`characters` via `resetRepo()` only when `PRAGMA table_info` shows the primary key is not exactly `(id)` — refetchable caches, but a healthy database keeps its rows (the tab/character lists drive the UI until the next refresh, pinned by `migratesVersion1ToCurrent`) — and runs `BuyoutRepo::ensureSchema()` (`CREATE IF NOT EXISTS`; user-authored buyouts are never dropped, pinned by `buyoutRowsSurviveTheRepairStep`). The broken shape itself is pinned by `compositeKeyDatabaseIsRebuilt`, which exercises the `ON CONFLICT(id)` paths on the rebuilt tables. `migrate()` also now logs when `BEGIN IMMEDIATE` fails instead of silently skipping the migration. Standing lesson: any DDL change to repo tables must bump `SCHEMA_VERSION` in the same commit — `CREATE TABLE IF NOT EXISTS` silently keeps whatever shape an existing database has |
| F65 | The legacy stash endpoint returned the same `backend-item-request-limit` policy name with an `Ip`-only shape for an invalid/unauthenticated POESESSID and an `Account,Ip` shape after a valid POESESSID was installed. The pump adopted the new definition but logged the expected authentication transition as a warning and error, which surfaced to the user, and retained request history recorded against a different counter set | Fixed July 28, 2026: same-name shape changes are explicitly supported. `RateLimitPolicy::HasSameShape()` compares rule names, item counts, and bucket periods; `RateLimitManager::Update()` clears history before adopting a different shape, logs one concise `info` message, and leaves the full transition at `debug`. Same-shape limit-value changes retain history. Pinned by `tst_ratelimitmanager::policyShapeChangeClearsHistory`, which transitions `Ip` to `Account,Ip` after recording a saturating event and proves the next request is delayed only by the gate floor rather than the obsolete history |
| F62 | The stash/character cache stored a lossy re-serialization (`json::writeStash`/`writeCharacter` of the parsed structs), not the JSON GGG sent: reads tolerate unknown keys but glaze writes only declared members, so every unmodeled API field was silently dropped before it reached `json_data`. The cache could not backfill a newly modeled field, could not reproduce a parse bug, and could answer a wire-format change only by invalidation — 3.28→3.29 would otherwise have been a mechanical blob transform instead of emptying the cache (found July 24, 2026, while designing that invalidation) | Fixed July 28, 2026, per the July 26 decision (full prose in git history): raw wire bytes flow through the persistence lane at per-reply granularity. The facade captures the reply's stash/character sub-object losslessly (`glz::raw_json`), parses the typed payload from that same substring, and returns both (`poe::StashPayload`/`poe::CharacterPayload`); `stashReceived`/`characterReceived` carry the bytes as an opaque `QByteArray` the worker never interprets; `saveStash`/`saveCharacter` persist the bytes verbatim. `json_data` keeps its shape — the tolerant reader parses old re-serialized rows and new wire rows alike — and `json_version` labels GGG's wire format from then on, which is what makes a future blob upgrader possible. The save trigger stays the worker's post-acceptance emit, so nothing the worker discards (stopped stragglers, failed parses) is ever persisted. A 200 whose reply lacks its stash/character sub-object (missing or null) is classified at the facade as `FetchError{Parse}` per M2 D5/R2-4 — the payload members are non-optional, so a success cannot represent that state, and the worker's untyped "is empty" abort branches are deleted (independent review of the first implementation caught that it initially preserved them as successful empty payloads, contradicting D5). Network-redesign D7 amended in place: nothing above the boundary *interprets* bytes. Rejected alternatives: a facade/pump-level persistence tap (persists replies the worker discards — reintroduces the cache/memory divergence class M1 eliminated) and glaze unknown-field capture on every poe type (known-field parse bugs still bake in; every nested type carries an `extra` map forever). Tests serialize typed fixtures where real replies' bytes would flow (`FakePoeApiClient`, `saveStashFixture`/`saveCharacterFixture`) — re-serialization is harmless in tests; it is the production cache that must be faithful. Pinned by `tst_poeapiclient`'s byte-fidelity pins (`stashPayloadCarriesTheWireBytes`, `characterPayloadCarriesTheWireBytes`, `missingStashSubObjectIsAParseError`, `missingCharacterSubObjectIsAParseError`) and `tst_reconcile`'s verbatim-storage pins (`savedStashBytesAreStoredVerbatim`, `savedCharacterBytesAreStoredVerbatim`). Backfill fidelity begins with the first refresh after the fix ships — nothing recovers fields already dropped from existing caches |
| F67 | `Item::operator<` compared `m_hash` against itself: the tie-break tuple's third element was the left-hand hash on both sides, so the intended hash-level determinism for id-less items was dead code (found July 30, 2026, during the M3 sort-profiling spike) | Fixed, items-pipeline M3 S1 (spec D5): the one-token change to `rhs.m_hash` restores the intended `(name, uid, hash)` order, and the keyed-sort suffix carries the same order so keyed and comparator sorts agree. Pinned by `intendedTieBreakRestored` (determinism) and `keyedOrderMatchesComparatorOrder` (equivalence), both in `tst_search` |
| F68 | `src/poe/endpoints/website/` was dead code — no includers anywhere, and two headers (`webleagues.h`, `webstashitems.h`) were copy-paste casualties defining types under filenames that promised something else | Deleted, August 2026 mechanical-findings sweep: the directory and its `CMakeLists.txt` entries removed |
| F69 | `Shop::StashesIndexed` was declared, never emitted, never connected | Deleted, August 2026 mechanical-findings sweep |
| F70 | The "Failed to find item" recovery advice in `Shop::OnShopSubmitted` pointed at a Shop → "Update stash index" menu action that no longer exists (and every job fetches a fresh index anyway) | Reworded, August 2026 mechanical-findings sweep: the message now says to try submitting again |
| F71 | `NetworkManager::createRequest` logged the full `Bearer …` value at trace level, a user-selectable log level — "turn on trace logging and send the log" shipped the token | Fixed, August 2026 mechanical-findings sweep: the value is masked the way `setPoeSessionId` masks POESESSID |
| F72 | The Authorization mask in `NetworkManager::logHeaders` compared `name` (the "request"/"reply" label) instead of `header`, so masking could never fire; latent today, but any future post-send request log would have leaked the bearer unmasked | Fixed, August 2026 mechanical-findings sweep: the check compares `header` |
| F73 | Token bytes could reach the error log via `glz::format_error`'s buffer context on token serialization failure (`OAuthManager::receiveToken`) and token parse failure (`read_json` via `readOAuthToken`) | Fixed, August 2026 mechanical-findings sweep: `receiveToken` formats the error without the buffer, and `read_json` takes a `buffer_may_hold_credentials` flag (set by `readOAuthToken`) that logs the error code and position only |
| F76 | Dead OAuth/session code: `OAuthManager::m_authenticated` never written, `isAuthenticatedChanged` connected nowhere, `LoginDialog::OnSessionIDChanged` never connected | Deleted, August 2026 mechanical-findings sweep (F69 precedent) |
