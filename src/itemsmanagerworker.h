// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QCoroTask>

#include <QNetworkCookie>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <expected>
#include <functional>
#include <map>
#include <set>
#include <stop_token>
#include <variant>
#include <vector>

#include "fetchsourcekey.h"
#include "item.h"
#include "refreshoutcome.h"
#include "sourcekeyeditems.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "ratelimit/fetcherror.h"
#include "util/programstate.h"
#include "util/util.h"

class QSettings;
class QSignalMapper;
class QThread;
class QTimer;

class BuyoutManager;
class DataStore;
class ItemLocation;
class NetworkManager;
class PoeApiClient;
class RePoE;

// What a content fetch asks the API for. The worker names the resource; the
// facade builds the request (phase 4b) — above this line nothing sees a
// QNetworkRequest.
struct StashFetch
{
    QString stash_id;
    QString substash_id;
};

struct CharacterFetch
{
    QString name;
};

// One entry in a content batch: what to fetch and where its items belong. A
// batch is a plain local vector built inside the handler that discovered it
// (the stash list, the character list, or a parent reply's children) and
// launched all at once by LaunchContent — there is no persistent worker queue
// (F56).
struct ItemsRequest
{
    std::variant<StashFetch, CharacterFetch> what;
    ItemLocation location;
};

struct ParseResult
{
    std::vector<ItemLocation> tabs;
    Items items;
    std::set<QString> tab_id_index;
};

// Read-only observation of the deferred task sweep (network-redesign phase 5,
// verification §2). Injected by the worker suite; null in production. The
// worker only ever writes to it — the seam observes lifecycle, it never drives
// it. W-SWEEP asserts that a completion schedules a sweep that runs on a later
// event-loop turn and destroys only completed (ready) per-fetch handles.
struct WorkerSweepObserver
{
    int scheduled = 0;     // ScheduleSweep() actually queued a sweep (coalesced)
    int executed = 0;      // SweepTasks() ran
    int destroyed = 0;     // ready handles erased, summed across every sweep
    size_t live_after = 0; // per-fetch handles still held after the last sweep
};

class ItemsManagerWorker : public QObject
{
    Q_OBJECT
public:
    explicit ItemsManagerWorker(QSettings &m_settings,
                                BuyoutManager &buyout_manager,
                                PoeApiClient &api,
                                QObject *parent = nullptr);
    ~ItemsManagerWorker() override;

    void UpdateRequest(TabSelection type, const std::vector<ItemLocation> &locations);

    // The single test-only seam into the worker (network-redesign phase 5,
    // verification §2). Only forward-declared here and DEFINED in a test-only
    // header (tests/workertestaccess.h), so it grants the worker suite private
    // access without adding any test surface — production code cannot reach it,
    // and the worker's public API carries no standing *ForTest / Set* methods.
    friend class WorkerTestAccess;

    // Runs on the parser thread and must not touch m_settings (QSettings is
    // not thread-safe for one shared instance the UI writes concurrently); it
    // reads only the datastore and the realm/league captured at construction.
    ParseResult ParseCachedItems(const QString &dataDir) const;

signals:
    void ItemsRefreshed(const Items &items,
                        const std::vector<ItemLocation> &tabs,
                        bool initial_refresh);
    void StatusUpdate(ProgramState state, const QString &status);
    void NotifyUser(const QString &message);

    // Presentation-lane delta signals (items-pipeline M2, D3). Both carry
    // pipeline-native payloads only — never wire bytes or poe::* objects,
    // which belong to the persistence lane below (D1); the lanes never share
    // payload types.
    //
    // TabRefreshed: the complete replacement for one fetch source — the exact
    // Item objects just parsed and appended, sharing their shared_ptrs.
    // Consumers apply it strictly by FetchSourceKey
    // {location.type(), location.fetch_id()}, never by location equality
    // (which deliberately ignores fetch_id). An empty items means an emptied
    // fetch source, never a deletion — tab deletion stays snapshot-boundary
    // (D6). Emitted immediately after the atomic replace and before the
    // counter increment, so every delta precedes its update's terminal event.
    void TabRefreshed(const ItemLocation &location, const Items &items);
    // ChildrenReconciled: one aggregate child reconciliation per top-level
    // parent reply (fetch_id == id — the same condition as the worker's own
    // ghost erase, NOT the Map/Unique-only condition that gates the
    // persistence-lane stashChildrenReplaced). `expected` is authoritative:
    // the parent's own fetch source plus its currently listed children.
    // Consumers erase items under the parent's stable id whose key is not in
    // the set, against their OWN baseline — correct even where published
    // state diverged from worker memory across a failed update (R5-2/R6-2).
    void ChildrenReconciled(const ItemLocation &parent, const std::vector<FetchSourceKey> &expected);

    // The typed terminal event (M2 D4): exactly once per accepted Update(),
    // after the worker is observably Idle — FinishUpdate emits
    // CompletedRefresh after the final ItemsRefreshed and the Idle
    // transition; the first AbortUpdate emits FailedRefresh with the
    // update's first terminal error. Update() during this fan-out is
    // refused (R4-4): an observer that chains a restart queues it.
    void RefreshFinished(const RefreshOutcome &outcome);

    void characterListReceived(const std::vector<poe::Character> &characters, const QString &realm);
    // The per-fetch persistence signals carry the reply's exact wire bytes
    // alongside the parsed payload (F62). The worker never interprets the
    // bytes — it couriers one opaque blob per reply from the facade to the
    // datastore, which persists the bytes rather than a lossy
    // re-serialization of the parsed struct. Emission stays post-acceptance:
    // nothing the worker discards (stopped stragglers, failed parses) is
    // ever persisted.
    void characterReceived(const poe::Character &character,
                           const QByteArray &bytes,
                           const QString &realm);
    void stashListReceived(const std::vector<poe::StashTab> &stashes,
                           const QString &realm,
                           const QString &league);
    void stashReceived(const poe::StashTab &stash,
                       const QByteArray &bytes,
                       const QString &realm,
                       const QString &league);

    // Authoritative-list signals (F53): emitted only for a fresh top-level
    // list (never for ProcessTab's folder-children re-emits of
    // stashListReceived, which are partial and must not drive deletion) so
    // the datastore can drop rows the server no longer lists.
    void stashListReplaced(const std::vector<poe::StashTab> &stashes,
                           const QString &realm,
                           const QString &league);
    void characterListReplaced(const std::vector<poe::Character> &characters, const QString &realm);
    // Emitted when a parent stash reply arrives (F53): the reply is
    // authoritative for the parent's children, so the datastore can drop
    // child rows it no longer lists. child_ids is empty when child
    // fetching is disabled in the settings.
    void stashChildrenReplaced(const QString &parent_id,
                               const QStringList &child_ids,
                               const QString &realm,
                               const QString &league);

public slots:
    void OnRePoEReady();
    void Update(Util::TabSelection type, const std::vector<ItemLocation> &tab_names = {});

private:
    // Every handler takes one already-classified result (phase 4b): the
    // transport check, the status check, and the parse the handlers used to
    // do all live below the facade now, so a failure of any flavour arrives
    // as the same unexpected value.
    template<typename T>
    using Result = std::expected<T, RateLimit::FetchError>;

    void OnStashListReceived(const Result<poe::StashListWrapper> &result);
    void OnStashReceived(const Result<poe::StashPayload> &result, const ItemLocation &location);
    void OnCharacterListReceived(const Result<poe::CharacterListWrapper> &result);
    void OnCharacterReceived(const Result<poe::CharacterPayload> &result,
                             const ItemLocation &location);

    enum class WorkerState { Initializing, Idle, Updating };

    bool isInitialized() const { return m_state != WorkerState::Initializing; }
    bool isUpdating() const { return m_state == WorkerState::Updating; }
    void StartParseThread();
    void OnParseCompleted(ParseResult result);
    void LoadItems(const poe::Character &character,
                   ItemLocation location,
                   ParseResult &result) const;
    void LoadItems(const poe::StashTab &stash, ItemLocation location, ParseResult &result) const;
    void RebaseItemLocations(ItemLocationType type);
    void SubmitStashListRequest();
    void SubmitCharacterListRequest();
    // Launch a whole content batch at once (D6, F56): no worker-side pacing, no
    // one-at-a-time submission. The batch is a local vector the caller built —
    // one policy lane's worth of fetches (a list's content, or a parent reply's
    // discovered children) in source traversal order. The whole batch's needed
    // counters are initialized before its first launch, because a ready/fail-fast
    // future runs its completion synchronously inside the launch loop (IR2/S1-6).
    void LaunchContent(std::vector<ItemsRequest> batch);

    // The root orchestration (D6, rev. 10): it launches the update's required
    // list(s) without awaiting one another and returns. It is an ordinary
    // synchronous method, NOT a coroutine — the fan-out is a counter-driven join,
    // not a linear await (content and reply-discovered child batches are
    // discovered dynamically and cannot be a static co_await sequence), so it
    // never suspends and there is no asynchronous lifetime for an owned task
    // handle to manage. Only the per-fetch tasks suspend, and they are owned in
    // m_fetch_tasks (R4-3). Its whole body is wrapped in a catch-all (R5-1) so a
    // throw in orchestration aborts and finalizes rather than escaping into the
    // caller; a plain function's try/catch does this exactly as a coroutine's
    // would, since nothing here suspends. Each list handler launches its own
    // complete content batch (character content is never held behind the stash
    // list), each per-fetch coroutine self-drives, and the completion that
    // reconciles the counters finalizes the update. The m_has_*_list /
    // m_need_*_list flags stay because a counter-driven join cannot tell a
    // required-but-empty list from one that has not arrived yet; they are the
    // minimal list-arrival state, not a residual callback pyramid.
    void RunUpdate();

    // Per-fetch tasks (D6): each co_awaits one facade future via
    // qCoro(future).takeResult(), checks the update token immediately after
    // (post-await identity invariant, IR2), then hands the result to the
    // matching handler. The token is the update's sole identity: a straggler
    // from a failed update carries that update's stopped token, so the post-await
    // check discards it before it can touch a later update's state (W-IDENTITY).
    // A per-fetch catch-all turns an exceptional future (R5-1) into an ordinary
    // Internal failure so it flows through the handler's failure branch and takes
    // the direct terminal abort, instead of escaping into the unawaited task and
    // wedging the update. Every handle is owned in m_fetch_tasks — no
    // fire-and-forget.
    QCoro::Task<> FetchStashList(std::stop_token token);
    QCoro::Task<> FetchCharacterList(std::stop_token token);
    QCoro::Task<> FetchStash(ItemLocation location,
                             QString stash_id,
                             QString substash_id,
                             std::stop_token token);
    QCoro::Task<> FetchCharacter(ItemLocation location, QString name, std::stop_token token);

    // The single, idempotent terminal transition for a failed update (R5-1):
    // every failure — a list/content error, an exceptional future, or a throw in
    // orchestration or a handler — routes here. It requests stop, records at
    // least one request failure, and returns the worker to Idle immediately,
    // abandoning in-flight siblings as stopped stragglers. It
    // is deliberately INDEPENDENT of the completion counters: success reconciles
    // the counters in CheckUpdateFinished(); failure takes this direct path, so
    // the two never contend and a failed fetch never rewrites the counters
    // (which drove reported progress backward, P-STATUS).
    void AbortUpdate();

    // First-failure stop for the value-level failure branches: record the
    // update's first terminal error (D4 — BEFORE the throwable test fault
    // hook, so the stopped-but-still-active window already carries it),
    // stop the update token, then fire the hook (a no-op in production) at
    // the point after stopping the token but before the handler finishes
    // its failure bookkeeping and its direct AbortUpdate() — the window in
    // which the update is stopped but still active, which the catch-alls
    // must still recognize as theirs to abort.
    void StopUpdateForFailure(const RateLimit::FetchError &error);

    // Store the update's first terminal error; later failures and settling
    // stragglers never overwrite it (D4). The catch-alls use this directly
    // with an Internal-kind error.
    void RecordFirstError(const RateLimit::FetchError &error);

    // Invoke the test fault hook once if armed (test-only; may throw — that is
    // the point). See SetFaultHook.
    void FireFaultHook();

    // Queue a deferred, coalesced reclaim of completed per-fetch handles. Runs
    // outside any completing coroutine (D6/R5-1): the completion that
    // finalizes the update must not destroy the frame it is running in. Only
    // ready (finished) handles are destroyed — destroying a suspended
    // straggler's handle would detach, not stop, it (S1-1/S1-4).
    void ScheduleSweep();
    void SweepTasks();

    void SendStatusUpdate();
    void CheckUpdateFinished();
    void FinishUpdate();

    void ProcessTab(const poe::StashTab &tab, std::vector<ItemsRequest> &batch);

    QSettings &m_settings;
    BuyoutManager &m_buyout_manager;
    PoeApiClient &m_api;

    QString m_realm;
    QString m_league;
    QString m_account;

    std::vector<ItemLocation> m_tabs;

    // Source-keyed item storage (M2 D3): the M2-M2 measurement fired the
    // spec's storage conditional, so the per-reply replace and the
    // reconcile/list erases are bucket operations, never O(all-items)
    // passes. Whole-collection reads (ItemsRefreshed emits, rebasing, the
    // finish sort) go through Flat()/buckets() at snapshot boundaries only.
    SourceKeyedItems m_items;

    size_t m_stashes_needed{0};
    size_t m_stashes_received{0};

    size_t m_characters_needed{0};
    size_t m_characters_received{0};

    std::set<QString> m_tab_id_index;

    WorkerState m_state{WorkerState::Initializing};
    QPointer<QThread> m_parser_thread;
    std::atomic<bool> m_shutdown{false};

    bool m_updateRequest{false};
    TabSelection m_type;
    std::vector<ItemLocation> m_locations;
    size_t m_request_failures{0};

    // The update's first terminal error (D4), reset at the next accepted
    // update; AbortUpdate emits it in FailedRefresh. The flag distinguishes
    // "no error recorded" without an optional's overhead in the hot path.
    RateLimit::FetchError m_first_error;
    bool m_first_error_set{false};

    // Deterministic per-source skips (D5, filled in when the skip policy
    // lands); FinishUpdate emits them in CompletedRefresh. Reset at the
    // next accepted update.
    std::vector<SkippedSource> m_skipped_sources;

    // Held across the RefreshFinished emit (D4/R4-4): the worker is
    // observably Idle during the terminal fan-out, but an Update() arriving
    // inside it is refused exactly as if an update were active — a chained
    // restart must queue to the next event-loop turn.
    bool m_delivering_terminal{false};

    // The current update's content selection: refresh everything
    // (All/TabsOnly), or only the tabs/characters whose ids are listed. A
    // partial refresh fetches strictly its selection; a newly discovered
    // tab is listed but not fetched until a full refresh or a selection
    // reaches it (F55, revised).
    bool m_update_all;
    std::set<QString> m_tabs_to_update;

    bool m_need_stash_list;
    bool m_need_character_list;

    bool m_has_stash_list;
    bool m_has_character_list;

    bool m_update_tab_contents;

    // One stop_source per update (D2): every facade call in the update takes
    // its token, and a terminal failure request_stop()s it. Reset to a fresh
    // source at the start of each Update(), so the next update's calls carry a
    // distinct, unstopped token. This token IS the update's identity: the
    // post-await check discards any resumed straggler whose token is stopped,
    // which replaces the deleted generation guard (W-IDENTITY, verification §5).
    std::stop_source m_stop_source;

    // Read-only sweep observation (verification §2); null in production.
    WorkerSweepObserver *m_sweep_observer{nullptr};

    // Test-only one-shot fault injection (verification §3); null in production.
    std::function<void()> m_fault_hook;

    // Set while a deferred sweep is already queued, so many completions in one
    // turn coalesce into a single SweepTasks() run.
    bool m_sweep_scheduled{false};

    // Owned per-fetch coroutine handles (D6/R4-3). Declared LAST so they
    // destruct FIRST: at worker destruction a suspended frame is detached
    // (S1-1), and it must be released before any member its frame could still
    // observe. m_fetch_tasks holds every per-fetch handle until the deferred
    // sweep reclaims the completed ones. The root orchestration (RunUpdate) is a
    // synchronous method that never suspends, so it needs no owned handle here
    // (rev. 10).
    std::vector<QCoro::Task<>> m_fetch_tasks;
};
