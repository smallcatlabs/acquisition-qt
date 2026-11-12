#include <QtTest/QtTest>

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QSettings>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <stop_token>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "fakeapiclient.h"
#include "fakenetworkmanager.h"
#include "fakescheduler.h"
#include "itemsmanager.h"
#include "itemsmanagerworker.h"
#include "poe/poeapiclient.h"
#include "ratelimit/gate.h"
#include "ratelimit/ratelimiter.h"
#include "testfixtures.h"
#include "util/json_readers.h"
#include "util/networkmanager.h"
#include "workertestaccess.h"

// Offline update-cycle tests for ItemsManagerWorker, driven through a fake
// typed API facade (items-pipeline M1; moved off the byte-crafting network
// fake in network-redesign phase 4b). These pin the reachable F28 semantics —
// a failed update loses nothing and the next one starts clean (the stale-reply
// half became unreachable under the future boundary; see
// failedUpdateDoesNotLeakIntoTheNext) — and the F48 no-duplicate-characters
// fix, none of which can be produced reliably by hand against the live API.
//
// The worker's request building and endpoint labels are not pinned here;
// they belong to the facade and are pinned in tst_poeapiclient. What these
// tests pin is what the worker asked for, in domain terms.

// NOTE: this class must be declared before the JSON helpers below: moc's
// lexer treats the // in a raw string literal (the icon URL) as a comment
// and loses any Q_OBJECT class that follows.
class WorkerUpdateTest : public QObject
{
    Q_OBJECT

private slots:
    void failedUpdateKeepsItemsIntact();
    void failedUpdateDoesNotLeakIntoTheNext();
    void partialUpdateDoesNotDuplicateCharacters();
    void renamedTabMetadataRefreshesWithoutFetch();
    void vanishedMapChildItemsAreRemovedOnParentRefresh();
    void failedUpdateDoesNotRebasePublishedLocations();
    void partialRefreshSkipsNeverFetchedTabs();
    void partialRefreshFetchesChildrenOfSelectedMapParent();
    void datastoreFollowsServerDeletions();
    void datastoreFollowsCharacterDeletions();

    // Direct FakePoeApiClient harness pins: the stopped-straggler settlement and
    // cross-update-overlap identity rules the fake enforces, pinned against the
    // fake in isolation. The worker-level behavior that relies on them is pinned
    // separately (W-ER1, W-IDENTITY); these keep the harness contract honest on
    // its own.
    void fakeSettlesStoppedStragglersCanceledExactlyOnce();
    void fakeTreatsCrossUpdateIdentityOverlapAsLive();

    // Phase 5B — task lifecycle, update identity, and the deferred sweep,
    // while submission is still serial (verification §3, W-TOKEN/W-INIT/
    // W-THROW/W-SWEEP). These install the coroutine foundation; they do not
    // yet prove the generation guard is replaced (that is W-IDENTITY, 5D).
    void everyCallInAnUpdateSharesOneTokenAndTheNextUpdateGetsAFreshOne(); // W-TOKEN
    void readyFirstListRunsInlineWithoutCorruptingCounters();              // W-INIT (success)
    void readyFirstListErrorAbortsLeavingTheSecondListAStraggler();        // W-INIT (error)
    void exceptionalFetchIsCaughtAndAbortsTheUpdate();                     // W-THROW
    void handlerExceptionAbortsToIdleInsteadOfWedging();                   // W-THROW (handler)
    void rootOrchestrationExceptionAbortsToIdle();                         // W-THROW (root)
    void completionsScheduleADeferredSweepThatReclaimsFinishedHandles();   // W-SWEEP

    // Phase 5C — staged batching (verification §3). These prove F56 is resolved:
    // both lists and each lane's whole content batch go out without worker-side
    // serialization, folder and Map children batch correctly, source order is
    // preserved per lane, and a stopped sibling resumes safely.
    void stagedBatchesRunConcurrentlyPreservingLaneOrder();             // W-F56-*
    void stoppedSiblingResumesCanceledAndMutatesNothing();              // W-ER1
    void listFailureWithSiblingContentInFlightTerminatesCleanly();      // W-ER1 (list lane)
    void readyContentBatchInitializesCountsBeforeInlineCompletion();    // W-INIT (content)
    void readyContentErrorStillLaunchesTheRestOfTheBatch();             // W-INIT (content error)
    void readyChildCompletesInlineWithoutCorruptingParentBookkeeping(); // W-INIT (child)
    void readyChildErrorStillLaunchesTheRestAndParentStaysRetryable();  // W-INIT (child error)
    void failureKeepsProgressMonotonicWithOneTerminalTransition();      // P-STATUS / P-FAILURE
    void tabsOnlyRequestsBothListsButNoContent();                       // P-SELECTION (TabsOnly)

    // Phase 5D — identity replacement proof (verification §5). The generation
    // guard is gone, so the update token is the sole identity: a successful
    // straggler from a failed update must not touch a subsequent active update's
    // state. Bypassing the shared post-await token gate (ProcessIfLive) makes
    // these fail (mutation proof recorded in the commit). Two lanes are pinned —
    // stash content and character content — so the one gate every fetch coroutine
    // routes through is regression-covered from both.
    void oldSuccessfulStragglerDoesNotCorruptASubsequentUpdate();          // W-IDENTITY (stash)
    void oldSuccessfulCharacterStragglerDoesNotCorruptASubsequentUpdate(); // W-IDENTITY (character)

    // Preservation pins carried from the 5C review (verification §4): the active
    // update refuses a concurrent one, a failed/stopped list emits no
    // authoritative-list signals, and reply-discovered children are accepted on
    // Folder and Unique parents (the generic path accepts them on all three
    // parent types, F49/F53).
    void updateDuringActiveUpdateRefusesWithoutDisturbingIt();   // P-REFUSE
    void failedOrStoppedListEmitsNoListSignals();                // P-LIST-SIGNALS (neg)
    void folderReplyChildrenAreFetchedButNotReconciled();        // Folder reply children
    void uniqueReplyChildrenAreFetchedAndReconciled();           // Unique reply children
    void appliedReplySurvivesLaterFailureInMemoryAndDatastore(); // P-APPLIED

    // Items-pipeline M2, D3 (stage 1): the presentation-lane delta contract,
    // both halves — the worker's emits and the published-copy application.
    void deltaMatchesAppliedReplacement();
    void parentReplyReconcilesChildrenAgainstExpectedSet();

    // Items-pipeline M2, stage 2: the published-state contract (D6), scoped
    // per-delta pricing (D7), and the documented D2 transient.
    void publishedStateIsSnapshotPlusAppliedDeltas();
    void emptyDeltaEmptiesFetchSourceOnly();
    void reconcileErasesGhostsAcrossFailedUpdates();
    void scopedPricingConvergesToFinalPass();
    void scopedPricingIsFailSafeAcrossFailedUpdate();
    void parentBucketMayMixChildGenerationsMidRefresh();

    // Items-pipeline M2, stage 6: the D4 typed terminal event.
    void terminalEventExactlyOncePerUpdate();
    void deltasNeverFollowTerminalEvent();
    void terminalFanOutRefusesReentrantUpdate();

    // Items-pipeline M2, stage 7: the D5 skip policy — Parse failures on
    // content fetches skip and continue; everything else stays terminal.
    void parseFailureSkipsTabAndUpdateCompletes();
    void missingStashWrapperSkipsTab();
    void missingCharacterWrapperSkipsTab();

    // Folder-tree D12: worker guards for folders.
    void folderItemsAreDiscardedWithWarning();
    void misfiledFolderChildWarns();
    void folderReplyChildDisplaysUnderItsOwnTab();
    // Folder-tree A2 / R9: the checked selection's folder rule.
    void checkedSelectionIncludesFoldersOfCheckedMembers();

    // #260: the cache load and a live refresh build the same items.
    void cacheLoadAndLiveRefreshBuildTheSameItems();

    // #259: a failed update's status line names what went wrong, through the
    // real facade and hub.
    void failedUpdateStatusNamesTheError();
    void timedOutRequestSaysTheServerDidNotAnswer();
    void rateLimitedSetupDoesNotClaimNoRetryTime();

    // #279: the auto-refresh timer never notifies and never queues.
    void timerTickDuringUpdateIsDroppedQuietly();
    void timerTickWhileInitializingKeepsTheUsersQueuedRequest();
    void timerTickWhenIdleRefreshesCheckedTabs();
};

namespace {

    constexpr const char *kRealm = "pc";
    constexpr const char *kLeague = "Worker Update League";

    QString itemJson(const QString &id)
    {
        return QString(R"({
            "verified": true,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "%1",
            "name": "",
            "typeLine": "Test Item",
            "baseType": "Test Item",
            "identified": true,
            "ilvl": 1,
            "x": 0,
            "y": 0
        })")
            .arg(id);
    }

    QString itemJsonWithNote(const QString &id, const QString &note)
    {
        return QString(R"({
            "verified": true,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "%1",
            "name": "",
            "typeLine": "Test Item",
            "baseType": "Test Item",
            "identified": true,
            "ilvl": 1,
            "note": "%2",
            "x": 0,
            "y": 0
        })")
            .arg(id, note);
    }

    QByteArray stashJsonWithItems(const QString &id,
                                  const QString &name,
                                  unsigned index,
                                  const QString &items_json)
    {
        return QString(R"({"id":"%1","name":"%2","type":"PremiumStash","index":%3,"items":[%4]})")
            .arg(id, name, QString::number(index), items_json)
            .toUtf8();
    }

    QString joinItems(const QStringList &item_ids)
    {
        QStringList parts;
        for (const auto &id : item_ids) {
            parts.append(itemJson(id));
        }
        return parts.join(",");
    }

    QByteArray stashJson(const QString &id,
                         const QString &name,
                         unsigned index,
                         const std::optional<QStringList> &item_ids = {},
                         const QString &type = "PremiumStash",
                         const QString &parent = {},
                         const QList<QByteArray> &children = {})
    {
        QString json = QString(R"({"id":"%1","name":"%2","type":"%3","index":%4)")
                           .arg(id, name, type, QString::number(index));
        if (!parent.isEmpty()) {
            json += QString(R"(,"parent":"%1")").arg(parent);
        }
        if (item_ids) {
            json += QString(R"(,"items":[%1])").arg(joinItems(*item_ids));
        }
        if (!children.isEmpty()) {
            QByteArrayList list(children.cbegin(), children.cend());
            json += R"(,"children":[)" + list.join(",") + "]";
        }
        json += "}";
        return json.toUtf8();
    }

    // The JSON helpers stay because the datastore seeding path needs them.
    // The network side does not: the worker suite drives a typed facade
    // fake, so responses are domain objects, not bytes. These turn one into
    // the other so a test can express both sides the same way.
    poe::StashTab stashOf(const QByteArray &json)
    {
        const auto stash = json::readStash(json);
        if (!stash) {
            qFatal("test bug: stashOf() could not read the stash json");
        }
        return *stash;
    }

    std::vector<poe::StashTab> stashList(const QList<QByteArray> &stashes)
    {
        std::vector<poe::StashTab> list;
        for (const auto &stash : stashes) {
            list.push_back(stashOf(stash));
        }
        return list;
    }

    QByteArray characterJson(const QString &id,
                             const QString &name,
                             const std::optional<QStringList> &item_ids = {})
    {
        QString json
            = QString(
                  R"({"id":"%1","name":"%2","realm":"%3","class":"Witch","league":"%4","level":1,"experience":0)")
                  .arg(id, name, kRealm, kLeague);
        if (item_ids) {
            json += QString(R"(,"equipment":[%1])").arg(joinItems(*item_ids));
        }
        json += "}";
        return json.toUtf8();
    }

    poe::Character characterOf(const QByteArray &json)
    {
        const auto character = json::readCharacter(json);
        if (!character) {
            qFatal("test bug: characterOf() could not read the character json");
        }
        return *character;
    }

    std::vector<poe::Character> characterList(const QList<QByteArray> &characters)
    {
        std::vector<poe::Character> list;
        for (const auto &character : characters) {
            list.push_back(characterOf(character));
        }
        return list;
    }

    // Captures what the default logger writes at warn and above while in
    // scope (folder-tree D12's warnings), then restores the previous
    // default logger. Tests only: production sinks go through the main
    // logger's hub (F42) and are never touched here.
    class ScopedLogCapture
    {
    public:
        ScopedLogCapture()
            : m_previous(spdlog::default_logger())
        {
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_stream);
            sink->set_pattern("%v");
            auto logger = std::make_shared<spdlog::logger>("worker-update-capture", sink);
            logger->set_level(spdlog::level::warn);
            spdlog::set_default_logger(logger);
        }
        ~ScopedLogCapture() { spdlog::set_default_logger(m_previous); }
        ScopedLogCapture(const ScopedLogCapture &) = delete;
        ScopedLogCapture &operator=(const ScopedLogCapture &) = delete;

        // The captured lines containing every one of `needles`.
        QStringList lines(const QStringList &needles) const
        {
            QStringList matched;
            const auto all = QString::fromStdString(m_stream.str()).split('\n', Qt::SkipEmptyParts);
            for (const QString &line : all) {
                bool hit = true;
                for (const QString &needle : needles) {
                    hit = hit && line.contains(needle);
                }
                if (hit) {
                    matched.append(line);
                }
            }
            return matched;
        }

    private:
        std::ostringstream m_stream;
        std::shared_ptr<spdlog::logger> m_previous;
    };

    QStringList sortedItemIds(const Items &items)
    {
        QStringList ids;
        for (const auto &item : items) {
            ids.append(item->id());
        }
        ids.sort();
        return ids;
    }

    // A global event filter that flags any event delivered while it is
    // installed. Installed on the application object, it observes events for
    // every object — including QEvent::DeferredDelete deliveries, whose effect
    // QEventLoop::processEvents() cannot report. The drain uses it to declare
    // quiescence only when a full pass delivered nothing through EITHER the
    // processEvents phase or the deferred-delete flush.
    struct EventActivityFilter : QObject
    {
        bool active = false;
        bool eventFilter(QObject *, QEvent *) override
        {
            active = true;
            return false; // observe only; never consume
        }
    };

    // A worker wired to a fake typed facade, with the last ItemsRefreshed
    // emission captured for inspection.
    //
    // The facade's continuations are context-bound, so a delivery lands on a
    // later event-loop pass rather than inside the call. Each deliver* helper
    // names the call by domain identity (never a submission index), settles
    // it, then drains, so every assertion after it still sees the worker's
    // settled state — and if the drain were ever insufficient, the call-count
    // assertion that follows fails loudly rather than silently passing.
    struct WorkerFixture
    {
        explicit WorkerFixture(const QString &account)
            : settings(bm.tempDir.filePath("settings.ini"), QSettings::IniFormat)
            , unused_limiter(network)
            , api(unused_limiter)
        {
            settings.setValue("account", account);
            settings.setValue("realm", kRealm);
            settings.setValue("league", kLeague);
            settings.sync();
        }

        // Verification §2: an ordinary worker fixture must leave no fake call
        // pending and no worker resumption queued. Every update here runs to a
        // terminal state — a refresh emit or a failure — so once the fixture
        // goes out of scope, draining to quiescence must reach idle and leave
        // nothing awaiting a result. Draining first is essential: a fake call is
        // settled before its consumer resumes, so a bare pendingCount() could
        // read zero with a resumption still queued. The check runs while `f` is
        // still a live local inside the test method, so a leak is attributed to
        // that test.
        ~WorkerFixture()
        {
            QVERIFY(drainUntilIdle());
            QCOMPARE(api.pendingCount(), size_t(0));
            // Once every fake call has settled and the deferred sweep has drained,
            // no per-fetch handle remains (verification §2). A stopped straggler's
            // handle intentionally OUTLIVES its update — an aborted update goes
            // idle immediately, leaving the sibling suspended — but the test
            // settles it Canceled before teardown, at which point its consumer
            // resumes and the sweep reclaims it. A non-zero count here means a
            // handle leaked or a straggler was never settled.
            if (worker) {
                QCOMPARE(access().outstandingFetchTasks(), size_t(0));
            }
        }

        // The single test seam into the worker (verification §2): every
        // observation/injection goes through one friend accessor, not standing
        // *ForTest / Set* methods on the worker's production API.
        WorkerTestAccess access() const { return WorkerTestAccess(*worker); }

        // Call after seeding the datastore: the worker reads the cache on
        // construction paths keyed off the settings file's directory.
        void start()
        {
            worker = std::make_unique<ItemsManagerWorker>(settings, *bm.manager, api);
            access().setSweepObserver(&sweep);
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::ItemsRefreshed,
                             worker.get(),
                             [this](const Items &items,
                                    const std::vector<ItemLocation> &tabs,
                                    bool initial) {
                                 last_items = items;
                                 last_tabs = tabs;
                                 last_initial = initial;
                                 ++refresh_count;
                             });
            // Capture every StatusUpdate with the counter snapshot live at its
            // emission, so a test can pin monotonic progress and the single
            // terminal Ready/failure transition (P-STATUS/P-FAILURE).
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::StatusUpdate,
                             worker.get(),
                             [this](ProgramState state, const QString &message) {
                                 status_updates.push_back(
                                     {state, message, access().progressCounters()});
                             });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::NotifyUser,
                             worker.get(),
                             [this](const QString &message) { notify_messages.append(message); });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::stashListReceived,
                             worker.get(),
                             [this](const std::vector<poe::StashTab> &,
                                    const QString &,
                                    const QString &) { ++stash_list_received; });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::stashListReplaced,
                             worker.get(),
                             [this](const std::vector<poe::StashTab> &,
                                    const QString &,
                                    const QString &) { ++stash_list_replaced; });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::characterListReceived,
                             worker.get(),
                             [this](const std::vector<poe::Character> &, const QString &) {
                                 ++character_list_received;
                             });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::characterListReplaced,
                             worker.get(),
                             [this](const std::vector<poe::Character> &, const QString &) {
                                 ++character_list_replaced;
                             });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::stashChildrenReplaced,
                             worker.get(),
                             [this](const QString &parent_id,
                                    const QStringList &,
                                    const QString &,
                                    const QString &) {
                                 stash_children_replaced_parents.append(parent_id);
                             });
            // Presentation-lane delta capture (M2 D3): each record snapshots
            // the progress counters and refresh count live at emission, so a
            // test can pin "after the replace, before the counter increment,
            // before the terminal emit" without racing the handler. A shared
            // sequence number orders the two signals against each other.
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::TabRefreshed,
                             worker.get(),
                             [this](const ItemLocation &location, const Items &items) {
                                 deltas.push_back({location,
                                                   items,
                                                   refresh_count,
                                                   access().progressCounters(),
                                                   signal_seq++});
                             });
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::ChildrenReconciled,
                             worker.get(),
                             [this](const ItemLocation &parent,
                                    const std::vector<FetchSourceKey> &expected) {
                                 reconciles.push_back({parent, expected, signal_seq++});
                             });
            // The typed terminal event (M2 D4), recorded in the same signal
            // sequence as the deltas so ordering pins are direct.
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::RefreshFinished,
                             worker.get(),
                             [this](const RefreshOutcome &outcome) {
                                 terminals.push_back({outcome, refresh_count, signal_seq++});
                             });
            worker->OnRePoEReady();
        }

        // Attach an ItemsManager wired the way Application does (M2 stage 2):
        // the final snapshot always, and — unless a test wants the
        // final-pass-only baseline for comparison — the presentation-lane
        // deltas too. Call right after start(), before the initial cached
        // load emits, so the manager sees every emission of the run.
        void attachManager(bool connect_deltas = true)
        {
            manager = std::make_unique<ItemsManager>(settings, *bm.manager);
            QObject::connect(worker.get(),
                             &ItemsManagerWorker::ItemsRefreshed,
                             manager.get(),
                             &ItemsManager::OnItemsRefreshed);
            if (connect_deltas) {
                QObject::connect(worker.get(),
                                 &ItemsManagerWorker::TabRefreshed,
                                 manager.get(),
                                 &ItemsManager::OnTabRefreshed);
                QObject::connect(worker.get(),
                                 &ItemsManagerWorker::ChildrenReconciled,
                                 manager.get(),
                                 &ItemsManager::OnChildrenReconciled);
            }
            QObject::connect(manager.get(),
                             &ItemsManager::TabRefreshed,
                             manager.get(),
                             [this](const ItemLocation &, const Items &) {
                                 ++manager_delta_reemits;
                             });
            QObject::connect(manager.get(),
                             &ItemsManager::ChildrenReconciled,
                             manager.get(),
                             [this](const ItemLocation &, const std::vector<FetchSourceKey> &) {
                                 ++manager_reconcile_reemits;
                             });
        }

        // Route the manager's refresh requests to the worker the way
        // Application does. Call after attachManager().
        void connectRefreshRequests()
        {
            QObject::connect(manager.get(),
                             &ItemsManager::UpdateSignal,
                             worker.get(),
                             &ItemsManagerWorker::Update);
            QObject::connect(manager.get(),
                             &ItemsManager::AutoUpdateSignal,
                             worker.get(),
                             &ItemsManagerWorker::AutoUpdate);
        }

        QString dataDir() const { return bm.tempDir.filePath("data"); }

        // --- what the worker asked for ------------------------------------

        // A submission count (no over-fetching, no premature next request),
        // not a request identity: individual calls are addressed below by what
        // the worker asked for.
        size_t callCount() const { return api.callCount(); }

        // Identity assertions: does the worker have a call of this shape still
        // outstanding? Each folds in the kind and every distinguishing field,
        // so `hasPendingStash("stashaaaa1")` also asserts it is a top-level
        // GetStash (empty substash) for that id.
        bool hasPendingStashList() const { return api.hasPendingStashList(kRealm, kLeague); }
        bool hasPendingCharacterList() const { return api.hasPendingCharacterList(kRealm); }
        bool hasPendingStash(const QString &stash_id, const QString &substash_id = {}) const
        {
            return api.hasPendingStash(stash_id, substash_id);
        }
        bool hasPendingCharacter(const QString &name) const
        {
            return api.hasPendingCharacter(name);
        }

        // --- delivery, addressed by domain identity -----------------------

        void deliverStashList(std::vector<poe::StashTab> stashes)
        {
            api.resolveStashList(api.pendingStashList(kRealm, kLeague), std::move(stashes));
            QVERIFY(drainUntilIdle());
        }

        void deliverCharacterList(std::vector<poe::Character> characters)
        {
            api.resolveCharacterList(api.pendingCharacterList(kRealm), std::move(characters));
            QVERIFY(drainUntilIdle());
        }

        void deliverStash(const QString &stash_id, poe::StashTab stash)
        {
            api.resolveStash(api.pendingStash(stash_id), std::move(stash));
            QVERIFY(drainUntilIdle());
        }

        // A child fetch: same parent stash id, a non-empty substash id.
        void deliverChild(const QString &stash_id, const QString &substash_id, poe::StashTab stash)
        {
            api.resolveStash(api.pendingStash(stash_id, substash_id), std::move(stash));
            QVERIFY(drainUntilIdle());
        }

        void deliverCharacter(const QString &name, poe::Character character)
        {
            api.resolveCharacter(api.pendingCharacter(name), std::move(character));
            QVERIFY(drainUntilIdle());
        }

        // Settle every in-flight sibling a first-failure abort left behind, then
        // drain (verification §2, W-ER1). Under batching a terminal failure
        // returns the worker to idle immediately (D6), leaving any concurrently
        // launched request outstanding with the now-stopped shared token.
        // Production settles those through the limiter's cancellation/transfer
        // timeout; here the test settles them deterministically Canceled. Their
        // consumers resume, see the stopped token, mutate nothing, and their
        // handles are swept — so an ordinary fixture reaches zero pending at
        // teardown. Returns how many stragglers were settled so a test can pin
        // it. A clean single-fetch failure leaves none and this is a no-op.
        size_t settleStragglers()
        {
            const size_t settled = api.settleStoppedStragglers();
            if (!drainUntilIdle()) {
                qFatal("settleStragglers: the worker never reached quiescence");
            }
            return settled;
        }

        // Resolve an old update's stopped straggler with SUCCESS — the W-IDENTITY
        // case (verification §5): a successful old reply resumes while a
        // subsequent update is active. The deliver* helpers address only
        // deliverable calls and so exclude stragglers; this targets the stopped
        // slot by identity, then drains so the consumer resumes and (under correct
        // behavior) discards the reply on the stopped token.
        void resolveStragglerStash(const QString &stash_id, poe::StashTab stash)
        {
            api.resolveStash(api.stoppedStash(stash_id), std::move(stash));
            QVERIFY(drainUntilIdle());
        }

        // Fail an outstanding stash fetch. The kind is immaterial to every pin
        // here — the worker treats each non-Canceled failure the same way — so
        // these read as "this fetch died" rather than naming a transport error.
        void failStash(const QString &stash_id,
                       const QString &substash_id = {},
                       RateLimit::FetchError::Kind kind = RateLimit::FetchError::Kind::Network)
        {
            api.reject(api.pendingStash(stash_id, substash_id), kind);
            QVERIFY(drainUntilIdle());
        }

        // Fail an outstanding top-level list fetch (the stash or character
        // list), for the scenarios where a list dies while the OTHER lane's
        // content is already in flight.
        void failStashList(RateLimit::FetchError::Kind kind = RateLimit::FetchError::Kind::Network)
        {
            api.reject(api.pendingStashList(kRealm, kLeague), kind);
            QVERIFY(drainUntilIdle());
        }

        // Settle an outstanding fetch with an EXCEPTIONAL future: the worker's
        // co_await rethrows, exercising the per-fetch catch-all (W-THROW).
        void throwStash(const QString &stash_id, const QString &substash_id = {})
        {
            api.throwAt(api.pendingStash(stash_id, substash_id));
            QVERIFY(drainUntilIdle());
        }

        void throwCharacter(const QString &name)
        {
            api.throwAt(api.pendingCharacter(name));
            QVERIFY(drainUntilIdle());
        }

        // Pump queued coroutine resumptions and any deferred deletions until the
        // event loop is quiescent — a full pass that delivers nothing — or the
        // bound trips. Condition-driven on the loop emptying (not a fixed pass
        // count): it neither under-drains a multi-hop resumption nor assumes the
        // chain settles on the first turn, and it flushes deleteLater() work
        // posted outside a running loop, which processEvents alone does not.
        //
        // Quiescence is judged by a global event filter, not by
        // processEvents()'s return value: a deferred delete flushed by
        // sendPostedEvents() may run a destructor that schedules more work, and
        // processEvents() cannot report that. So each pass runs both phases with
        // the filter armed and only counts as empty when the filter saw no event
        // at all — otherwise another pass runs.
        //
        // This is what makes zero-pending meaningful at teardown: a fake call is
        // marked settled before its consumer's queued .then/co_await resumes, so
        // pendingCount() can read zero with a resumption still queued. Draining
        // to quiescence first closes that window. Returns false if the loop
        // never settled — a runaway resumption the caller surfaces via QVERIFY.
        [[nodiscard]] static bool drainUntilIdle()
        {
            constexpr int kMaxPasses = 1000;
            EventActivityFilter filter;
            QCoreApplication::instance()->installEventFilter(&filter);
            bool idle = false;
            for (int pass = 0; pass < kMaxPasses; ++pass) {
                filter.active = false;
                QEventLoop loop;
                loop.processEvents(QEventLoop::AllEvents);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                if (!filter.active) {
                    idle = true;
                    break;
                }
            }
            QCoreApplication::instance()->removeEventFilter(&filter);
            return idle;
        }

        BuyoutManagerFixture bm;
        QSettings settings;
        NetworkManager network;
        // Never called: FakePoeApiClient overrides every method. It exists
        // only to satisfy the facade's base constructor.
        RateLimiter unused_limiter;
        FakePoeApiClient api;
        // Declared before `worker` so it outlives it: the worker holds a raw
        // pointer to it and writes sweep counts through it (W-SWEEP).
        WorkerSweepObserver sweep;
        std::unique_ptr<ItemsManagerWorker> worker;

        Items last_items;
        std::vector<ItemLocation> last_tabs;
        bool last_initial{false};
        int refresh_count{0};

        // Presentation-lane delta records (M2 D3), one per emit, in order.
        struct DeltaRecord
        {
            ItemLocation location;
            Items items;
            int refresh_count_at_emit;
            WorkerTestAccess::ProgressForTest counters_at_emit;
            int seq;
        };
        struct ReconcileRecord
        {
            ItemLocation parent;
            std::vector<FetchSourceKey> expected;
            int seq;
        };
        std::vector<DeltaRecord> deltas;
        std::vector<ReconcileRecord> reconciles;
        // Terminal events (M2 D4), in the shared signal sequence.
        struct TerminalRecord
        {
            RefreshOutcome outcome;
            int refresh_count_at_emit;
            int seq;
        };
        std::vector<TerminalRecord> terminals;
        int signal_seq{0};

        // The optional published-copy consumer (M2 stage 2) and its light
        // re-emit counts. Declared after `worker` so it is destroyed first.
        std::unique_ptr<ItemsManager> manager;
        int manager_delta_reemits{0};
        int manager_reconcile_reemits{0};

        // Every NotifyUser message, in order — used to pin that a refused update
        // notifies the user (P-REFUSE).
        QStringList notify_messages;

        // Authoritative-list signal counts (P-LIST-SIGNALS): a failed or stopped
        // list must emit neither the *-received signal nor the F53 *-replaced
        // reconciliation. stash_children_replaced_parents records one parent id
        // per stashChildrenReplaced emit (Map/Unique reply reconciliation, F53).
        int stash_list_received{0};
        int stash_list_replaced{0};
        int character_list_received{0};
        int character_list_replaced{0};
        QStringList stash_children_replaced_parents;

        // Every StatusUpdate the worker emitted, with the counter snapshot live
        // when it fired. Progress messages report received/needed; the terminal
        // is a single Ready transition (success or failure).
        struct StatusRecord
        {
            ProgramState state;
            QString message;
            WorkerTestAccess::ProgressForTest counters;
        };
        std::vector<StatusRecord> status_updates;

        // How many Ready transitions have been emitted since `from` — used to pin
        // that an update produces exactly one terminal Ready (P-FAILURE).
        int readyTransitionsSince(size_t from) const
        {
            int n = 0;
            for (size_t i = from; i < status_updates.size(); ++i) {
                if (status_updates[i].state == ProgramState::Ready) {
                    ++n;
                }
            }
            return n;
        }
    };

    // Wire the worker's persistence signals to a store the way
    // Application does, including the F53 reconciliation connections.
    void connectPersistence(ItemsManagerWorker *worker, UserStore &store)
    {
        auto stashes = &store.stashes();
        auto characters = &store.characters();
        QObject::connect(worker,
                         &ItemsManagerWorker::stashListReceived,
                         stashes,
                         &StashRepo::saveStashList);
        QObject::connect(worker, &ItemsManagerWorker::stashReceived, stashes, &StashRepo::saveStash);
        QObject::connect(worker,
                         &ItemsManagerWorker::characterListReceived,
                         characters,
                         &CharacterRepo::saveCharacterList);
        QObject::connect(worker,
                         &ItemsManagerWorker::characterReceived,
                         characters,
                         &CharacterRepo::saveCharacter);
        QObject::connect(worker,
                         &ItemsManagerWorker::stashListReplaced,
                         stashes,
                         &StashRepo::reconcileStashList);
        QObject::connect(worker,
                         &ItemsManagerWorker::characterListReplaced,
                         characters,
                         &CharacterRepo::reconcileCharacterList);
        QObject::connect(worker,
                         &ItemsManagerWorker::stashChildrenReplaced,
                         stashes,
                         &StashRepo::reconcileStashChildren);
    }

    QStringList storedStashIds(UserStore &store)
    {
        QStringList ids;
        for (const auto &stash : store.stashes().getStashList(kRealm, kLeague)) {
            ids.append(stash.id);
        }
        ids.sort();
        return ids;
    }

} // namespace

// A terminal failure mid-update must lose nothing: items already replaced
// stay replaced, items whose fetch never landed stay cached (F28).
void WorkerUpdateTest::failedUpdateKeepsItemsIntact()
{
    WorkerFixture f("worker-update-account-1");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1", "a2"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-1");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QVERIFY(f.last_initial);
    QCOMPARE(f.last_items.size(), size_t(3));

    const std::vector<poe::StashTab> fresh_list = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)});

    // A full update launches both lists at once (D6): neither is held behind
    // the other.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStashList());
    QVERIFY(f.hasPendingCharacterList());

    // The stash list's whole content batch — both tabs — goes out at once, not
    // one request at a time.
    f.deliverStashList(fresh_list);
    QCOMPARE(f.callCount(), size_t(4));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.deliverCharacterList({});
    QCOMPARE(f.callCount(), size_t(4)); // the empty character list adds no content

    // Tab A lands with a new set of items...
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1",
                                     "Tab A",
                                     0,
                                     QStringList{"a1-new", "a2-new", "a3-new"})));

    // ...then tab B's fetch dies. It was the only sibling still in flight, so
    // the update terminates with nothing left stranded and no emit.
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1);

    // A follow-up update of tab A alone succeeds and shows that the failed
    // update lost nothing: tab B's cached item is still present.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_a)});
    QCOMPARE(f.callCount(), size_t(5));
    f.deliverStashList(fresh_list);

    QCOMPARE(f.callCount(), size_t(6));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1",
                                     "Tab A",
                                     0,
                                     QStringList{"a1-new", "a2-new", "a3-new"})));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1-new", "a2-new", "a3-new", "b1"}));
}

// A terminal failure ends the update losing nothing, and the next update
// starts from a clean slate and succeeds — no failure count or in-flight work
// carries across the boundary (F28).
//
// This test used to also pin the stale-reply half of F28 by delivering the
// same recorded reply three times — once while the worker was idle, once
// mid-update-2 — which the legacy signal boundary allowed. That half is no
// longer reachable through this single-tab scenario: each fetch settles
// exactly once, and this update never has a sibling in flight when it aborts.
// The cross-update straggler the batch worker CAN produce — a stopped old
// request that resumes while a new update is active — is pinned separately by
// W-ER1 and (in 5D) W-IDENTITY. See the F28 entry in docs/cleanup/findings.md.
void WorkerUpdateTest::failedUpdateDoesNotLeakIntoTheNext()
{
    WorkerFixture f("worker-update-account-2");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    QVERIFY(tab_a);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-2");
        QVERIFY(store.stashes().saveStashList({*tab_a}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.last_items.size(), size_t(1));

    const ItemLocation loc_a(*tab_a);
    const std::vector<poe::StashTab> fresh_list = stashList({stashJson("stashaaaa1", "Tab A", 0)});

    // Update 1: the item fetch for tab A fails, terminating the update.
    f.worker->Update(TabSelection::Selected, {loc_a});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(fresh_list);
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    f.failStash("stashaaaa1");
    QCOMPARE(f.refresh_count, 1);

    // Nothing from update 1 is left in flight, and the failure did not
    // disturb the cached items.
    QCOMPARE(f.callCount(), size_t(2));
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1"}));

    // Update 2 starts clean: the failure count from update 1 must not carry
    // over, or it would abort update 2 with no emit.
    f.worker->Update(TabSelection::Selected, {loc_a});
    QCOMPARE(f.callCount(), size_t(3));
    f.deliverStashList(fresh_list);
    QCOMPARE(f.callCount(), size_t(4));
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1-after"})));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1-after"}));
}

// A partial update that includes a character must fetch only the selected
// characters and keep exactly one tab entry per character. The old
// skip-check compared names against an id-keyed index and never matched,
// so every character in the league was re-added and re-fetched (F48).
void WorkerUpdateTest::partialUpdateDoesNotDuplicateCharacters()
{
    WorkerFixture f("worker-update-account-3");

    const auto char_1 = json::readCharacter(
        characterJson("charid0001", "CharOne", QStringList{"c1-item"}));
    const auto char_2 = json::readCharacter(
        characterJson("charid0002", "CharTwo", QStringList{"c2-item"}));
    QVERIFY(char_1 && char_2);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-3");
        QVERIFY(store.characters().saveCharacterList({*char_1, *char_2}));
        QVERIFY(saveCharacterFixture(store.characters(), *char_1));
        QVERIFY(saveCharacterFixture(store.characters(), *char_2));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.last_tabs.size(), size_t(2));
    QCOMPARE(f.last_items.size(), size_t(2));

    // Refresh only CharOne. The fresh list mentions both characters, with
    // CharTwo renamed in-game (same id, new name).
    f.worker->Update(TabSelection::Selected, {ItemLocation(*char_1, 0)});
    QCOMPARE(f.callCount(), size_t(1));
    QVERIFY(f.hasPendingCharacterList());
    f.deliverCharacterList(characterList(
        {characterJson("charid0001", "CharOne"), characterJson("charid0002", "CharTwoRenamed")}));

    // Only the selected character is fetched.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingCharacter("CharOne"));
    f.deliverCharacter("CharOne",
                       characterOf(
                           characterJson("charid0001", "CharOne", QStringList{"c1-item-new"})));

    QCOMPARE(f.refresh_count, 2);

    // Exactly one tab entry per character, and CharTwo's cached item intact.
    QCOMPARE(f.last_tabs.size(), size_t(2));
    QStringList tab_ids;
    for (const auto &tab : f.last_tabs) {
        tab_ids.append(tab.id());
    }
    tab_ids.sort();
    QCOMPARE(tab_ids, QStringList({"charid0001", "charid0002"}));
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"c1-item-new", "c2-item"}));

    // The unfetched character's surviving item carries the fresh name —
    // forum codes and sort order read the item's embedded location.
    const auto item_c2 = std::find_if(f.last_items.cbegin(),
                                      f.last_items.cend(),
                                      [](const std::shared_ptr<Item> &item) {
                                          return item->id() == "c2-item";
                                      });
    QVERIFY(item_c2 != f.last_items.cend());
    QCOMPARE((*item_c2)->location().character(), QString("CharTwoRenamed"));
}

// Tab-list reconciliation gives every listed tab fresh metadata, even tabs
// outside the update selection: a tab renamed or moved in-game updates its
// label and position on any partial refresh, with no extra fetch (the M1
// behavior change that absorbed the F15 sketch), and its cached items are
// kept. The surviving items' embedded locations are rebased too — search
// buckets are built from item locations, so a stale embedded copy would
// show the old name (and a moved tab would bucket twice, once per index).
void WorkerUpdateTest::renamedTabMetadataRefreshesWithoutFetch()
{
    WorkerFixture f("worker-update-account-4");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Old Name", 0, QStringList{"a1"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-4");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.last_items.size(), size_t(2));

    // Refresh only tab B; the fresh list shows tab A renamed and moved
    // in-game (index 0 -> 2).
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_b)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(
        stashList({stashJson("stashbbbb1", "Tab B", 1), stashJson("stashaaaa1", "New Name", 2)}));

    // Only tab B is fetched; the rename costs no extra request.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.deliverStash("stashbbbb1",
                   stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1-new"})));

    QCOMPARE(f.refresh_count, 2);

    // Tab A carries the new label and position, and its cached item survived.
    const auto renamed = std::find_if(f.last_tabs.cbegin(),
                                      f.last_tabs.cend(),
                                      [](const ItemLocation &tab) {
                                          return tab.id() == "stashaaaa1";
                                      });
    QVERIFY(renamed != f.last_tabs.cend());
    QCOMPARE(renamed->tab_label(), QString("New Name"));
    QCOMPARE(renamed->tab_index(), 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "b1-new"}));

    // The surviving item's embedded location was rebased to match.
    const auto item_a = std::find_if(f.last_items.cbegin(),
                                     f.last_items.cend(),
                                     [](const std::shared_ptr<Item> &item) {
                                         return item->id() == "a1";
                                     });
    QVERIFY(item_a != f.last_items.cend());
    QCOMPARE((*item_a)->location().tab_label(), QString("New Name"));
    QCOMPARE((*item_a)->location().tab_index(), 2);
}

// A parent's reply is authoritative for its children: cached items fetched
// from a Map/Unique child the parent no longer lists must be removed when
// the parent is refreshed. Without this reconciliation they would survive
// every refresh, even a full one — the per-reply replacement only erases
// fetch ids it is about to re-fetch, and the tab-list cleanup keys on the
// display id, which for child items is the (still-listed) parent.
void WorkerUpdateTest::vanishedMapChildItemsAreRemovedOnParentRefresh()
{
    WorkerFixture f("worker-update-account-5");
    f.settings.setValue("get_map_stashes", true);

    // Cached state: a MapStash whose child holds one item. Child stashes
    // are cached under their own ids with a parent reference, and are not
    // part of the stash list (matching the live API).
    const auto parent_tab = json::readStash(
        stashJson("mapstash01", "Maps", 0, QStringList{"p1"}, "MapStash"));
    const auto child_tab = json::readStash(
        stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01"));
    QVERIFY(parent_tab && child_tab);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-5");
        QVERIFY(store.stashes().saveStashList({*parent_tab}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *parent_tab, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *child_tab, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"m1", "p1"}));
    QCOMPARE(f.last_tabs.size(), size_t(1));

    // Refresh the map stash. Its reply lists a different child: the old
    // child's cached item must go, the new child's contents arrive.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*parent_tab)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(stashList({stashJson("mapstash01", "Maps", 0, {}, "MapStash")}));

    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("mapstash01"));
    f.deliverStash(
        "mapstash01",
        stashOf(stashJson("mapstash01",
                          "Maps",
                          0,
                          QStringList{"p1-new"},
                          "MapStash",
                          {},
                          {stashJson("mapchild02", "Tier 2", 0, {}, "MapStash", "mapstash01")})));

    // The new child is fetched by its own id...
    QCOMPARE(f.callCount(), size_t(3));
    QVERIFY(f.hasPendingStash("mapstash01", "mapchild02"));
    f.deliverChild(
        "mapstash01",
        "mapchild02",
        stashOf(stashJson("mapchild02", "Tier 2", 0, QStringList{"m2"}, "MapStash", "mapstash01")));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"m2", "p1-new"}));

    // ...and its items display under the parent tab.
    const auto item_m2 = std::find_if(f.last_items.cbegin(),
                                      f.last_items.cend(),
                                      [](const std::shared_ptr<Item> &item) {
                                          return item->id() == "m2";
                                      });
    QVERIFY(item_m2 != f.last_items.cend());
    QCOMPARE((*item_m2)->location().id(), QString("mapstash01"));
    QCOMPARE((*item_m2)->location().fetch_id(), QString("mapchild02"));
}

// The emitted Items share Item objects with ItemsManager and the UI, so
// rebasing surviving item locations may only happen when an update
// finishes successfully (in FinishUpdate, just before the emit). Rebasing
// at list receipt would mutate the already-published snapshot mid-update —
// and a terminal failure would leave it mutated indefinitely, with no emit
// to rebuild the search buckets around the new metadata.
void WorkerUpdateTest::failedUpdateDoesNotRebasePublishedLocations()
{
    WorkerFixture f("worker-update-account-6");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Old Name", 0, QStringList{"a1"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-6");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Hold the published item the way ItemsManager and the UI do: a
    // shared_ptr into the emitted snapshot.
    std::shared_ptr<Item> item_a;
    for (const auto &item : f.last_items) {
        if (item->id() == "a1") {
            item_a = item;
        }
    }
    QVERIFY(item_a);

    const std::vector<poe::StashTab> fresh_list = stashList(
        {stashJson("stashaaaa1", "New Name", 0), stashJson("stashbbbb1", "Tab B", 1)});

    // Refresh only tab B; the fresh list renames tab A, then tab B's fetch
    // dies, terminating the update without an emit.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_b)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(fresh_list);
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1);

    // The published snapshot is untouched by the failed update.
    QCOMPARE(item_a->location().tab_label(), QString("Old Name"));

    // A subsequent successful update applies the rename to the same shared
    // object the UI holds.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_b)});
    QCOMPARE(f.callCount(), size_t(3));
    f.deliverStashList(fresh_list);
    QCOMPARE(f.callCount(), size_t(4));
    f.deliverStash("stashbbbb1",
                   stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1-new"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(item_a->location().tab_label(), QString("New Name"));
}

// A partial refresh fetches only the tabs it was asked to. A tab that was
// listed but whose contents were never fetched is left untouched by a
// selection that excludes it, so a partial refresh never balloons into a
// full one (F55, revised). A full refresh still fetches it.
void WorkerUpdateTest::partialRefreshSkipsNeverFetchedTabs()
{
    WorkerFixture f("worker-update-account-7");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    const auto tab_n = json::readStash(stashJson("stashnnnn1", "New Tab", 1));
    QVERIFY(tab_a && tab_n);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-7");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_n}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        // tab N: listed, never fetched.
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.last_tabs.size(), size_t(2));
    QCOMPARE(f.last_items.size(), size_t(1));

    const std::vector<poe::StashTab> stash_list = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashnnnn1", "New Tab", 1)});

    // Refresh only tab A: tab N is left alone even though its contents were
    // never fetched — a partial refresh touches only its selection.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_a)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(stash_list);
    // Only tab A is fetched; the never-fetched tab N is not pulled in.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(!f.hasPendingStash("stashnnnn1"));
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    QCOMPARE(f.refresh_count, 2);
    // Tab N is still empty.
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1"}));

    // A full refresh does fetch the never-fetched tab.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(4)); // + stash list + character list
    QVERIFY(f.hasPendingStashList());
    f.deliverStashList(stash_list);
    QCOMPARE(f.callCount(), size_t(6)); // both tabs launch
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("stashnnnn1"));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    f.deliverStash("stashnnnn1", stashOf(stashJson("stashnnnn1", "New Tab", 1, QStringList{"n1"})));
    QCOMPARE(f.refresh_count, 3);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "n1"}));
}

// Selecting a Map/Unique parent in a partial refresh fetches the parent and
// its reply-discovered children, while a known, unselected tab is left alone.
// Children are never top-level list entries, so they ride the parent's fetch
// decision — a partial refresh that reaches a special stash still reaches its
// children (F55, revised).
void WorkerUpdateTest::partialRefreshFetchesChildrenOfSelectedMapParent()
{
    WorkerFixture f("worker-update-account-8");
    f.settings.setValue("get_map_stashes", true);

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    const auto map_parent = json::readStash(
        stashJson("mapstash01", "Maps", 1, QStringList{"p1"}, "MapStash"));
    QVERIFY(tab_a && map_parent);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-8");
        QVERIFY(store.stashes().saveStashList({*tab_a, *map_parent}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *map_parent, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const std::vector<poe::StashTab> stash_list = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("mapstash01", "Maps", 1, {}, "MapStash")});
    const poe::StashTab map_parent_body = stashOf(
        stashJson("mapstash01",
                  "Maps",
                  1,
                  QStringList{"p1"},
                  "MapStash",
                  {},
                  {stashJson("mapchild01", "Tier 1", 0, {}, "MapStash", "mapstash01")}));

    // Select only the Map parent.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*map_parent)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverStashList(stash_list);
    // Only the selected Map parent is fetched; the known, unselected tab A is not.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStash("mapstash01"));
    QVERIFY(!f.hasPendingStash("stashaaaa1"));
    // The parent's reply discovers its child, which is fetched even though the
    // child was never in the selection — it rides the parent's fetch.
    f.deliverStash("mapstash01", map_parent_body);
    QCOMPARE(f.callCount(), size_t(3));
    QVERIFY(f.hasPendingStash("mapstash01", "mapchild01"));
    f.deliverChild(
        "mapstash01",
        "mapchild01",
        stashOf(stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01")));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "m1", "p1"}));
}

// End-to-end F53: with the persistence wiring in place, a fresh top-level
// list deletes the rows of server-deleted tabs (a surviving Map child row
// is untouched — it is never in any list), and a Map parent's reply
// replaces its child rows.
void WorkerUpdateTest::datastoreFollowsServerDeletions()
{
    WorkerFixture f("worker-update-account-9");
    f.settings.setValue("get_map_stashes", true);

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    const auto map_parent = json::readStash(
        stashJson("mapstash01", "Maps", 2, QStringList{"p1"}, "MapStash"));
    const auto map_child = json::readStash(
        stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01"));
    QVERIFY(tab_a && tab_b && map_parent && map_child);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-9");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b, *map_parent}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *map_parent, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *map_child, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "b1", "m1", "p1"}));

    UserStore store(QDir(f.dataDir()), "worker-update-account-9");
    connectPersistence(f.worker.get(), store);

    // Server-side, Tab B was deleted; the fresh list omits it.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2)); // both lists
    f.deliverStashList(stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("mapstash01", "Maps", 1, {}, "MapStash")}));

    // Tab A and the Map parent launch as one stash content batch; the character
    // list is still outstanding.
    QCOMPARE(f.callCount(), size_t(4));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("mapstash01"));
    // Tab B's row is gone at list receipt; the Map child row survives.
    QCOMPARE(storedStashIds(store), QStringList({"mapchild01", "mapstash01", "stashaaaa1"}));

    f.deliverCharacterList({});
    QCOMPARE(f.callCount(), size_t(4)); // the empty character list adds no content
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));

    // The Map parent's reply lists a different child: the old child row is
    // replaced in the datastore and the new child launches.
    f.deliverStash(
        "mapstash01",
        stashOf(stashJson("mapstash01",
                          "Maps",
                          1,
                          QStringList{"p1"},
                          "MapStash",
                          {},
                          {stashJson("mapchild02", "Tier 2", 0, {}, "MapStash", "mapstash01")})));
    QCOMPARE(storedStashIds(store), QStringList({"mapstash01", "stashaaaa1"}));

    QCOMPARE(f.callCount(), size_t(5));
    QVERIFY(f.hasPendingStash("mapstash01", "mapchild02"));
    f.deliverChild(
        "mapstash01",
        "mapchild02",
        stashOf(stashJson("mapchild02", "Tier 2", 0, QStringList{"m2"}, "MapStash", "mapstash01")));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(storedStashIds(store), QStringList({"mapchild02", "mapstash01", "stashaaaa1"}));
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "m2", "p1"}));
}

// End-to-end F53 for characters: a fresh character list deletes the rows
// of server-deleted characters.
void WorkerUpdateTest::datastoreFollowsCharacterDeletions()
{
    WorkerFixture f("worker-update-account-10");

    const auto char_1 = json::readCharacter(
        characterJson("charid0001", "CharOne", QStringList{"c1-item"}));
    const auto char_2 = json::readCharacter(
        characterJson("charid0002", "CharTwo", QStringList{"c2-item"}));
    QVERIFY(char_1 && char_2);
    {
        UserStore store(QDir(f.dataDir()), "worker-update-account-10");
        QVERIFY(store.characters().saveCharacterList({*char_1, *char_2}));
        QVERIFY(saveCharacterFixture(store.characters(), *char_1));
        QVERIFY(saveCharacterFixture(store.characters(), *char_2));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.last_items.size(), size_t(2));

    UserStore store(QDir(f.dataDir()), "worker-update-account-10");
    connectPersistence(f.worker.get(), store);

    // Server-side, CharTwo was deleted; the fresh list omits it.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*char_1, 0)});
    QCOMPARE(f.callCount(), size_t(1));
    f.deliverCharacterList(characterList({characterJson("charid0001", "CharOne")}));

    QStringList ids;
    for (const auto &character : store.characters().getCharacterList(kRealm)) {
        ids.append(character.id);
    }
    QCOMPARE(ids, QStringList({"charid0001"}));

    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingCharacter("CharOne"));
    f.deliverCharacter("CharOne",
                       characterOf(characterJson("charid0001", "CharOne", QStringList{"c1-item"})));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"c1-item"}));
}

// The straggler-settle path: stopped calls are settled Canceled in bulk,
// exactly once, while live calls are untouched and remain deliverable. The
// finders exclude stopped stragglers, so an active update never delivers to
// one. Driven directly against the fake to pin these harness semantics in
// isolation (the worker-level use is pinned by W-ER1/W-IDENTITY).
void WorkerUpdateTest::fakeSettlesStoppedStragglersCanceledExactlyOnce()
{
    NetworkManager network;
    RateLimiter limiter(network);
    FakePoeApiClient api(limiter);

    std::stop_source dead_a;
    std::stop_source dead_bob;
    std::stop_source live;
    dead_a.request_stop();
    dead_bob.request_stop();

    // Two stopped stragglers from an aborted update and one live call from the
    // next. The returned futures are retained so the settlement outcome — not
    // just the counts — is pinned.
    auto fa = api.getStash(kRealm, kLeague, "A", {}, dead_a.get_token());
    auto fbob = api.getCharacter(kRealm, "Bob", dead_bob.get_token());
    auto fc = api.getStash(kRealm, kLeague, "C", {}, live.get_token());

    QCOMPARE(api.callCount(), size_t(3));
    QCOMPARE(api.pendingCount(), size_t(3));
    QCOMPARE(api.stoppedStragglerCount(), size_t(2));

    // Only the live call is deliverable; the stopped stragglers are not.
    QVERIFY(api.hasPendingStash("C"));
    QVERIFY(!api.hasPendingStash("A"));
    QVERIFY(!api.hasPendingCharacter("Bob"));

    // Nothing is settled yet — every future is still outstanding.
    QVERIFY(!fa.isFinished());
    QVERIFY(!fbob.isFinished());
    QVERIFY(!fc.isFinished());

    // Settling the stragglers Canceled leaves only the live call pending.
    QCOMPARE(api.settleStoppedStragglers(), size_t(2));
    QCOMPARE(api.pendingCount(), size_t(1));
    QCOMPARE(api.stoppedStragglerCount(), size_t(0));

    // Each straggler finished with exactly one Canceled result — the outcome,
    // not merely a count. A Network settlement here would fail the test.
    QVERIFY(fa.isFinished());
    QCOMPARE(fa.resultCount(), 1);
    QVERIFY(!fa.result().has_value());
    QCOMPARE(fa.result().error().kind, RateLimit::FetchError::Kind::Canceled);
    QVERIFY(fbob.isFinished());
    QCOMPARE(fbob.resultCount(), 1);
    QVERIFY(!fbob.result().has_value());
    QCOMPARE(fbob.result().error().kind, RateLimit::FetchError::Kind::Canceled);

    // The live call was untouched by the sweep and is still outstanding.
    QVERIFY(!fc.isFinished());

    // A second sweep finds nothing to settle — no double-settle qFatal.
    QCOMPARE(api.settleStoppedStragglers(), size_t(0));

    // The live call is still deliverable and settles to a value on demand.
    QVERIFY(api.hasPendingStash("C"));
    api.resolveStash(api.pendingStash("C"), stashOf(stashJson("C", "Tab C", 0, QStringList{"c1"})));
    QCOMPARE(api.pendingCount(), size_t(0));
    QVERIFY(fc.isFinished());
    QCOMPARE(fc.resultCount(), 1);
    QVERIFY(fc.result().has_value());
}

// A stopped old-update straggler and a live new-update call may share a domain
// identity — a legitimate cross-update overlap that must NOT trip the F49
// duplicate tripwire. The finder resolves to the single live call; the
// straggler settles Canceled independently.
void WorkerUpdateTest::fakeTreatsCrossUpdateIdentityOverlapAsLive()
{
    NetworkManager network;
    RateLimiter limiter(network);
    FakePoeApiClient api(limiter);

    std::stop_source dead;
    std::stop_source live;
    dead.request_stop();

    // Both fetch stash "X": the first is an aborted update's straggler, the
    // second the new update's live request. Futures retained to pin outcomes.
    auto f_straggler = api.getStash(kRealm, kLeague, "X", {}, dead.get_token());
    auto f_live = api.getStash(kRealm, kLeague, "X", {}, live.get_token());
    QCOMPARE(api.pendingCount(), size_t(2));

    // Exactly one *deliverable* "X" exists, so the finder does not qFatal as an
    // F49 duplicate — it selects the live call, and the straggler stays put.
    QVERIFY(api.hasPendingStash("X"));
    api.resolveStash(api.pendingStash("X"), stashOf(stashJson("X", "Tab X", 0, QStringList{"x1"})));

    // The live call — and only it — finished with a value; the straggler is
    // still outstanding, not collateral of the delivery.
    QVERIFY(f_live.isFinished());
    QVERIFY(f_live.result().has_value());
    QVERIFY(!f_straggler.isFinished());

    // The straggler is still owed a settlement; the sweep clears it Canceled.
    QCOMPARE(api.pendingCount(), size_t(1));
    QCOMPARE(api.stoppedStragglerCount(), size_t(1));
    QCOMPARE(api.settleStoppedStragglers(), size_t(1));
    QCOMPARE(api.pendingCount(), size_t(0));
    QVERIFY(f_straggler.isFinished());
    QCOMPARE(f_straggler.resultCount(), 1);
    QVERIFY(!f_straggler.result().has_value());
    QCOMPARE(f_straggler.result().error().kind, RateLimit::FetchError::Kind::Canceled);
}

// W-TOKEN: every list/content call in one update carries the same non-default
// stop_token; a terminal failure stops that token; the next update carries a
// distinct, unstopped one. This is what makes the token the update's identity
// (the post-await check discards a straggler whose token was stopped). 5B keeps
// the generation guard alongside — W-IDENTITY (5D) proves the token replaces it.
// This pin covers the list + stash-content + failure-stops + fresh-token
// lifecycle; character-content and reply-discovered child calls sharing the same
// token are pinned in stagedBatchesRunConcurrentlyPreservingLaneOrder, where all
// four call kinds are concurrently in flight under one token.
void WorkerUpdateTest::everyCallInAnUpdateSharesOneTokenAndTheNextUpdateGetsAFreshOne()
{
    WorkerFixture f("wtoken");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const auto s1 = stashJson("stok1", "Tab S1", 0);

    f.worker->Update(TabSelection::All);
    // Both lists are issued up front under one update token that can be stopped
    // (non-default) and has not been stopped yet. Addressed by identity.
    QCOMPARE(f.callCount(), size_t(2));
    const std::stop_token token = f.api.tokenForStashList(kRealm, kLeague);
    QVERIFY(token.stop_possible());
    QVERIFY(!token.stop_requested());
    QVERIFY(f.api.tokenForCharacterList(kRealm) == token); // the character list shares it

    f.deliverStashList(stashList({s1}));
    QCOMPARE(f.callCount(), size_t(3)); // + content fetch for stok1
    // The content fetch shares the same update token — the same object identity,
    // not merely an equal value.
    QVERIFY(f.api.tokenForStash("stok1") == token);
    f.deliverCharacterList({});
    QCOMPARE(f.callCount(), size_t(3)); // the empty character list adds no content

    // A terminal failure stops that shared token (first-failure stop).
    QVERIFY(f.hasPendingStash("stok1"));
    f.failStash("stok1");
    QVERIFY(token.stop_requested());
    QCOMPARE(f.refresh_count, 1);

    // The next update gets a fresh source: its token can still be stopped, has
    // not been, and is a different token from the stopped one — and both its
    // lists share that new token.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(5));
    const std::stop_token token2 = f.api.tokenForStashList(kRealm, kLeague);
    QVERIFY(token2.stop_possible());
    QVERIFY(!token2.stop_requested());
    QVERIFY(token2 != token);
    QVERIFY(f.api.tokenForCharacterList(kRealm) == token2);

    // Run the second update to a clean terminal state so teardown finds nothing
    // pending.
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    f.deliverStash("stok1", stashOf(stashJson("stok1", "Tab S1", 0, QStringList{"i1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-INIT (success): a first list whose future is already finished runs its
// continuation INLINE during the launch loop (S1-6). It must not finalize early
// or corrupt the counters that were initialized before the launch — the update
// proceeds and completes normally.
void WorkerUpdateTest::readyFirstListRunsInlineWithoutCorruptingCounters()
{
    WorkerFixture f("winit-ok");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Arm the stash list to come back already-finished with one tab.
    f.api.preresolveStashList(stashList({stashJson("sready1", "Tab R1", 0)}));

    f.worker->Update(TabSelection::All);
    // The ready stash list ran inline during launch: it processed its tab and
    // launched that tab's content batch immediately (D6, not held behind the
    // character list), then RunUpdate issued the character list. The needed
    // counter was initialized before that inline content launch, so nothing
    // finalized early or corrupted the run.
    QCOMPARE(f.callCount(), size_t(3)); // stash list + sready1 content + character list
    QVERIFY(f.hasPendingCharacterList());
    QVERIFY(f.hasPendingStash("sready1"));
    QCOMPARE(f.refresh_count, 1);

    f.deliverCharacterList({});
    f.deliverStash("sready1", stashOf(stashJson("sready1", "Tab R1", 0, QStringList{"r1"})));

    // The inline-ready first list did not corrupt the run: exactly one fresh
    // refresh with the expected item.
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"r1"}));
}

// W-INIT (error): a first list whose future is already finished with a FAILURE
// runs inline during launch and aborts the update cleanly — no early emit. The
// character list is still issued (D6: every required list is launched even when
// an earlier ready failure has already stopped the token), so it comes back a
// stopped straggler that settles Canceled and mutates nothing.
void WorkerUpdateTest::readyFirstListErrorAbortsLeavingTheSecondListAStraggler()
{
    WorkerFixture f("winit-err");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.api.prerejectStashList(RateLimit::FetchError::Kind::Network);

    f.worker->Update(TabSelection::All);
    // Both lists were launched. The stash list failed inline and stopped the
    // shared token before the update went idle; the character list still went
    // out and now carries that stopped token — a straggler, not deliverable. No
    // content, no emit.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(!f.hasPendingCharacterList());
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1));
    QCOMPARE(f.refresh_count, 1);

    // Settling the straggler Canceled mutates nothing and leaves no emit; the
    // worker is already idle.
    QCOMPARE(f.settleStragglers(), size_t(1));
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.pendingCount(), size_t(0));

    // The worker returned to idle: a fresh update is accepted and completes,
    // proving the inline failure did not wedge anything.
    const auto s1 = stashJson("sready1", "Tab R1", 0);
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    f.deliverStash("sready1", stashOf(stashJson("sready1", "Tab R1", 0, QStringList{"r1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-THROW: an exceptional facade future (an IR4 boundary violation) is caught by
// the per-fetch task's root catch-all, which turns it into an ordinary failure:
// the first-failure stop fires and the update aborts to idle through the direct
// terminal path instead of wedging. The failed fetch is NOT counted as received
// — success reconciles the counters, failure aborts directly and leaves them
// untouched (P-STATUS) — so this pins the abort, not a counter move.
void WorkerUpdateTest::exceptionalFetchIsCaughtAndAbortsTheUpdate()
{
    WorkerFixture f("wthrow");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const auto s1 = stashJson("sthrow1", "Tab T1", 0);

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    QCOMPARE(f.callCount(), size_t(3));
    QVERIFY(f.hasPendingStash("sthrow1"));
    const std::stop_token token = f.api.tokenForStash("sthrow1"); // captured while pending

    // The content fetch's future rethrows at co_await.
    f.throwStash("sthrow1");

    // No new refresh (the update aborted), and the shared token was stopped by
    // the first-failure path the catch-all fed into.
    QCOMPARE(f.refresh_count, 1);
    QVERIFY(token.stop_requested());

    // The failed fetch is NOT counted as received: failure takes the direct
    // terminal abort and leaves the counters untouched (P-STATUS). The one tab
    // was needed but never received.
    QCOMPARE(f.access().progressCounters().stashes_needed, size_t(1));
    QCOMPARE(f.access().progressCounters().stashes_received, size_t(0));

    // Not wedged: the worker is idle again and a fresh update completes.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    f.deliverStash("sthrow1", stashOf(stashJson("sthrow1", "Tab T1", 0, QStringList{"t1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-THROW (handler body, stopped-but-active): an exception from inside a fetch
// continuation is contained by the per-fetch task's WHOLE-body catch-all, which
// takes the direct terminal AbortUpdate() path. The fault fires from a failure
// handler AFTER it has already stopped the update token but BEFORE it reaches its
// own AbortUpdate() — so the catch-all must NOT gate on the token: it aborts
// unconditionally. A token-guarded catch would misread this still-active update
// as already cancelled, skip the abort, and wedge. Two things make the
// unconditional abort safe: AbortUpdate() is idempotent and WorkerState-guarded,
// and a genuinely stale straggler never reaches the handler at all (the
// post-await token gate discards it before the try body runs), so the only throw
// that can reach this catch belongs to the active update. This single pin proves
// the catch contains the throw, a stopped-but-active update still aborts to Idle,
// and recovery succeeds. Uses the injected fault hook — never Qt's undefined
// slot-throwing behavior.
void WorkerUpdateTest::handlerExceptionAbortsToIdleInsteadOfWedging()
{
    WorkerFixture f("whandlerthrow");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const auto s1 = stashJson("shthrow1", "Tab H1", 0);

    // Drive to the content stage with the hook unarmed (so RunUpdate does not
    // consume it), then arm it: the content failure's handler stops the token
    // and hits the fault site before setting its completion flags.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    QVERIFY(f.hasPendingStash("shthrow1"));

    f.access().setFaultHook([] { throw std::runtime_error("failure handler exploded"); });
    f.failStash("shthrow1");

    // Contained and forced terminal: no refresh, no leaked handle, no wedge.
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.access().outstandingFetchTasks(), size_t(0));

    // Recovery reaches Idle: a fresh update is accepted and completes.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    f.deliverStash("shthrow1", stashOf(stashJson("shthrow1", "Tab H1", 0, QStringList{"h1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-THROW (root orchestration): the root orchestration's own body has a catch-all
// distinct from the per-fetch tasks' (verification §3). A throw in orchestration
// itself — before any list is launched — is contained there and drives the same
// terminal AbortUpdate(), rather than escaping into the caller (ItemsManager /
// the UI). Recovery proves the worker reached Idle.
void WorkerUpdateTest::rootOrchestrationExceptionAbortsToIdle()
{
    WorkerFixture f("wrootthrow");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Armed before Update(): RunUpdate() hits the root fault site at the top of
    // its body, before launching any list.
    f.access().setFaultHook([] { throw std::runtime_error("orchestration exploded"); });
    f.worker->Update(TabSelection::All);

    // Aborted before any request was issued, and it did not escape or wedge.
    QCOMPARE(f.callCount(), size_t(0));
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.access().outstandingFetchTasks(), size_t(0));

    // Recovery reaches Idle.
    const auto s1 = stashJson("srthrow1", "Tab R1", 0);
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({s1}));
    f.deliverCharacterList({});
    f.deliverStash("srthrow1", stashOf(stashJson("srthrow1", "Tab R1", 0, QStringList{"r1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-SWEEP: each per-fetch completion schedules a deferred sweep that runs on a
// later event-loop turn (via a queued invocation — outside the completing
// coroutine) and destroys only finished handles. Across a full update every
// launched per-fetch handle is reclaimed and none is left live.
void WorkerUpdateTest::completionsScheduleADeferredSweepThatReclaimsFinishedHandles()
{
    WorkerFixture f("wsweep");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // The observer starts clean: the initial parse launches no fetches.
    QCOMPARE(f.sweep.scheduled, 0);
    QCOMPARE(f.sweep.executed, 0);

    const auto s1 = stashJson("ssweep1", "Tab W1", 0);

    f.worker->Update(TabSelection::All);

    // The stash-list completion launches its content batch and schedules one
    // deferred sweep. That sweep destroys the finished stash-list handle but
    // PRESERVES the two suspended handles it left in flight — the still-pending
    // character list and the just-launched content fetch. Only ready handles
    // are reclaimed; destroying a suspended one would detach, not stop, it.
    f.deliverStashList(stashList({s1}));
    QCOMPARE(f.sweep.executed, 1);
    QCOMPARE(f.sweep.destroyed, 1);
    QCOMPARE(f.sweep.live_after, size_t(2)); // character list + content, both suspended
    QCOMPARE(f.access().outstandingFetchTasks(), size_t(2));

    // The character-list completion schedules another sweep (a later completion
    // always schedules its own), which reclaims the character-list handle and
    // leaves the suspended content handle live.
    f.deliverCharacterList({});
    QCOMPARE(f.sweep.executed, 2);
    QCOMPARE(f.sweep.destroyed, 2);
    QCOMPARE(f.sweep.live_after, size_t(1));

    // The final content completion sweeps the last handle; nothing is left live.
    f.deliverStash("ssweep1", stashOf(stashJson("ssweep1", "Tab W1", 0, QStringList{"w1"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.sweep.scheduled, 3);
    QCOMPARE(f.sweep.executed, 3);
    QCOMPARE(f.sweep.destroyed, 3);
    QCOMPARE(f.sweep.live_after, size_t(0));
    QCOMPARE(f.access().outstandingFetchTasks(), size_t(0));
}

// W-F56 headline (verification §3): the batch worker submits both lists and each
// lane's complete content batch without one-at-a-time serialization. One scenario
// covers W-F56-LISTS (both lists up front), W-F56-CONTENT (each list's whole
// content batch; character work in flight while stash work is outstanding),
// W-F56-CHILDREN (folder children join the list batch; reply-discovered Map
// children launch as one batch), and W-F56-ORDER (deliberately non-sorted
// identities keep source traversal order within every lane — stash, character,
// AND the reply-discovered child lane — with no global lane order asserted). It
// also carries W-TOKEN's content and child coverage: every list, content, and
// child call in the update shares the one update token. It FAILS if
// SubmitNextItemRequest-style serialization returns: a serial worker would leave
// only one content fetch in flight at a time, so the simultaneous-pending and
// lane-order assertions below could not all hold.
void WorkerUpdateTest::stagedBatchesRunConcurrentlyPreservingLaneOrder()
{
    WorkerFixture f("wf56");
    f.settings.setValue("get_map_stashes", true);
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // A stash list in deliberately non-sorted order: s_zzz1 (sorts LAST) is
    // listed first, a folder with two nested children sits in the middle, and a
    // Map stash is last. A serial or sorted worker would not launch these in
    // this order.
    const std::vector<poe::StashTab> stashes = stashList(
        {stashJson("s_zzz1", "Zzz Tab", 5),
         stashJson("s_aaa1", "Aaa Tab", 2),
         stashJson("fold01",
                   "My Folder",
                   0,
                   {},
                   "Folder",
                   {},
                   {stashJson("fchild1", "Child One", 0, {}, "PremiumStash", "fold01"),
                    stashJson("fchild2", "Child Two", 1, {}, "PremiumStash", "fold01")}),
         stashJson("map01", "Maps", 9, {}, "MapStash")});
    const std::vector<poe::Character> characters = characterList(
        {characterJson("zedid00001", "Zed"), characterJson("annid00001", "Ann")});

    // A full update issues both lists at once.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStashList());
    QVERIFY(f.hasPendingCharacterList());

    // Capture the update token while both lists are pending; W-TOKEN below
    // asserts every list, content, and child call in this update shares it.
    const std::stop_token token = f.api.tokenForStashList(kRealm, kLeague);
    QVERIFY(token.stop_possible());
    QVERIFY(!token.stop_requested());
    QVERIFY(f.api.tokenForCharacterList(kRealm) == token);

    // The stash list's whole content batch goes out at once: the two plain tabs,
    // the folder, its two recursively discovered children (which join this list
    // batch), and the Map parent — six fetches, none held behind another.
    f.deliverStashList(stashes);
    QCOMPARE(f.callCount(), size_t(8));
    QVERIFY(f.hasPendingStash("s_zzz1"));
    QVERIFY(f.hasPendingStash("s_aaa1"));
    QVERIFY(f.hasPendingStash("fold01"));
    QVERIFY(f.hasPendingStash("fchild1"));
    QVERIFY(f.hasPendingStash("fchild2"));
    QVERIFY(f.hasPendingStash("map01"));

    // W-F56-ORDER: within the stash lane the launch order is the list's source
    // traversal order (folder children immediately after their folder), NOT the
    // ids' sorted order — s_zzz1 sorts last but launched first. Addressed by the
    // lane's own submission order, never a global call index.
    QCOMPARE(f.api.stashFetchOrder(),
             QStringList({"s_zzz1", "s_aaa1", "fold01", "fchild1", "fchild2", "map01"}));

    // W-TOKEN (stash content): every content fetch carries the same update token
    // the lists did — same object identity, not merely an equal value.
    QVERIFY(f.api.tokenForStash("s_zzz1") == token);
    QVERIFY(f.api.tokenForStash("fchild1") == token);

    // The character list's content batch launches from its own handler — while
    // every stash fetch above is still outstanding. This is the cross-lane
    // concurrency F56 is about: character work is not queued behind stash work.
    f.deliverCharacterList(characters);
    QCOMPARE(f.callCount(), size_t(10));
    QVERIFY(f.hasPendingCharacter("Zed"));
    QVERIFY(f.hasPendingCharacter("Ann"));
    QVERIFY(f.hasPendingStash("s_zzz1")); // stash lane still outstanding meanwhile
    QVERIFY(f.hasPendingStash("map01"));
    // Character lane keeps its own source order (Zed before Ann, though Ann sorts
    // first). No order is asserted BETWEEN the lanes.
    QCOMPARE(f.api.characterFetchOrder(), QStringList({"Zed", "Ann"}));

    // W-TOKEN (character content): the character content batch carries the same
    // update token, proving it is not just the stash lane that shares identity.
    QVERIFY(f.api.tokenForCharacter("Zed") == token);
    QVERIFY(f.api.tokenForCharacter("Ann") == token);

    // Deliver the whole population in a deliberately scrambled order — arbitrary
    // completion order is supported.
    f.deliverStash("s_aaa1", stashOf(stashJson("s_aaa1", "Aaa Tab", 2, QStringList{"a1"})));
    f.deliverCharacter("Ann", characterOf(characterJson("annid00001", "Ann", QStringList{"ann1"})));
    f.deliverStash("fchild2", stashOf(stashJson("fchild2", "Child Two", 1, QStringList{"fc2"})));

    // The Map parent's reply discovers two children in deliberately non-sorted
    // source order (mc2 before mc1); they launch as one complete child batch
    // (W-F56-CHILDREN), not one at a time.
    f.deliverStash("map01",
                   stashOf(stashJson("map01",
                                     "Maps",
                                     9,
                                     QStringList{"mp1"},
                                     "MapStash",
                                     {},
                                     {stashJson("mc2", "Tier 2", 1, {}, "MapStash", "map01"),
                                      stashJson("mc1", "Tier 1", 0, {}, "MapStash", "map01")})));
    QCOMPARE(f.callCount(), size_t(12));
    QVERIFY(f.hasPendingStash("map01", "mc1"));
    QVERIFY(f.hasPendingStash("map01", "mc2"));

    // W-F56-ORDER (child lane): the reply-discovered child batch keeps its source
    // traversal order, addressed by parent/substash identity. Non-sorted on
    // purpose — a sorted or serialized launch would flip it to {mc1, mc2} here.
    QCOMPARE(f.api.substashFetchOrder("map01"), QStringList({"mc2", "mc1"}));

    // W-TOKEN (child): the reply-discovered children share the one update token
    // too, so every list, content, AND child call in this update has one identity.
    QVERIFY(f.api.tokenForStash("map01", "mc1") == token);
    QVERIFY(f.api.tokenForStash("map01", "mc2") == token);

    f.deliverStash("s_zzz1", stashOf(stashJson("s_zzz1", "Zzz Tab", 5, QStringList{"z1"})));
    f.deliverStash("fold01", stashOf(stashJson("fold01", "My Folder", 0, {}, "Folder")));
    f.deliverCharacter("Zed", characterOf(characterJson("zedid00001", "Zed", QStringList{"zed1"})));
    f.deliverStash("fchild1", stashOf(stashJson("fchild1", "Child One", 0, QStringList{"fc1"})));
    f.deliverChild("map01",
                   "mc2",
                   stashOf(stashJson("mc2", "Tier 2", 1, QStringList{"m2"}, "MapStash", "map01")));
    // Still one child outstanding: the update has not finished.
    QCOMPARE(f.refresh_count, 1);
    f.deliverChild("map01",
                   "mc1",
                   stashOf(stashJson("mc1", "Tier 1", 0, QStringList{"m1"}, "MapStash", "map01")));

    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items),
             QStringList({"a1", "ann1", "fc1", "fc2", "m1", "m2", "mp1", "z1", "zed1"}));
}

// W-ER1 (verification §3): two worker fetches are in flight under one shared
// token; one fails terminally, which stops the token and returns the worker to
// idle immediately, leaving the sibling as a straggler. When the sibling settles
// Canceled its consumer resumes, sees the stopped token, and returns without
// running the handler — it mutates no state, emits nothing, causes no second
// terminal transition, and adds no failure that could corrupt the next update.
void WorkerUpdateTest::stoppedSiblingResumesCanceledAndMutatesNothing()
{
    WorkerFixture f("wer1");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "wer1");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "b1"}));

    const std::vector<poe::StashTab> fresh = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)});

    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    f.deliverStashList(fresh);
    // Both tab contents are in flight at once under the same update token.
    QCOMPARE(f.callCount(), size_t(4));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.deliverCharacterList({});
    const std::stop_token token = f.api.tokenForStash("stashaaaa1");
    QVERIFY(token.stop_possible());
    QVERIFY(f.api.tokenForStash("stashbbbb1") == token); // the sibling shares the token

    // Tab A's fetch fails: first-failure stops the shared token and the update
    // goes idle at once, leaving tab B's fetch in flight as a straggler.
    f.failStash("stashaaaa1");
    QVERIFY(token.stop_requested());
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1));

    const QStringList before = sortedItemIds(f.last_items);

    // The sibling settles Canceled and resumes: no emit, no state change, no
    // second terminal transition.
    QCOMPARE(f.settleStragglers(), size_t(1));
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(sortedItemIds(f.last_items), before);

    // The straggler carried no failure or corruption into the next update, which
    // completes cleanly.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1-new"})));
    f.deliverStash("stashbbbb1",
                   stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1-new"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1-new", "b1-new"}));
}

// W-ER1 (list lane): the character list succeeds and launches its content while
// the stash list is still outstanding; then the stash list fails. First-failure
// must terminate the update immediately (D6): the list-failure branch takes the
// direct terminal AbortUpdate() path, which returns the worker to Idle at once
// regardless of the completion counters, abandoning the in-flight character
// content as a stopped straggler. A design that instead waited for
// CheckUpdateFinished's success-equality would hang here — the abandoned
// character content is never received — which is exactly why the failure path is
// counter-independent (P-STATUS/P-FAILURE). This pins that the worker is not
// wedged: it accepts and completes a fresh update afterward.
void WorkerUpdateTest::listFailureWithSiblingContentInFlightTerminatesCleanly()
{
    WorkerFixture f("wer1-list");

    const auto char_1 = json::readCharacter(
        characterJson("charid0001", "CharOne", QStringList{"c1"}));
    QVERIFY(char_1);
    {
        UserStore store(QDir(f.dataDir()), "wer1-list");
        QVERIFY(store.characters().saveCharacterList({*char_1}));
        QVERIFY(saveCharacterFixture(store.characters(), *char_1));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    // The character list arrives first and launches its content while the stash
    // list is still in flight.
    f.deliverCharacterList(characterList({characterJson("charid0001", "CharOne")}));
    QCOMPARE(f.callCount(), size_t(3));
    QVERIFY(f.hasPendingCharacter("CharOne"));
    QVERIFY(f.hasPendingStashList());

    // Now the stash list fails. The update must return to idle at once, leaving
    // the character content as a stopped straggler — not wedge on it.
    f.failStashList();
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1)); // the character content
    QCOMPARE(f.settleStragglers(), size_t(1));

    // Not wedged: a fresh update is accepted and completes.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(5)); // + both lists of the recovery update
    f.deliverStashList({});
    f.deliverCharacterList(characterList({characterJson("charid0001", "CharOne")}));
    f.deliverCharacter("CharOne",
                       characterOf(characterJson("charid0001", "CharOne", QStringList{"c1-new"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"c1-new"}));
}

// W-INIT (content batch): both content fetches of a stash batch come back
// already-finished and run inline during the launch loop. Because the whole
// batch's needed counter is initialized before the first launch, the first
// inline completion cannot finalize the update on its own — only after BOTH
// complete does it finish, with both tabs' items. The character list is
// delivered first so both lists are already done when the ready batch launches,
// which is exactly the ordering under which an incrementally initialized counter
// would finalize early after only one tab.
void WorkerUpdateTest::readyContentBatchInitializesCountsBeforeInlineCompletion()
{
    WorkerFixture f("winit-content");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.api.preresolveStash(stashOf(stashJson("sc_a", "Tab A", 0, QStringList{"a1"})));
    f.api.preresolveStash(stashOf(stashJson("sc_b", "Tab B", 1, QStringList{"b1"})));

    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    f.deliverCharacterList({}); // both lists done before the ready batch launches
    f.deliverStashList(stashList({stashJson("sc_a", "Tab A", 0), stashJson("sc_b", "Tab B", 1)}));

    // Both ready fetches ran inline; the update finished exactly once, with both.
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "b1"}));
}

// W-INIT (content batch, inline error): the first fetch of a stash batch comes
// back already-failed. The inline failure stops the shared token and returns the
// worker to idle, but the whole initialized batch must still be launched — the
// second fetch goes out carrying the stopped token and becomes a straggler. A
// launch loop that bailed on the inline failure would never issue it.
void WorkerUpdateTest::readyContentErrorStillLaunchesTheRestOfTheBatch()
{
    WorkerFixture f("winit-content-err");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.api.prerejectStash(RateLimit::FetchError::Kind::Network);

    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    f.deliverStashList(stashList({stashJson("sc_a", "Tab A", 0), stashJson("sc_b", "Tab B", 1)}));

    // Both content fetches were issued even though the first failed inline: sc_a
    // is settled (ready-error), sc_b is a stopped straggler.
    QCOMPARE(f.callCount(), size_t(4));
    QCOMPARE(f.refresh_count, 1);
    QVERIFY(!f.hasPendingStash("sc_a"));
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1));

    QCOMPARE(f.settleStragglers(), size_t(1));
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.pendingCount(), size_t(0));

    // Recovery: the inline failure did not wedge the worker.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("sc_a", "Tab A", 0), stashJson("sc_b", "Tab B", 1)}));
    f.deliverCharacterList({});
    f.deliverStash("sc_a", stashOf(stashJson("sc_a", "Tab A", 0, QStringList{"a1"})));
    f.deliverStash("sc_b", stashOf(stashJson("sc_b", "Tab B", 1, QStringList{"b1"})));
    QCOMPARE(f.refresh_count, 2);
}

// W-INIT (child batch): a Map parent's reply discovers two children and one of
// them comes back already-finished, running inline during the child batch launch.
// Because the child batch's count is tallied into the completion total before the
// launch, the inline child completion does not finalize the update early — it
// finishes only after the second child arrives, with both children's items.
void WorkerUpdateTest::readyChildCompletesInlineWithoutCorruptingParentBookkeeping()
{
    WorkerFixture f("winit-child");
    f.settings.setValue("get_map_stashes", true);

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    QVERIFY(tab_a);
    {
        UserStore store(QDir(f.dataDir()), "winit-child");
        QVERIFY(store.stashes().saveStashList({*tab_a}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("map01", "Maps", 1, {}, "MapStash")}));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));

    // Arm the first child to come back already-finished, then deliver the parent:
    // its reply launches the [mc1, mc2] child batch and mc1 completes inline.
    f.api.preresolveStash(
        stashOf(stashJson("mc1", "Tier 1", 0, QStringList{"m1"}, "MapStash", "map01")));
    f.deliverStash("map01",
                   stashOf(stashJson("map01",
                                     "Maps",
                                     1,
                                     QStringList{"mp1"},
                                     "MapStash",
                                     {},
                                     {stashJson("mc1", "Tier 1", 0, {}, "MapStash", "map01"),
                                      stashJson("mc2", "Tier 2", 1, {}, "MapStash", "map01")})));

    // mc1 ran inline, but the update is NOT finished: mc2 is still outstanding.
    QCOMPARE(f.refresh_count, 1);
    QVERIFY(!f.hasPendingStash("map01", "mc1")); // ready, settled inline
    QVERIFY(f.hasPendingStash("map01", "mc2"));

    f.deliverChild("map01",
                   "mc2",
                   stashOf(stashJson("mc2", "Tier 2", 1, QStringList{"m2"}, "MapStash", "map01")));

    // Only now does the update finish, with the parent's and both children's items.
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "m1", "m2", "mp1"}));
}

// W-INIT (child batch, inline error): the FIRST Map child comes back
// already-failed during the child batch launch. Like the content-batch error
// case, the whole initialized child stage must still be launched — the second
// child goes out carrying the stopped token — and the parent must stay
// retryable (its child cycle never completed), with exactly one failure
// transition.
void WorkerUpdateTest::readyChildErrorStillLaunchesTheRestAndParentStaysRetryable()
{
    WorkerFixture f("winit-child-err");
    f.settings.setValue("get_map_stashes", true);

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    QVERIFY(tab_a);
    {
        UserStore store(QDir(f.dataDir()), "winit-child-err");
        QVERIFY(store.stashes().saveStashList({*tab_a}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const std::vector<poe::StashTab> stash_list = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("map01", "Maps", 1, {}, "MapStash")});
    const poe::StashTab parent_body = stashOf(
        stashJson("map01",
                  "Maps",
                  1,
                  QStringList{"mp1"},
                  "MapStash",
                  {},
                  {stashJson("mc1", "Tier 1", 0, {}, "MapStash", "map01"),
                   stashJson("mc2", "Tier 2", 1, {}, "MapStash", "map01")}));

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stash_list);
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));

    const size_t mark = f.status_updates.size();
    // Arm the first child to fail already-finished, then deliver the parent: its
    // reply launches the [mc1, mc2] child batch and mc1 fails inline.
    f.api.prerejectStash(RateLimit::FetchError::Kind::Network);
    f.deliverStash("map01", parent_body);

    // The whole child stage still launched: mc1 is settled (ready-error), mc2 was
    // submitted with the stopped token and is a straggler. One failure transition.
    QVERIFY(!f.hasPendingStash("map01", "mc1"));
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1)); // mc2
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.readyTransitionsSince(mark), 1);
    QCOMPARE(f.settleStragglers(), size_t(1));

    // The parent's child cycle failed. Refetch it by including it in the
    // selection (a partial refresh reaches only its selection — new/incomplete
    // tabs are no longer auto-fetched, F55 revised): both children now succeed.
    const auto map_parent = json::readStash(stashJson("map01", "Maps", 1, {}, "MapStash"));
    QVERIFY(map_parent);
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_a), ItemLocation(*map_parent)});
    f.deliverStashList(stash_list);
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("map01"));
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    f.deliverStash("map01", parent_body);
    QVERIFY(f.hasPendingStash("map01", "mc1"));
    QVERIFY(f.hasPendingStash("map01", "mc2"));
    f.deliverChild("map01",
                   "mc1",
                   stashOf(stashJson("mc1", "Tier 1", 0, QStringList{"m1"}, "MapStash", "map01")));
    f.deliverChild("map01",
                   "mc2",
                   stashOf(stashJson("mc2", "Tier 2", 1, QStringList{"m2"}, "MapStash", "map01")));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "m1", "m2", "mp1"}));
}

// P-STATUS / P-FAILURE: a mid-batch failure must not run progress backward. With
// a three-tab content batch (needed=3) reporting 0/3 then 1/3, the first failure
// takes the direct terminal path — it does NOT count the failed fetch as received
// or snap `needed` down to fake CheckUpdateFinished's success equality — so the
// reported counters stay monotonic, and the failed update emits exactly one
// terminal Ready transition.
void WorkerUpdateTest::failureKeepsProgressMonotonicWithOneTerminalTransition()
{
    WorkerFixture f("wmono");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    const auto tab_c = json::readStash(stashJson("stashcccc1", "Tab C", 2, QStringList{"c1"}));
    QVERIFY(tab_a && tab_b && tab_c);
    {
        UserStore store(QDir(f.dataDir()), "wmono");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b, *tab_c}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_c, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const std::vector<poe::StashTab> fresh = stashList({stashJson("stashaaaa1", "Tab A", 0),
                                                        stashJson("stashbbbb1", "Tab B", 1),
                                                        stashJson("stashcccc1", "Tab C", 2)});

    const size_t mark = f.status_updates.size();
    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh); // needed = 3, the whole batch launches
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    // Tab B fails while tab C is still in flight — the first failure with the
    // batch total (3) far above the received count (1).
    f.failStash("stashbbbb1");

    // Progress never ran backward across the update's status updates: neither
    // received nor needed decreased, and needed never dropped below received.
    size_t prev_received = 0;
    size_t prev_needed = 0;
    for (size_t i = mark; i < f.status_updates.size(); ++i) {
        const auto &c = f.status_updates[i].counters;
        QVERIFY(c.stashes_received >= prev_received);
        QVERIFY(c.stashes_needed >= prev_needed);
        QVERIFY(c.stashes_needed >= c.stashes_received);
        prev_received = c.stashes_received;
        prev_needed = c.stashes_needed;
    }
    // The batch total was reported at least once and never shrank to fake equality.
    QVERIFY(prev_needed >= 3);
    // Exactly one terminal Ready transition for the failed update (P-FAILURE).
    QCOMPARE(f.readyTransitionsSince(mark), 1);
    QCOMPARE(f.refresh_count, 1);

    // Tab C was still in flight when B failed; settle it Canceled.
    QCOMPARE(f.settleStragglers(), size_t(1));
}

// P-SELECTION (TabsOnly): a TabsOnly update requests BOTH lists (like All) but
// fetches no tab or character content — the lists refresh metadata only. Under
// batching this means each list handler's content batch is empty, and the update
// finishes on the two list receipts alone.
void WorkerUpdateTest::tabsOnlyRequestsBothListsButNoContent()
{
    WorkerFixture f("tabsonly");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Old Name", 0, QStringList{"a1"}));
    const auto char_1 = json::readCharacter(
        characterJson("charid0001", "CharOne", QStringList{"c1"}));
    QVERIFY(tab_a && char_1);
    {
        UserStore store(QDir(f.dataDir()), "tabsonly");
        QVERIFY(store.stashes().saveStashList({*tab_a}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(store.characters().saveCharacterList({*char_1}));
        QVERIFY(saveCharacterFixture(store.characters(), *char_1));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::TabsOnly);
    // Both lists are requested, exactly as a full update.
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStashList());
    QVERIFY(f.hasPendingCharacterList());

    // Each list refreshes metadata (tab A renamed) but launches no content.
    f.deliverStashList(stashList({stashJson("stashaaaa1", "New Name", 0)}));
    f.deliverCharacterList(characterList({characterJson("charid0001", "CharOne")}));

    // No content fetch was ever issued, and the update finished on the lists
    // alone with the fresh metadata applied.
    QCOMPARE(f.callCount(), size_t(2));
    QCOMPARE(f.refresh_count, 2);
    const auto renamed = std::find_if(f.last_tabs.cbegin(),
                                      f.last_tabs.cend(),
                                      [](const ItemLocation &tab) {
                                          return tab.id() == "stashaaaa1";
                                      });
    QVERIFY(renamed != f.last_tabs.cend());
    QCOMPARE(renamed->tab_label(), QString("New Name"));
    // The cached items survived untouched (no content refetch).
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a1", "c1"}));
}

// W-IDENTITY (verification §5): with the generation guard deleted, the update
// token is the update's SOLE identity. A failed update stops its token and goes
// idle immediately, leaving an in-flight sibling as a straggler that still owes
// a settlement. If that straggler later settles SUCCESSFULLY while a SUBSEQUENT
// update is active, its consumer must discard the reply on the stopped token and
// touch nothing belonging to the new update.
//
// The observable is constructed (verification §5) so the old success WOULD
// corrupt the new update if it were processed: the new update fetches the same
// tab, needs exactly that one content fetch, and the straggler carries a
// different item set. Under correct behavior the straggler resolves with no emit
// and no counter movement; the new update then finishes with its OWN item, the
// stale item nowhere in the result. Mutation proof: bypassing the post-await
// `!token.stop_requested()` gate in the shared ProcessIfLive helper makes the
// straggler finalize the new update early with the stale item, and this test
// fails. Every fetch coroutine routes through that one helper, so this stash
// variant and the character variant below regression-cover the single gate for
// all four fetch kinds.
void WorkerUpdateTest::oldSuccessfulStragglerDoesNotCorruptASubsequentUpdate()
{
    WorkerFixture f("widentity");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b-old"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "widentity");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-old", "b-old"}));

    const std::vector<poe::StashTab> fresh = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)});

    // Update 1 (All): both tab contents go in flight under token1; then tab B
    // fails. First-failure stops token1 and the worker goes idle at once, leaving
    // tab A's fetch as a stopped straggler that still owes a settlement.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    const std::stop_token token1 = f.api.tokenForStash("stashaaaa1");
    f.failStash("stashbbbb1");
    QVERIFY(token1.stop_requested());
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1)); // tab A's fetch

    // Update 2 (Selected tab A): a fresh, unstopped token2. Only tab A is
    // fetched — tab B is a known tab and unselected, so nothing needs its
    // content. The stash list carries both tabs; its content batch is just A.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*tab_a)});
    f.deliverStashList(fresh);
    QVERIFY(f.hasPendingStash("stashaaaa1")); // the NEW A fetch (token2), deliverable
    const std::stop_token token2 = f.api.tokenForStash("stashaaaa1");
    QVERIFY(token2 != token1);
    QVERIFY(!token2.stop_requested());
    // Two GetStash calls for A now coexist: the stopped straggler (token1) and
    // the new deliverable call (token2). The identity finder settles them apart.
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1));
    QCOMPARE(f.access().progressCounters().stashes_needed, size_t(1));

    // The OLD straggler resolves SUCCESSFULLY, carrying a stale item set. On the
    // stopped token1 its consumer discards the reply: no emit, no counter
    // movement, no premature finish of the active update 2.
    f.resolveStragglerStash("stashaaaa1",
                            stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-STALE"})));
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.access().progressCounters().stashes_received, size_t(0));
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(0)); // the straggler is now settled

    // Update 2 then finishes on its OWN reply (token2), with tab A's fresh item
    // and tab B's untouched cache. The stale item the straggler carried is
    // nowhere in the result.
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-new"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-new", "b-old"}));
}

// W-IDENTITY (character lane): the same proof as the stash variant above, but the
// straggler is a character content fetch (FetchCharacter). Each fetch coroutine
// routes its handler through the one shared post-await gate (ProcessIfLive), so
// pinning a second lane confirms the character coroutine really does gate before
// touching state — the single gate is regression-covered from both content lanes,
// not just the stash one. The stoppedCharacter() fake finder settles the old
// character straggler with SUCCESS by identity.
void WorkerUpdateTest::oldSuccessfulCharacterStragglerDoesNotCorruptASubsequentUpdate()
{
    WorkerFixture f("widentity-char");

    const auto char_1 = json::readCharacter(
        characterJson("charid0001", "CharOne", QStringList{"c1-old"}));
    const auto char_2 = json::readCharacter(
        characterJson("charid0002", "CharTwo", QStringList{"c2-old"}));
    QVERIFY(char_1 && char_2);
    {
        UserStore store(QDir(f.dataDir()), "widentity-char");
        QVERIFY(store.characters().saveCharacterList({*char_1, *char_2}));
        QVERIFY(saveCharacterFixture(store.characters(), *char_1));
        QVERIFY(saveCharacterFixture(store.characters(), *char_2));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"c1-old", "c2-old"}));

    const std::vector<poe::Character> fresh = characterList(
        {characterJson("charid0001", "CharOne"), characterJson("charid0002", "CharTwo")});

    // Update 1 (All): both character contents go in flight under token1; then
    // CharTwo fails. First-failure stops token1 and the worker goes idle at once,
    // leaving CharOne's fetch a stopped straggler.
    f.worker->Update(TabSelection::All);
    f.deliverStashList({});
    f.deliverCharacterList(fresh);
    QVERIFY(f.hasPendingCharacter("CharOne"));
    QVERIFY(f.hasPendingCharacter("CharTwo"));
    const std::stop_token token1 = f.api.tokenForCharacter("CharOne");
    f.api.reject(f.api.pendingCharacter("CharTwo"), RateLimit::FetchError::Kind::Network);
    QVERIFY(WorkerFixture::drainUntilIdle());
    QVERIFY(token1.stop_requested());
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1)); // CharOne's fetch

    // Update 2 (Selected CharOne): a fresh, unstopped token2. Only CharOne is
    // fetched — CharTwo is a known character and unselected.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*char_1, 0)});
    f.deliverCharacterList(fresh);
    QVERIFY(f.hasPendingCharacter("CharOne")); // the NEW fetch (token2), deliverable
    const std::stop_token token2 = f.api.tokenForCharacter("CharOne");
    QVERIFY(token2 != token1);
    QVERIFY(!token2.stop_requested());
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1));
    QCOMPARE(f.access().progressCounters().characters_needed, size_t(1));

    // The OLD character straggler resolves SUCCESSFULLY with a stale item set. On
    // the stopped token1 its consumer discards the reply: no emit, no counter
    // movement, no premature finish of update 2.
    f.api.resolveCharacter(f.api.stoppedCharacter("CharOne"),
                           characterOf(
                               characterJson("charid0001", "CharOne", QStringList{"c1-STALE"})));
    QVERIFY(WorkerFixture::drainUntilIdle());
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(f.access().progressCounters().characters_received, size_t(0));
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(0));

    // Update 2 finishes on its OWN reply (token2), with CharOne's fresh item and
    // CharTwo's untouched cache. The stale item is nowhere in the result.
    f.deliverCharacter("CharOne",
                       characterOf(characterJson("charid0001", "CharOne", QStringList{"c1-new"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"c1-new", "c2-old"}));
}

// P-REFUSE (verification §4): an Update() issued while an initialized update is
// already active is refused — it notifies the user, submits nothing, and neither
// stops, replaces, nor queues behind the running update. (Deferral while the
// worker is still initializing is a separate branch, exercised elsewhere.)
void WorkerUpdateTest::updateDuringActiveUpdateRefusesWithoutDisturbingIt()
{
    WorkerFixture f("prefuse");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    QVERIFY(tab_a);
    {
        UserStore store(QDir(f.dataDir()), "prefuse");
        QVERIFY(store.stashes().saveStashList({*tab_a}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const std::vector<poe::StashTab> fresh = stashList({stashJson("stashaaaa1", "Tab A", 0)});

    // Update 1 goes active with tab A's content in flight under a live token.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    const std::stop_token token = f.api.tokenForStash("stashaaaa1");
    const size_t calls_before = f.callCount();
    const int notifies_before = int(f.notify_messages.size());

    // A second Update() while the first is active is refused: notify, submit
    // nothing, do not stop or replace the active token/update.
    f.worker->Update(TabSelection::All);
    QVERIFY(WorkerFixture::drainUntilIdle());
    QCOMPARE(f.notify_messages.size(), notifies_before + 1);
    QVERIFY(f.notify_messages.last().contains("in progress"));
    QCOMPARE(f.callCount(), calls_before); // nothing submitted
    QVERIFY(!token.stop_requested());      // active token untouched
    QCOMPARE(f.refresh_count, 1);          // refusal did not finalize anything

    // The original update completes normally on its own token — the refusal left
    // it entirely intact.
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-new"})));
    QCOMPARE(f.refresh_count, 2);
    QVERIFY(!token.stop_requested());
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-new"}));
}

// P-LIST-SIGNALS negative half (verification §4): the authoritative-list signals
// (list-received and the F53 *-replaced reconciliation) fire only for a
// successful fresh list processed while its update is active. A list that FAILS
// emits neither; and a list left STOPPED by a first-failure abort emits neither
// when its straggler resumes, because the post-await token gate discards it
// before the handler runs.
void WorkerUpdateTest::failedOrStoppedListEmitsNoListSignals()
{
    WorkerFixture f("plistsignals-neg");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Update All launches both lists under one shared token.
    f.worker->Update(TabSelection::All);
    QCOMPARE(f.callCount(), size_t(2));
    QVERIFY(f.hasPendingStashList());
    QVERIFY(f.hasPendingCharacterList());

    const int sr = f.stash_list_received;
    const int srr = f.stash_list_replaced;
    const int cr = f.character_list_received;
    const int crr = f.character_list_replaced;

    // The stash list FAILS: its handler returns before emitting either the
    // received signal or the stashListReplaced reconciliation. First-failure
    // stops the shared token, leaving the character list a stopped straggler.
    f.failStashList();
    QCOMPARE(f.stash_list_received, sr);
    QCOMPARE(f.stash_list_replaced, srr);
    QCOMPARE(f.api.stoppedStragglerCount(), size_t(1)); // the character list

    // The STOPPED character list resumes: gated out on the stopped token, its
    // handler never runs, so no character-list signal fires either.
    QCOMPARE(f.settleStragglers(), size_t(1));
    QCOMPARE(f.character_list_received, cr);
    QCOMPARE(f.character_list_replaced, crr);
    QCOMPARE(f.refresh_count, 1); // a failed update emits no refresh
}

// Reply-discovered children on a FOLDER parent (verification §4). The generic
// reply path accepts children on all three parent types; this pins the Folder
// case. NOTE: a live Folder reply carries no children — folder children arrive
// in the top-level list (F49) — so this exercises the worker's DEFENSIVE
// reply-child path on a Folder parent, not a shape the live API produces. A
// Folder reply drives no F53 child reconciliation (stashChildrenReplaced is
// emitted only for Map/Unique parents; folder child rows follow the top list).
void WorkerUpdateTest::folderReplyChildrenAreFetchedButNotReconciled()
{
    WorkerFixture f("folder-reply-children");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Update All. The stash list carries a single Folder tab (no list children);
    // the folder's reply then defensively carries a child.
    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    f.deliverStashList(stashList({stashJson("folder0001", "Folder", 0, {}, "Folder")}));
    QVERIFY(f.hasPendingStash("folder0001"));

    const int reconciled_before = int(f.stash_children_replaced_parents.size());

    f.deliverStash("folder0001",
                   stashOf(stashJson(
                       "folder0001",
                       "Folder",
                       0,
                       {},
                       "Folder",
                       {},
                       {stashJson("folderchild1", "Sub", 0, {}, "PremiumStash", "folder0001")})));

    // The reply-discovered child is fetched as its own batch by its own id...
    QVERIFY(f.hasPendingStash("folder0001", "folderchild1"));
    // ...but a Folder reply drives NO child reconciliation.
    QCOMPARE(int(f.stash_children_replaced_parents.size()), reconciled_before);

    f.deliverChild("folder0001",
                   "folderchild1",
                   stashOf(stashJson("folderchild1",
                                     "Sub",
                                     0,
                                     QStringList{"fc1"},
                                     "PremiumStash",
                                     "folder0001")));
    QCOMPARE(f.refresh_count, 2);
    QVERIFY(sortedItemIds(f.last_items).contains("fc1"));
}

// Reply-discovered children on a UNIQUE parent (verification §4). Like the Map
// reply-child path, a Unique reply's children (with get_unique_stashes enabled)
// launch as one complete batch, and the Unique reply IS authoritative for its
// children, so it drives one F53 stashChildrenReplaced reconciliation.
void WorkerUpdateTest::uniqueReplyChildrenAreFetchedAndReconciled()
{
    WorkerFixture f("unique-reply-children");
    f.settings.setValue("get_unique_stashes", true);
    f.settings.sync();
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    f.deliverStashList(stashList({stashJson("unique0001", "Uniques", 0, {}, "UniqueStash")}));
    QVERIFY(f.hasPendingStash("unique0001"));

    const int reconciled_before = int(f.stash_children_replaced_parents.size());

    f.deliverStash(
        "unique0001",
        stashOf(stashJson("unique0001",
                          "Uniques",
                          0,
                          {},
                          "UniqueStash",
                          {},
                          {stashJson("uniquechild1", "Set", 0, {}, "UniqueStash", "unique0001")})));

    // The Unique reply's child launches as its own batch...
    QVERIFY(f.hasPendingStash("unique0001", "uniquechild1"));
    // ...and a Unique reply drives exactly one child reconciliation for the
    // parent (F53).
    QCOMPARE(int(f.stash_children_replaced_parents.size()), reconciled_before + 1);
    QCOMPARE(f.stash_children_replaced_parents.last(), QString("unique0001"));

    f.deliverChild("unique0001",
                   "uniquechild1",
                   stashOf(stashJson(
                       "uniquechild1", "Set", 0, QStringList{"uc1"}, "UniqueStash", "unique0001")));
    QCOMPARE(f.refresh_count, 2);
    QVERIFY(sortedItemIds(f.last_items).contains("uc1"));
}

// P-APPLIED (verification §4): a reply processed and atomically applied before a
// later terminal failure remains applied in BOTH memory and datastore. The
// earlier failedUpdateKeepsItemsIntact pin masks this: it refetches tab A in its
// recovery update, so clearing A on abort would still pass, and it never connects
// persistence. This pins the two halves the contract actually requires:
//   * memory — after A is applied and B fails, a TabsOnly update publishes the
//     in-memory items WITHOUT refetching A, and A's applied item is still there;
//   * datastore — A's reply persisted through the connected stashReceived slot
//     before the failure, and the abort does not roll it back.
void WorkerUpdateTest::appliedReplySurvivesLaterFailureInMemoryAndDatastore()
{
    WorkerFixture f("papplied");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b-old"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "papplied");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-old", "b-old"}));

    // Persist replies the way Application does, so the datastore half is real.
    UserStore store(QDir(f.dataDir()), "papplied");
    connectPersistence(f.worker.get(), store);

    const std::vector<poe::StashTab> fresh = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)});

    // Update 1 (All): both tab contents go in flight. Tab A lands and is applied
    // — to memory and, through the connected stashReceived slot, to the
    // datastore. Then tab B fails, terminating the update with no emit.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-applied"})));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1); // the failed update emits nothing

    // Datastore half: A's applied reply persisted before the failure and the
    // abort did not roll it back.
    auto storedItemIds = [&](const QString &id) {
        const auto stored = store.stashes().getStash(id, kRealm, kLeague);
        QStringList ids;
        if (stored && stored->items) {
            for (const auto &item : *stored->items) {
                if (item.id) {
                    ids.append(*item.id);
                }
            }
        }
        ids.sort();
        return ids;
    };
    QCOMPARE(storedItemIds("stashaaaa1"), QStringList({"a-applied"}));

    // Memory half: publish the in-memory snapshot WITHOUT refetching A. A TabsOnly
    // update fetches both lists but no content, so no getStash for A is issued —
    // if the abort had cleared A's applied items, they would be absent from the
    // emit.
    const size_t calls_before = f.callCount();
    f.worker->Update(TabSelection::TabsOnly);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    QCOMPARE(f.callCount(), calls_before + 2); // only the two lists — no content refetch
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-applied", "b-old"}));
}

// M2 D3 (stage 1, worker half): every accepted content reply emits exactly one
// primary TabRefreshed whose key is the reply's FetchSourceKey and whose items
// are exactly the applied replacement, sharing the worker's shared_ptrs —
// after the atomic replace, before the counter increment, and before the
// terminal emit. A top-level stash reply is followed by exactly one
// ChildrenReconciled, in that order; character replies emit none. The cached
// initial load stays one snapshot with no deltas, and an empty reply still
// emits its (empty) delta — an emptied fetch source, never a deletion.
void WorkerUpdateTest::deltaMatchesAppliedReplacement()
{
    WorkerFixture f("delta-replacement");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b-old"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "delta-replacement");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
        QVERIFY(store.characters().saveCharacterList(
            characterList({characterJson("charaaaa01", "TestChar")})));
        QVERIFY(saveCharacterFixture(store.characters(),
                                     characterOf(characterJson("charaaaa01",
                                                               "TestChar",
                                                               QStringList{"c-old"}))));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    // The cached initial load is one snapshot emit, never deltas (D3).
    QCOMPARE(f.deltas.size(), size_t(0));
    QCOMPARE(f.reconciles.size(), size_t(0));
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a-old", "b-old", "c-old"}));

    f.worker->Update(TabSelection::All);
    f.deliverStashList(
        stashList({stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList(characterList({characterJson("charaaaa01", "TestChar")}));
    QCOMPARE(f.deltas.size(), size_t(0)); // list replies produce no deltas

    // Stash reply: the delta is the applied replacement, keyed by the reply's
    // fetch source.
    f.deliverStash("stashaaaa1",
                   stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1", "a2"})));
    QCOMPARE(f.deltas.size(), size_t(1));
    {
        const auto &delta = f.deltas.back();
        QCOMPARE(delta.location.type(), ItemLocationType::STASH);
        QCOMPARE(delta.location.fetch_id(), QString("stashaaaa1"));
        QCOMPARE(sortedItemIds(delta.items), QStringList({"a1", "a2"}));
        // Before the counter increment (D3's emit point)...
        QCOMPARE(delta.counters_at_emit.stashes_received, size_t(0));
        // ...and before any terminal emit.
        QCOMPARE(delta.refresh_count_at_emit, 1);
    }
    // Every top-level stash reply is followed by exactly one aggregate
    // reconciliation carrying the authoritative expected set — for a plain
    // tab, just its own fetch source. The persistence-lane
    // stashChildrenReplaced stays Map/Unique-only: the lanes share an emit
    // site but not a condition.
    QCOMPARE(f.reconciles.size(), size_t(1));
    {
        const auto &reconcile = f.reconciles.back();
        QCOMPARE(reconcile.parent.id(), QString("stashaaaa1"));
        const std::vector<FetchSourceKey> expected{{ItemLocationType::STASH, "stashaaaa1"}};
        QVERIFY(reconcile.expected == expected);
        QVERIFY(reconcile.seq > f.deltas.back().seq); // primary delta first
    }
    QCOMPARE(f.stash_children_replaced_parents.size(), qsizetype(0));
    // Published copy (stage 2): the snapshot with A's replacement applied.
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a1", "a2", "b-old", "c-old"}));
    QCOMPARE(f.manager_delta_reemits, 1);
    QCOMPARE(f.manager_reconcile_reemits, 1);

    // An empty reply still emits its delta: an emptied fetch source.
    f.deliverStash("stashbbbb1", stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{})));
    QCOMPARE(f.deltas.size(), size_t(2));
    QCOMPARE(f.deltas.back().location.fetch_id(), QString("stashbbbb1"));
    QVERIFY(f.deltas.back().items.empty());
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a1", "a2", "c-old"}));

    // Character reply: symmetric delta, no reconciliation.
    f.deliverCharacter("TestChar",
                       characterOf(characterJson("charaaaa01", "TestChar", QStringList{"c1"})));
    QCOMPARE(f.deltas.size(), size_t(3));
    {
        const auto &delta = f.deltas.back();
        QCOMPARE(delta.location.type(), ItemLocationType::CHARACTER);
        QCOMPARE(delta.location.fetch_id(), QString("charaaaa01"));
        QCOMPARE(sortedItemIds(delta.items), QStringList({"c1"}));
        QCOMPARE(delta.counters_at_emit.characters_received, size_t(0));
        QCOMPARE(delta.refresh_count_at_emit, 1);
    }
    QCOMPARE(f.reconciles.size(), size_t(2)); // tab B's; characters add none
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a1", "a2", "c1"}));

    // The update completes after all deltas, and the delta shared the very
    // shared_ptrs the final snapshot publishes.
    QCOMPARE(f.refresh_count, 2);
    const auto &stash_delta = f.deltas.front();
    for (const auto &delta_item : stash_delta.items) {
        const auto published = std::find(f.last_items.cbegin(), f.last_items.cend(), delta_item);
        QVERIFY(published != f.last_items.cend()); // pointer identity, not equality
    }
    // The final snapshot leaves the published copy identical to the
    // streamed state (D6 convergence).
    QCOMPARE(sortedItemIds(f.manager->items()), sortedItemIds(f.last_items));
    QCOMPARE(f.manager_delta_reemits, 3);
    QCOMPARE(f.manager_reconcile_reemits, 2);
}

// M2 D3/R5-2/R6-2: a top-level parent reply that stops listing a child yields
// one ChildrenReconciled whose expected set is the parent's own fetch source
// plus its currently listed children — the same condition as the worker's own
// ghost erase (fetch_id == id), not the narrower Map/Unique persistence
// condition. Child replies (fetch_id != id) reconcile nothing. The published
// parent bucket loses exactly the items whose keys fall outside the expected
// set, in one erase pass (stage 2).
void WorkerUpdateTest::parentReplyReconcilesChildrenAgainstExpectedSet()
{
    WorkerFixture f("delta-reconcile");
    f.settings.setValue("get_map_stashes", true);

    const auto parent_tab = json::readStash(
        stashJson("mapstash01", "Maps", 0, QStringList{"p1"}, "MapStash"));
    const auto child_tab = json::readStash(
        stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01"));
    QVERIFY(parent_tab && child_tab);
    {
        UserStore store(QDir(f.dataDir()), "delta-reconcile");
        QVERIFY(store.stashes().saveStashList({*parent_tab}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *parent_tab, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *child_tab, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"m1", "p1"}));

    // The parent's reply drops mapchild01 and lists mapchild02 instead.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*parent_tab)});
    f.deliverStashList(stashList({stashJson("mapstash01", "Maps", 0, {}, "MapStash")}));
    f.deliverStash(
        "mapstash01",
        stashOf(stashJson("mapstash01",
                          "Maps",
                          0,
                          QStringList{"p1-new"},
                          "MapStash",
                          {},
                          {stashJson("mapchild02", "Tier 2", 0, {}, "MapStash", "mapstash01")})));

    // The primary delta first, then exactly one reconciliation whose
    // expected set is the parent plus its listed children.
    QCOMPARE(f.deltas.size(), size_t(1));
    QCOMPARE(f.deltas.back().location.fetch_id(), QString("mapstash01"));
    QCOMPARE(sortedItemIds(f.deltas.back().items), QStringList({"p1-new"}));
    QCOMPARE(f.reconciles.size(), size_t(1));
    {
        const auto &reconcile = f.reconciles.back();
        QCOMPARE(reconcile.parent.id(), QString("mapstash01"));
        const std::vector<FetchSourceKey> expected{{ItemLocationType::STASH, "mapstash01"},
                                                   {ItemLocationType::STASH, "mapchild02"}};
        QVERIFY(reconcile.expected == expected);
        QVERIFY(reconcile.seq > f.deltas.back().seq);
    }
    // Published copy (stage 2): the primary delta replaced p1, and the
    // reconciliation erased the vanished child's m1 — exactly the keys
    // outside the expected set, nothing else.
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"p1-new"}));

    // A child reply emits its own delta but no reconciliation: its fetch id
    // is not the display id, so it is not a top-level parent reply.
    f.deliverChild(
        "mapstash01",
        "mapchild02",
        stashOf(stashJson("mapchild02", "Tier 2", 0, QStringList{"m2"}, "MapStash", "mapstash01")));
    QCOMPARE(f.deltas.size(), size_t(2));
    QCOMPARE(f.deltas.back().location.id(), QString("mapstash01"));
    QCOMPARE(f.deltas.back().location.fetch_id(), QString("mapchild02"));
    QCOMPARE(f.reconciles.size(), size_t(1));

    // The worker's own erase ran against the same expected set: the vanished
    // child's item is gone from the final snapshot, and the published copy
    // converges to it.
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"m2", "p1-new"}));
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"m2", "p1-new"}));
}

// M2 D6 (stage 2): at any point mid-update, ItemsManager::items() equals the
// pre-update snapshot with applied replacements substituted, and after a
// mid-update failure it stays there — the published-copy extension of
// appliedReplySurvivesLaterFailureInMemoryAndDatastore.
void WorkerUpdateTest::publishedStateIsSnapshotPlusAppliedDeltas()
{
    WorkerFixture f("published-state");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b-old"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "published-state");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a-old", "b-old"}));

    const std::vector<poe::StashTab> fresh = stashList(
        {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)});

    f.worker->Update(TabSelection::All);
    f.deliverStashList(fresh);
    f.deliverCharacterList({});
    // Lists produce no deltas: the published copy is still the snapshot.
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a-old", "b-old"}));

    // A's replacement applies; B is untouched.
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-new"})));
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a-new", "b-old"}));

    // A mid-update terminal failure leaves the published copy exactly where
    // the applied deltas put it — no rollback, no final snapshot.
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a-new", "b-old"}));
}

// M2 D3/D6 (stage 2): an empty replacement empties that fetch source in the
// published copy and removes nothing else; no tab disappears from the
// published tab list mid-refresh (tab deletion is snapshot-boundary).
void WorkerUpdateTest::emptyDeltaEmptiesFetchSourceOnly()
{
    WorkerFixture f("empty-delta");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1", "a2"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "empty-delta");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"a1", "a2", "b1"}));
    QCOMPARE(f.bm.manager->GetStashTabLocations().size(), size_t(2));

    f.worker->Update(TabSelection::All);
    f.deliverStashList(
        stashList({stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList({});

    // A's emptied replacement removes exactly A's items...
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{})));
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1"}));
    // ...and the published tab list still holds both tabs mid-refresh.
    QCOMPARE(f.bm.manager->GetStashTabLocations().size(), size_t(2));

    f.deliverStash("stashbbbb1", stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1"}));
    QCOMPARE(f.bm.manager->GetStashTabLocations().size(), size_t(2));
}

// M2 R6-2 (stage 2): the expected set reconciles the PUBLISHED baseline, not
// worker memory. Update N's list deletes parent P (worker memory and
// datastore drop P and its child's items at list arrival), then N fails, so
// the published copy keeps both. When N+1 lists P again and P's reply no
// longer lists the child, applying N+1's ChildrenReconciled erases the
// child's published items even though the worker had nothing left to erase.
void WorkerUpdateTest::reconcileErasesGhostsAcrossFailedUpdates()
{
    WorkerFixture f("reconcile-ghosts");
    f.settings.setValue("get_map_stashes", true);

    const auto parent_tab = json::readStash(
        stashJson("mapstash01", "Maps", 0, QStringList{"p1"}, "MapStash"));
    const auto child_tab = json::readStash(
        stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01"));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(parent_tab && child_tab && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "reconcile-ghosts");
        QVERIFY(store.stashes().saveStashList({*parent_tab, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *parent_tab, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *child_tab, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1", "m1", "p1"}));

    // Update N: the fresh list no longer holds P — worker memory drops P's
    // and the child's items at list arrival — and then B's fetch dies, so
    // nothing reaches the published copy.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList({});
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1", "m1", "p1"}));

    // Update N+1: P is listed again, and its reply lists no children. The
    // reconciliation's expected set is {P}, and applying it to the published
    // baseline erases the child ghost m1 the worker itself no longer held.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList(
        {stashJson("mapstash01", "Maps", 0, {}, "MapStash"), stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList({});
    f.deliverStash("mapstash01",
                   stashOf(stashJson("mapstash01", "Maps", 0, QStringList{"p1-new"}, "MapStash")));
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1", "p1-new"}));

    f.deliverStash("stashbbbb1", stashOf(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"b1", "p1-new"}));
}

// M2 D7 (stage 2): scoped per-delta pricing followed by the final pass yields
// BuyoutManager state identical to the final pass alone (modulo last_update,
// which Buyout::operator== ignores). Two identical fixtures run the same
// scripted update — one streaming with scoped pricing, one final-pass-only —
// and every item buyout, tab buyout, and refresh lock must agree.
void WorkerUpdateTest::scopedPricingConvergesToFinalPass()
{
    const auto runScenario = [](WorkerFixture &f, const QString &account, bool connect_deltas) {
        const auto tab_a = json::readStash(
            stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-plain"}));
        const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
        QVERIFY(tab_a && tab_b);
        {
            UserStore store(QDir(f.dataDir()), account);
            QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
            QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
            QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
        }

        f.start();
        f.attachManager(connect_deltas);
        QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

        // A manual tab buyout for A, set before the update, that streamed
        // items must inherit; the fresh list renames B to a price-bearing
        // label, which only the final pass may turn into a tab buyout.
        f.bm.manager->SetTab(ItemLocation(*tab_a), makeChaosBuyout(3));

        f.worker->Update(TabSelection::All);
        f.deliverStashList(stashList(
            {stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "~b/o 2 chaos", 1)}));
        f.deliverCharacterList({});
        f.deliverStash("stashaaaa1",
                       stashOf(stashJsonWithItems("stashaaaa1",
                                                  "Tab A",
                                                  0,
                                                  itemJsonWithNote("a-note", "~b/o 5 chaos") + ","
                                                      + itemJson("a-plain"))));
        f.deliverStash("stashbbbb1",
                       stashOf(stashJson("stashbbbb1", "~b/o 2 chaos", 1, QStringList{"b1"})));
        QCOMPARE(f.refresh_count, 2);
    };

    WorkerFixture streamed("scoped-conv-streamed");
    runScenario(streamed, "scoped-conv-streamed", true);
    if (QTest::currentTestFailed()) {
        return;
    }
    WorkerFixture final_only("scoped-conv-final");
    runScenario(final_only, "scoped-conv-final", false);
    if (QTest::currentTestFailed()) {
        return;
    }

    // Identical published items...
    QCOMPARE(sortedItemIds(streamed.manager->items()), sortedItemIds(final_only.manager->items()));
    // ...identical item buyouts (operator== ignores last_update)...
    for (const auto &item : streamed.manager->items()) {
        const auto other = std::find_if(final_only.manager->items().cbegin(),
                                        final_only.manager->items().cend(),
                                        [&](const std::shared_ptr<Item> &candidate) {
                                            return candidate->id() == item->id();
                                        });
        QVERIFY(other != final_only.manager->items().cend());
        QVERIFY(streamed.bm.manager->Get(*item) == final_only.bm.manager->Get(**other));
    }
    // ...and identical tab buyouts and refresh locks per tab.
    for (const auto &loc : streamed.bm.manager->GetStashTabLocations()) {
        QVERIFY(streamed.bm.manager->GetTab(loc) == final_only.bm.manager->GetTab(loc));
        QCOMPARE(streamed.bm.manager->GetRefreshLocked(loc),
                 final_only.bm.manager->GetRefreshLocked(loc));
    }
}

// M2 D7/R1-4 (stage 2): after deltas followed by a terminal failure, every
// published item whose buyout requires a refresh has its tab refresh-locked,
// pre-existing locks survive (the scoped pass never calls ClearRefreshLocks),
// and the tab-buyout table equals its pre-update state — the rename-to-price
// label the update's list carried leaked nothing without a final pass.
void WorkerUpdateTest::scopedPricingIsFailSafeAcrossFailedUpdate()
{
    WorkerFixture f("scoped-failsafe");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-plain"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b1"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "scoped-failsafe");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    const ItemLocation loc_a = ItemLocation(*tab_a);
    const ItemLocation loc_b = ItemLocation(*tab_b);
    const Buyout pre_tab_a = f.bm.manager->GetTab(loc_a);
    const Buyout pre_tab_b = f.bm.manager->GetTab(loc_b);
    // A pre-existing lock the scoped pass must not clear.
    f.bm.manager->SetRefreshLocked(loc_b);

    // The update's fresh list renames A to a price-bearing label; A's delta
    // carries a note-priced item; then B dies, so no final pass ever runs.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList(
        {stashJson("stashaaaa1", "~b/o 9 chaos", 0), stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1",
                   stashOf(stashJsonWithItems("stashaaaa1",
                                              "~b/o 9 chaos",
                                              0,
                                              itemJsonWithNote("a-note", "~b/o 5 chaos"))));
    QVERIFY(f.hasPendingStash("stashbbbb1"));
    f.failStash("stashbbbb1");
    QCOMPARE(f.refresh_count, 1);

    // Every published item whose buyout requires a refresh has its tab
    // locked (remove-only tabs excluded per D7 rule 3 / R5-7).
    for (const auto &item : f.manager->items()) {
        if (f.bm.manager->Get(*item).RequiresRefresh() && !item->location().removeonly()) {
            QVERIFY(f.bm.manager->GetRefreshLocked(item->location()));
        }
    }
    // The note-priced delta item locked its tab in particular...
    QVERIFY(f.bm.manager->GetRefreshLocked(loc_a));
    // ...the pre-existing lock survived...
    QVERIFY(f.bm.manager->GetRefreshLocked(loc_b));
    // ...and the tab-buyout table is unchanged: no auto-tab leak from the
    // rename, no other tab-level mutation.
    QVERIFY(f.bm.manager->GetTab(loc_a) == pre_tab_a);
    QVERIFY(f.bm.manager->GetTab(loc_b) == pre_tab_b);
}

// M2 D2 (stage 2): documented-behavior pin, not a bug tripwire — a Map
// parent's display tab may transiently mix child-data generations while its
// children stream in; each fetch source stays internally consistent.
void WorkerUpdateTest::parentBucketMayMixChildGenerationsMidRefresh()
{
    WorkerFixture f("parent-mix");
    f.settings.setValue("get_map_stashes", true);

    const auto parent_tab = json::readStash(
        stashJson("mapstash01", "Maps", 0, QStringList{"p1"}, "MapStash"));
    const auto child_1 = json::readStash(
        stashJson("mapchild01", "Tier 1", 0, QStringList{"m1"}, "MapStash", "mapstash01"));
    const auto child_2 = json::readStash(
        stashJson("mapchild02", "Tier 2", 0, QStringList{"m2"}, "MapStash", "mapstash01"));
    QVERIFY(parent_tab && child_1 && child_2);
    {
        UserStore store(QDir(f.dataDir()), "parent-mix");
        QVERIFY(store.stashes().saveStashList({*parent_tab}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *parent_tab, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *child_1, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *child_2, kRealm, kLeague));
    }

    f.start();
    f.attachManager();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"m1", "m2", "p1"}));

    // The parent reply still lists both children; child 1's fresh contents
    // arrive, child 2's have not yet.
    f.worker->Update(TabSelection::Selected, {ItemLocation(*parent_tab)});
    f.deliverStashList(stashList({stashJson("mapstash01", "Maps", 0, {}, "MapStash")}));
    f.deliverStash(
        "mapstash01",
        stashOf(stashJson("mapstash01",
                          "Maps",
                          0,
                          QStringList{"p1-new"},
                          "MapStash",
                          {},
                          {stashJson("mapchild01", "Tier 1", 0, {}, "MapStash", "mapstash01"),
                           stashJson("mapchild02", "Tier 2", 0, {}, "MapStash", "mapstash01")})));
    f.deliverChild("mapstash01",
                   "mapchild01",
                   stashOf(stashJson("mapchild01",
                                     "Tier 1",
                                     0,
                                     QStringList{"m1-new"},
                                     "MapStash",
                                     "mapstash01")));

    // The published parent bucket now mixes generations: fresh parent items
    // and child 1, stale child 2 — all displaying under mapstash01.
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"m1-new", "m2", "p1-new"}));
    for (const auto &item : f.manager->items()) {
        QCOMPARE(item->location().id(), QString("mapstash01"));
    }

    f.deliverChild("mapstash01",
                   "mapchild02",
                   stashOf(stashJson("mapchild02",
                                     "Tier 2",
                                     0,
                                     QStringList{"m2-new"},
                                     "MapStash",
                                     "mapstash01")));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.manager->items()), QStringList({"m1-new", "m2-new", "p1-new"}));
}

// M2 D4 (stage 6): RefreshFinished fires exactly once per accepted Update()
// — CompletedRefresh (empty skipped) after the final ItemsRefreshed on
// success, FailedRefresh with the FIRST terminal error on failure; a refused
// Update() emits nothing, and settling stragglers adds nothing.
void WorkerUpdateTest::terminalEventExactlyOncePerUpdate()
{
    WorkerFixture f("d4-once");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QCOMPARE(f.terminals.size(), size_t(0)); // the cached load is not an update

    // Update 1 succeeds.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Tab A", 0)}));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.terminals.size(), size_t(1));
    const auto &completed = f.terminals[0];
    QVERIFY(std::holds_alternative<CompletedRefresh>(completed.outcome));
    QVERIFY(std::get<CompletedRefresh>(completed.outcome).skipped.empty());
    // Pinned ordering: final ItemsRefreshed precedes the terminal event.
    QCOMPARE(completed.refresh_count_at_emit, 2);

    // Update 2 fails on its stash list; the sibling character list becomes a
    // stopped straggler whose settlement must not emit a second terminal.
    f.worker->Update(TabSelection::All);
    const size_t calls_during = f.callCount();

    // A refused Update() (already updating) emits nothing.
    f.worker->Update(TabSelection::All);
    QVERIFY(WorkerFixture::drainUntilIdle());
    QCOMPARE(f.callCount(), calls_during);
    QCOMPARE(f.terminals.size(), size_t(1));

    f.failStashList(RateLimit::FetchError::Kind::Http);
    QCOMPARE(f.terminals.size(), size_t(2));
    const auto &failed = f.terminals[1];
    QVERIFY(std::holds_alternative<FailedRefresh>(failed.outcome));
    // The FIRST terminal error is preserved, kind intact.
    QCOMPARE(std::get<FailedRefresh>(failed.outcome).error.kind, RateLimit::FetchError::Kind::Http);

    QCOMPARE(f.settleStragglers(), size_t(1));
    QCOMPARE(f.terminals.size(), size_t(2)); // stragglers add nothing
}

// M2 D4 (stage 6): the ordering guarantee IS the identity — every
// TabRefreshed and ChildrenReconciled of an update precedes its
// RefreshFinished, and nothing of that update follows it, even when a
// stopped straggler later resolves with SUCCESS (extends W-IDENTITY to both
// delta signals).
void WorkerUpdateTest::deltasNeverFollowTerminalEvent()
{
    WorkerFixture f("d4-order");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Three tabs: A delivers (delta + reconcile), B fails (terminal), C is
    // abandoned in flight as the stopped straggler.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Tab A", 0),
                                  stashJson("stashbbbb1", "Tab B", 1),
                                  stashJson("stashcccc1", "Tab C", 2)}));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    QCOMPARE(f.deltas.size(), size_t(1));
    QCOMPARE(f.reconciles.size(), size_t(1));

    f.failStash("stashbbbb1");
    QCOMPARE(f.terminals.size(), size_t(1));
    const int terminal_seq = f.terminals[0].seq;

    // Everything the update emitted precedes its terminal event.
    for (const auto &delta : f.deltas) {
        QVERIFY(delta.seq < terminal_seq);
    }
    for (const auto &reconcile : f.reconciles) {
        QVERIFY(reconcile.seq < terminal_seq);
    }

    // The straggler resolves with SUCCESS: the post-await gate discards it,
    // so nothing of the failed update follows its terminal event.
    f.resolveStragglerStash("stashcccc1",
                            stashOf(stashJson("stashcccc1", "Tab C", 2, QStringList{"c1"})));
    QCOMPARE(f.deltas.size(), size_t(1));
    QCOMPARE(f.reconciles.size(), size_t(1));
    QCOMPARE(f.terminals.size(), size_t(1));
}

// M2 D4/R4-4 (stage 6): the worker is observably Idle during the terminal
// fan-out, but an Update() arriving inside it is refused — otherwise a
// fail-fast N+1 could nest its own signals inside N's fan-out. A restart
// queued to the next event-loop turn is accepted normally.
void WorkerUpdateTest::terminalFanOutRefusesReentrantUpdate()
{
    WorkerFixture f("d4-reentrant");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // From inside the RefreshFinished fan-out, try to start the next update
    // synchronously and record what happened.
    size_t calls_inside = 0;
    int notifies_inside = 0;
    QObject::connect(f.worker.get(),
                     &ItemsManagerWorker::RefreshFinished,
                     f.worker.get(),
                     [&](const RefreshOutcome &) {
                         f.worker->Update(TabSelection::All);
                         calls_inside = f.callCount();
                         notifies_inside = int(f.notify_messages.size());
                     });

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Tab A", 0)}));
    f.deliverCharacterList({});
    const size_t calls_before_terminal = f.callCount();
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));

    // The reentrant Update() was refused: nothing submitted, the user
    // notified, and no nested terminal event was delivered.
    QCOMPARE(calls_inside, calls_before_terminal);
    QVERIFY(notifies_inside > 0);
    QVERIFY(f.notify_messages.last().contains("in progress"));
    QCOMPARE(f.terminals.size(), size_t(1));
    QVERIFY(std::holds_alternative<CompletedRefresh>(f.terminals[0].outcome));

    // A restart queued to the next event-loop turn finds a genuinely idle
    // worker and is accepted normally.
    QVERIFY(WorkerFixture::drainUntilIdle());
    const size_t calls_after = f.callCount();
    f.worker->Update(TabSelection::All);
    QVERIFY(WorkerFixture::drainUntilIdle());
    QVERIFY(f.callCount() > calls_after);
    QVERIFY(f.hasPendingStashList());

    // Leave the fixture quiescent: fail the accepted update's list and
    // settle its sibling.
    f.failStashList();
    f.settleStragglers();
}

// M2 D5 (stage 7): a Parse failure on a content fetch is deterministic —
// skip and continue. The update completes, the terminal event reports
// CompletedRefresh with the skipped source, the skipped tab's previous
// items survive (no delta ran), the final status names the skip count, and
// a Parse failure on a LIST fetch stays terminal.
void WorkerUpdateTest::parseFailureSkipsTabAndUpdateCompletes()
{
    WorkerFixture f("d5-skip");

    const auto tab_a = json::readStash(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-old"}));
    const auto tab_b = json::readStash(stashJson("stashbbbb1", "Tab B", 1, QStringList{"b-old"}));
    QVERIFY(tab_a && tab_b);
    {
        UserStore store(QDir(f.dataDir()), "d5-skip");
        QVERIFY(store.stashes().saveStashList({*tab_a, *tab_b}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_a, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *tab_b, kRealm, kLeague));
    }

    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverStashList(
        stashList({stashJson("stashaaaa1", "Tab A", 0), stashJson("stashbbbb1", "Tab B", 1)}));
    f.deliverCharacterList({});
    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a-new"})));

    // Tab B's payload fails the facade parse: skipped, not terminal.
    f.failStash("stashbbbb1", {}, RateLimit::FetchError::Kind::Parse);

    // The update COMPLETED: final snapshot published, clean terminal shape
    // with the skipped source named.
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.terminals.size(), size_t(1));
    QVERIFY(std::holds_alternative<CompletedRefresh>(f.terminals[0].outcome));
    const auto &skipped = std::get<CompletedRefresh>(f.terminals[0].outcome).skipped;
    QCOMPARE(skipped.size(), size_t(1));
    QCOMPARE(skipped[0].source.type, ItemLocationType::STASH);
    QCOMPARE(skipped[0].source.fetch_id, QString("stashbbbb1"));
    QCOMPARE(skipped[0].error.kind, RateLimit::FetchError::Kind::Parse);

    // No delta ran for tab B, so its previous items survive alongside A's
    // replacement (M1 non-destructive semantics).
    QCOMPARE(f.deltas.size(), size_t(1));
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"a-new", "b-old"}));

    // Skips are user-visible in the final status (R1-1).
    QVERIFY(!f.status_updates.empty());
    QVERIFY(f.status_updates.back().message.contains("1 skipped"));

    // A LIST Parse failure stays terminal — the update cannot even define
    // its batches without lists.
    f.worker->Update(TabSelection::All);
    f.failStashList(RateLimit::FetchError::Kind::Parse);
    QCOMPARE(f.terminals.size(), size_t(2));
    QVERIFY(std::holds_alternative<FailedRefresh>(f.terminals[1].outcome));
    QCOMPARE(std::get<FailedRefresh>(f.terminals[1].outcome).error.kind,
             RateLimit::FetchError::Kind::Parse);
    f.settleStragglers();
}

// M2 D5/R2-4 (stage 7): a 200 whose reply lacks its stash sub-object is
// classified Parse AT THE FACADE (F62) and takes the skip path — the
// worker's old untyped is-empty branches are gone, so from the worker's
// side this is exactly a Parse-classified content failure.
void WorkerUpdateTest::missingStashWrapperSkipsTab()
{
    WorkerFixture f("d5-wrapper-stash");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Tab A", 0)}));
    f.deliverCharacterList({});
    f.failStash("stashaaaa1", {}, RateLimit::FetchError::Kind::Parse);

    QCOMPARE(f.refresh_count, 2); // completed, not aborted
    QCOMPARE(f.terminals.size(), size_t(1));
    QVERIFY(std::holds_alternative<CompletedRefresh>(f.terminals[0].outcome));
    const auto &skipped = std::get<CompletedRefresh>(f.terminals[0].outcome).skipped;
    QCOMPARE(skipped.size(), size_t(1));
    QCOMPARE(skipped[0].source.fetch_id, QString("stashaaaa1"));
    QCOMPARE(f.deltas.size(), size_t(0)); // no delta for a skipped source
}

// M2 D5/R2-4 (stage 7): the character-lane twin of the pin above.
void WorkerUpdateTest::missingCharacterWrapperSkipsTab()
{
    WorkerFixture f("d5-wrapper-char");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverStashList({});
    f.deliverCharacterList(characterList({characterJson("charid0001", "CharOne")}));
    QVERIFY(f.hasPendingCharacter("CharOne"));
    f.api.reject(f.api.pendingCharacter("CharOne"), RateLimit::FetchError::Kind::Parse);
    QVERIFY(WorkerFixture::drainUntilIdle());

    QCOMPARE(f.refresh_count, 2); // completed, not aborted
    QCOMPARE(f.terminals.size(), size_t(1));
    QVERIFY(std::holds_alternative<CompletedRefresh>(f.terminals[0].outcome));
    const auto &skipped = std::get<CompletedRefresh>(f.terminals[0].outcome).skipped;
    QCOMPARE(skipped.size(), size_t(1));
    QCOMPARE(skipped[0].source.type, ItemLocationType::CHARACTER);
    QCOMPARE(skipped[0].source.fetch_id, QString("charid0001"));
    QCOMPARE(f.deltas.size(), size_t(0));
    QVERIFY(f.status_updates.back().message.contains("1 skipped"));
}

// Folder-tree D12 / R8 / A4: a Folder-type stash holds tabs, never items.
// Items one carries — in a cached row or a live reply — are discarded in the
// worker with one warning naming the tab and the count, so they never enter
// the item store and the two view modes cannot disagree about them. The
// folder itself is still requested (R9) and its empty delta still lands.
void WorkerUpdateTest::folderItemsAreDiscardedWithWarning()
{
    WorkerFixture f("folder-items-discarded");

    auto folder = json::readStash(
        stashJson("folder0001", "Loot Folder", 0, QStringList{"cached-f1", "cached-f2"}, "Folder"));
    auto member = json::readStash(stashJson("stashmmmm1", "Member", 1, QStringList{"m1"}));
    QVERIFY(folder && member);
    member->folder = "folder0001";
    {
        UserStore store(QDir(f.dataDir()), "folder-items-discarded");
        QVERIFY(store.stashes().saveStashList({*folder, *member}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *folder, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), *member, kRealm, kLeague));
    }

    // Cached row.
    {
        ScopedLogCapture log;
        f.start();
        QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
        QCOMPARE(sortedItemIds(f.last_items), QStringList({"m1"}));
        QCOMPARE(log.lines({"Loot Folder", "folder0001", "2 items"}).size(), 1);
    }

    // Live reply.
    ScopedLogCapture log;
    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    auto list = stashList({stashJson("folder0001", "Loot Folder", 0, {}, "Folder"),
                           stashJson("stashmmmm1", "Member", 1)});
    list[1].folder = "folder0001";
    f.deliverStashList(list);
    QVERIFY(f.hasPendingStash("folder0001"));
    QVERIFY(f.hasPendingStash("stashmmmm1"));
    f.deliverStash("folder0001",
                   stashOf(stashJson("folder0001",
                                     "Loot Folder",
                                     0,
                                     QStringList{"live-f1", "live-f2", "live-f3"},
                                     "Folder")));
    QCOMPARE(log.lines({"Loot Folder", "folder0001", "3 items"}).size(), 1);
    // The folder's delta still lands, empty.
    QVERIFY(!f.deltas.empty());
    QCOMPARE(f.deltas.back().location.id(), QString("folder0001"));
    QVERIFY(f.deltas.back().items.empty());

    f.deliverStash("stashmmmm1", stashOf(stashJson("stashmmmm1", "Member", 1, QStringList{"m2"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(sortedItemIds(f.last_items), QStringList({"m2"}));
    // One warning per discarding tab, never one per item.
    QCOMPARE(log.lines({"folder0001"}).size(), 1);
}

// Folder-tree D12: a tab listed under a folder's `children` whose own
// `folder` is missing or names another folder is logged. No fallback: the
// tab is processed as listed.
void WorkerUpdateTest::misfiledFolderChildWarns()
{
    WorkerFixture f("misfiled-folder-child");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    auto good = stashOf(stashJson("stashgood1", "Good", 1));
    good.folder = "folder0001";
    auto missing = stashOf(stashJson("stashmiss1", "Missing", 2));
    auto other = stashOf(stashJson("stashothr1", "Other", 3));
    other.folder = "folder0002";
    auto folder = stashOf(stashJson("folder0001", "Folder One", 0, {}, "Folder"));
    folder.children = std::vector<poe::StashTab>{good, missing, other};

    ScopedLogCapture log;
    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    f.deliverStashList({folder});

    QCOMPARE(log.lines({"Folder One", "Missing", "stashmiss1"}).size(), 1);
    QCOMPARE(log.lines({"Folder One", "Other", "stashothr1", "folder0002"}).size(), 1);
    QCOMPARE(log.lines({"stashgood1"}).size(), 0);

    // No fallback: every listed child is still requested as listed.
    QVERIFY(f.hasPendingStash("stashgood1"));
    QVERIFY(f.hasPendingStash("stashmiss1"));
    QVERIFY(f.hasPendingStash("stashothr1"));
    f.deliverStash("folder0001", stashOf(stashJson("folder0001", "Folder One", 0, {}, "Folder")));
    f.deliverStash("stashgood1", stashOf(stashJson("stashgood1", "Good", 1)));
    f.deliverStash("stashmiss1", stashOf(stashJson("stashmiss1", "Missing", 2)));
    f.deliverStash("stashothr1", stashOf(stashJson("stashothr1", "Other", 3)));
    QCOMPARE(f.refresh_count, 2);
}

// Folder-tree amendment (S1): the defensive reply-child path on a Folder
// parent (unobserved live — F49) files each child's items under the child's
// own tab, the way a folder's listed children and the cached rows of the
// same children already display. No item ever displays under a folder's
// location, so a folder bucket holds no items (D2).
void WorkerUpdateTest::folderReplyChildDisplaysUnderItsOwnTab()
{
    WorkerFixture f("folder-reply-child-own-tab");
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverCharacterList({});
    f.deliverStashList(stashList({stashJson("folder0001", "Folder", 0, {}, "Folder")}));
    f.deliverStash("folder0001",
                   stashOf(stashJson(
                       "folder0001",
                       "Folder",
                       0,
                       {},
                       "Folder",
                       {},
                       {stashJson("folderchild1", "Sub", 0, {}, "PremiumStash", "folder0001")})));
    f.deliverChild("folder0001",
                   "folderchild1",
                   stashOf(stashJson("folderchild1",
                                     "Sub",
                                     0,
                                     QStringList{"fc1"},
                                     "PremiumStash",
                                     "folder0001")));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.last_items.size(), size_t(1));
    const ItemLocation &location = f.last_items.front()->location();
    QCOMPARE(location.id(), QString("folderchild1"));
    QCOMPARE(location.fetch_id(), QString("folderchild1"));
    QVERIFY(!location.IsFolder());
    QCOMPARE(f.deltas.back().location.id(), QString("folderchild1"));
}

// Folder-tree A2 / R9 / D10: a Refresh Checked requests a Folder tab when
// at least one of its member tabs is in the checked selection — the
// folder is still fetched (R9) — and never because of a stored refresh
// flag on the folder id, which is neither read nor written.
void WorkerUpdateTest::checkedSelectionIncludesFoldersOfCheckedMembers()
{
    WorkerFixture f("checked-folder-selection");
    auto folder_f = json::readStash(stashJson("folderffff", "Folder F", 0, {}, "Folder"));
    auto member_a = json::readStash(stashJson("stashaaaa1", "Alpha", 1));
    auto folder_g = json::readStash(stashJson("foldergggg", "Folder G", 2, {}, "Folder"));
    auto member_c = json::readStash(stashJson("stashcccc1", "Gamma", 3));
    auto loose_d = json::readStash(stashJson("stashdddd1", "Delta", 4));
    QVERIFY(folder_f && member_a && folder_g && member_c && loose_d);
    member_a->folder = "folderffff";
    member_c->folder = "foldergggg";
    const std::vector<poe::StashTab> listed{*folder_f, *member_a, *folder_g, *member_c, *loose_d};
    {
        UserStore store(QDir(f.dataDir()), "checked-folder-selection");
        QVERIFY(store.stashes().saveStashList(listed, kRealm, kLeague));
    }
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // F's member is checked though F's own stored flag says no; every
    // member of G is unchecked though G's own stored flag says yes.
    auto &buyouts = *f.bm.manager;
    buyouts.SetRefreshChecked(ItemLocation(*folder_f), false);
    buyouts.SetRefreshChecked(ItemLocation(*member_a), true);
    buyouts.SetRefreshChecked(ItemLocation(*folder_g), true);
    buyouts.SetRefreshChecked(ItemLocation(*member_c), false);
    buyouts.SetRefreshChecked(ItemLocation(*loose_d), false);

    f.worker->Update(TabSelection::Checked);
    f.deliverStashList(listed);
    QVERIFY(f.hasPendingStash("folderffff"));
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    QVERIFY(!f.hasPendingStash("foldergggg"));
    QVERIFY(!f.hasPendingStash("stashcccc1"));
    QVERIFY(!f.hasPendingStash("stashdddd1"));
    f.deliverStash("folderffff", *folder_f);
    f.deliverStash("stashaaaa1", *member_a);
    QCOMPARE(f.refresh_count, 2);
    // Neither folder's stored flag was written.
    QVERIFY(!buyouts.GetRefreshChecked(ItemLocation(*folder_f)));
    QVERIFY(buyouts.GetRefreshChecked(ItemLocation(*folder_g)));
}

namespace {

    // A gem as the API nests it in an item's socketedItems: no x/y, a socket
    // index and colour, and gem properties in the game's own formats.
    QString socketedGemJson(const QString &id,
                            const QString &name,
                            bool support,
                            unsigned socket,
                            const QString &colour)
    {
        return QString(R"json({
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/gen/image/gem.png",
            "support": %3,
            "id": "%1",
            "name": "",
            "typeLine": "%2",
            "baseType": "%2",
            "identified": true,
            "ilvl": 0,
            "properties": [
                {"name": "Level", "values": [["20 (Max)", 0]], "displayMode": 0, "type": 5},
                {"name": "Quality", "values": [["+20%", 1]], "displayMode": 0, "type": 6}
            ],
            "frameType": 4,
            "socket": %4,
            "colour": "%5"
        })json")
            .arg(id, name, support ? "true" : "false", QString::number(socket), colour);
    }

    // A two-socket item holding two gems.
    QString socketedItemJson(const QString &id,
                             const QString &base_type,
                             const QString &inventory_id,
                             unsigned x,
                             unsigned y,
                             const QString &gems_json)
    {
        return QString(R"({
            "verified": false,
            "w": 1,
            "h": 3,
            "icon": "https://web.poecdn.com/gen/image/weapon.png",
            "id": "%1",
            "name": "",
            "typeLine": "%2",
            "baseType": "%2",
            "identified": true,
            "ilvl": 75,
            "sockets": [
                {"group": 0, "attr": "I", "sColour": "B"},
                {"group": 0, "attr": "S", "sColour": "R"}
            ],
            "frameType": 0,
            "x": %4,
            "y": %5,
            "inventoryId": "%3",
            "socketedItems": [%6]
        })")
            .arg(id, base_type, inventory_id, QString::number(x), QString::number(y), gems_json);
    }

    // Each item's id with where it is placed, sorted: two runs that build the
    // same items in the same places give equal lists.
    QStringList itemPlacements(const Items &items)
    {
        QStringList placements;
        for (const auto &item : items) {
            const ItemLocation &location = item->location();
            const QRectF rect = location.GetRect();
            placements.append(QString("%1 | %2 | %3 | %4,%5 %6x%7 | socketed=%8")
                                  .arg(item->id(),
                                       location.GetHeader(),
                                       location.fetch_id(),
                                       QString::number(rect.x()),
                                       QString::number(rect.y()),
                                       QString::number(rect.width()),
                                       QString::number(rect.height()),
                                       location.socketed() ? "yes" : "no"));
        }
        placements.sort();
        return placements;
    }

} // namespace

// #260: the startup cache load and a live refresh turn the same stash and
// character payloads into the same items, socketed gems included, in the same
// places. Before the fix the cache load built only top-level items, so after
// a restart every gem was missing until its tab was fetched again.
void WorkerUpdateTest::cacheLoadAndLiveRefreshBuildTheSameItems()
{
    const QString account = "worker-update-account-socketed";
    WorkerFixture f(account);

    const QString stash_items
        = socketedItemJson("stash-sword",
                           "Jewelled Foil",
                           "Stash1",
                           3,
                           0,
                           socketedGemJson("stash-gem-1", "Fireball", false, 0, "I") + ","
                               + socketedGemJson("stash-gem-2",
                                                 "Added Fire Damage Support",
                                                 true,
                                                 1,
                                                 "S"))
          + "," + itemJson("stash-plain");
    const poe::StashTab stash = stashOf(stashJsonWithItems("stashaaaa1", "Gems", 0, stash_items));

    const QString equipment = socketedItemJson("char-sword",
                                               "Jewelled Foil",
                                               "Weapon",
                                               0,
                                               0,
                                               socketedGemJson("char-gem-1", "Arc", false, 0, "I")
                                                   + ","
                                                   + socketedGemJson("char-gem-2",
                                                                     "Faster Casting Support",
                                                                     true,
                                                                     1,
                                                                     "S"));
    const QByteArray character_json
        = QString(
              R"({"id":"charid0001","name":"Gemmer","realm":"%1","class":"Witch","league":"%2","level":90,"experience":0,"equipment":[%3],"inventory":[%4]})")
              .arg(kRealm, kLeague, equipment, itemJson("char-plain"))
              .toUtf8();
    const poe::Character character = characterOf(character_json);

    {
        UserStore store(QDir(f.dataDir()), account);
        QVERIFY(store.stashes().saveStashList({stash}, kRealm, kLeague));
        QVERIFY(saveStashFixture(store.stashes(), stash, kRealm, kLeague));
        QVERIFY(store.characters().saveCharacterList({character}));
        QVERIFY(saveCharacterFixture(store.characters(), character));
    }

    // The cache load.
    f.start();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QVERIFY(f.last_initial);
    const QStringList cached = itemPlacements(f.last_items);

    // A live refresh of the same payloads.
    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Gems", 0)}));
    f.deliverCharacterList(characterList({characterJson("charid0001", "Gemmer")}));
    f.deliverStash("stashaaaa1", stash);
    f.deliverCharacter("Gemmer", character);
    QCOMPARE(f.refresh_count, 2);
    QVERIFY(!f.last_initial);
    const QStringList live = itemPlacements(f.last_items);

    QCOMPARE(sortedItemIds(f.last_items),
             QStringList({"char-gem-1",
                          "char-gem-2",
                          "char-plain",
                          "char-sword",
                          "stash-gem-1",
                          "stash-gem-2",
                          "stash-plain",
                          "stash-sword"}));
    QCOMPARE(cached, live);
}

namespace {

    // The rate-limit headers a HEAD probe needs to establish a single-rule
    // policy (the shape tst_ratelimiter and tst_workerintegration use).
    QList<QNetworkReply::RawHeaderPair> failurePolicyHeaders(const QByteArray &state = "0:60:0")
    {
        return {
            {"X-Rate-Limit-Policy", "test-request-limit"},
            {"X-Rate-Limit-Rules", "Ip"},
            {"X-Rate-Limit-Ip", "10:60:60"},
            {"X-Rate-Limit-Ip-State", state},
            {"Date", QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date).toUtf8()},
        };
    }

    // How the stash-list request of a Selected update fails: on the HEAD
    // probe that sets up its endpoint, or on the GET itself.
    struct FailureCase
    {
        const char *name;
        bool fail_head;
        QList<QNetworkReply::RawHeaderPair> headers;
        int status;
        QNetworkReply::NetworkError error;
        QByteArray body;
    };

    // Run one Selected update of a seeded stash tab through the real facade
    // and hub to its failure, and return the Ready status messages it
    // produced.
    QStringList readyMessagesForFailure(const FailureCase &failure)
    {
        using namespace std::chrono_literals;
        BuyoutManagerFixture bm;
        QSettings settings(bm.tempDir.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("realm", kRealm);
        settings.setValue("league", kLeague);
        settings.setValue("account", "status-text");
        settings.sync();

        poe::StashTab stash;
        stash.id = "stash00001";
        stash.name = "Status Tab";
        stash.type = "PremiumStash";
        stash.index = 0;
        {
            UserStore store(QDir(bm.tempDir.filePath("data")), "status-text");
            if (!store.stashes().saveStashList({stash}, kRealm, kLeague)
                || !saveStashFixture(store.stashes(), stash, kRealm, kLeague)) {
                qFatal("readyMessagesForFailure: could not seed the stash tab");
            }
        }

        FakeScheduler scheduler;
        FakeNetworkManager network;
        RateLimiter limiter(network, &scheduler);
        PoeApiClient api(limiter);
        ItemsManagerWorker worker(settings, *bm.manager, api);

        int initial = 0;
        QStringList ready;
        QObject::connect(&worker,
                         &ItemsManagerWorker::ItemsRefreshed,
                         &worker,
                         [&](const Items &, const std::vector<ItemLocation> &, bool first) {
                             initial += first ? 1 : 0;
                         });
        QObject::connect(&worker,
                         &ItemsManagerWorker::StatusUpdate,
                         &worker,
                         [&](ProgramState state, const QString &message) {
                             if (state == ProgramState::Ready) {
                                 ready.append(message);
                             }
                         });
        worker.OnRePoEReady();
        if (!QTest::qWaitFor([&] { return initial == 1; }, 10000)) {
            qFatal("readyMessagesForFailure: the cache load never finished");
        }
        ready.clear();

        const auto settle = [&] {
            for (int i = 0; i < 10; ++i) {
                QCoreApplication::processEvents();
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                scheduler.AdvanceBy(0ms);
            }
        };

        worker.Update(TabSelection::Selected, {ItemLocation(stash)});
        settle();
        if (failure.fail_head) {
            network.sent(0).reply->finish(failure.headers, failure.status, failure.error);
        } else {
            network.sent(0).reply->finish(failurePolicyHeaders(), 200);
            settle();
            // Past the pacing deadline and the gate's spacing floor: the GET
            // is sent.
            scheduler.AdvanceBy(100ms);
            settle();
            scheduler.AdvanceBy(RateLimit::Gate::MIN_SEND_SPACING);
            settle();
            if (network.count() != 2) {
                qFatal("readyMessagesForFailure: the list GET was not sent");
            }
            network.sent(1).reply->finish(failure.headers,
                                          failure.status,
                                          failure.error,
                                          failure.body);
        }
        settle();
        return ready;
    }

} // namespace

void WorkerUpdateTest::failedUpdateStatusNamesTheError()
{
    const QList<FailureCase> cases{
        {"expired login",
         false,
         failurePolicyHeaders("1:60:0"),
         401,
         QNetworkReply::AuthenticationRequiredError,
         {}},
        {"tab gone",
         false,
         failurePolicyHeaders("1:60:0"),
         404,
         QNetworkReply::ContentNotFoundError,
         {}},
        {"no network", false, {}, 0, QNetworkReply::ConnectionRefusedError, {}},
        {"rate limited",
         false,
         failurePolicyHeaders("1:60:0"),
         429,
         QNetworkReply::UnknownContentError,
         {}},
        {"unreadable list",
         false,
         failurePolicyHeaders("1:60:0"),
         200,
         QNetworkReply::NoError,
         "this is not json"},
        {"unusable setup headers", true, {}, 200, QNetworkReply::NoError, {}},
    };

    QStringList seen;
    for (const auto &failure : cases) {
        const QStringList ready = readyMessagesForFailure(failure);
        // P-FAILURE: one terminal Ready transition per failed update.
        QVERIFY2(ready.size() == 1, failure.name);
        const QString &message = ready.front();
        QVERIFY2(message.startsWith("Update failed: "), qPrintable(message));
        // Nothing from the request itself reaches the status line.
        QVERIFY2(!message.contains("://"), qPrintable(message));
        QVERIFY2(!message.contains("fake network error"), qPrintable(message));
        QVERIFY2(!seen.contains(message),
                 qPrintable(QString("'%1' repeats an earlier case: %2").arg(failure.name, message)));
        seen.append(message);
    }
    // An expired or revoked login points the user at logging in again.
    QVERIFY2(seen.front().contains("log in again"), qPrintable(seen.front()));
}

// Every API request carries a transfer timeout; when it runs out Qt aborts
// the reply with OperationCanceledError. Acquisition never aborts an API
// reply itself (network-redesign D2), so that code means "no answer in
// time", not that anyone cancelled.
void WorkerUpdateTest::timedOutRequestSaysTheServerDidNotAnswer()
{
    for (const auto error : {QNetworkReply::OperationCanceledError, QNetworkReply::TimeoutError}) {
        const QStringList ready = readyMessagesForFailure({"timed out", false, {}, 0, error, {}});
        QCOMPARE(ready.size(), 1);
        QCOMPARE(ready.front(), QString("Update failed: the server did not answer in time"));
    }
}

// A setup HEAD answered 429 with an acceptable Retry-After but incomplete
// rate-limit headers fails the update as RateLimited, without the retry
// details a pump-side 429 carries. The text claims nothing it cannot know.
void WorkerUpdateTest::rateLimitedSetupDoesNotClaimNoRetryTime()
{
    const QStringList ready = readyMessagesForFailure(
        {"setup 429", true, {{"Retry-After", "120"}}, 429, QNetworkReply::UnknownContentError, {}});
    QCOMPARE(ready.size(), 1);
    QCOMPARE(ready.front(), QString("Update failed: rate limited by the server"));
}

// #279: a timer tick that finds an update running does nothing: no notice
// (a modal per tick would stack while the user is away), no request, and the
// running update is left alone.
void WorkerUpdateTest::timerTickDuringUpdateIsDroppedQuietly()
{
    WorkerFixture f("timer-busy");
    f.start();
    f.attachManager();
    f.connectRefreshRequests();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    f.worker->Update(TabSelection::All);
    f.deliverStashList(stashList({stashJson("stashaaaa1", "Tab A", 0)}));
    f.deliverCharacterList({});
    QVERIFY(f.hasPendingStash("stashaaaa1"));
    const std::stop_token token = f.api.tokenForStash("stashaaaa1");
    const size_t calls_before = f.callCount();
    const auto notifies_before = f.notify_messages.size();

    f.manager->OnAutoRefreshTimer();
    QVERIFY(WorkerFixture::drainUntilIdle());
    QCOMPARE(f.notify_messages.size(), notifies_before);
    QCOMPARE(f.callCount(), calls_before);
    QVERIFY(!token.stop_requested());

    f.deliverStash("stashaaaa1", stashOf(stashJson("stashaaaa1", "Tab A", 0, QStringList{"a1"})));
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.terminals.size(), size_t(1));
}

// #279: while the cache is still loading, a user's refresh is queued; a timer
// tick in that window is dropped and must not replace it.
void WorkerUpdateTest::timerTickWhileInitializingKeepsTheUsersQueuedRequest()
{
    WorkerFixture f("timer-init");
    f.start();
    f.attachManager();
    f.connectRefreshRequests();
    // The cache load completes on a later event-loop turn, so the worker is
    // still initializing here.
    QCOMPARE(f.refresh_count, 0);

    f.worker->Update(TabSelection::All);
    QCOMPARE(f.notify_messages.size(), 1); // the user is told it is queued
    f.manager->OnAutoRefreshTimer();
    QCOMPARE(f.notify_messages.size(), 1);

    // The queued run is the user's "All": both lists are requested.
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);
    QVERIFY(WorkerFixture::drainUntilIdle());
    QVERIFY(f.hasPendingStashList());
    QVERIFY(f.hasPendingCharacterList());

    f.deliverStashList({});
    f.deliverCharacterList({});
    QCOMPARE(f.refresh_count, 2);
    QCOMPARE(f.terminals.size(), size_t(1));
}

// The timer's own job: an idle worker refreshes the checked tabs.
void WorkerUpdateTest::timerTickWhenIdleRefreshesCheckedTabs()
{
    WorkerFixture f("timer-idle");
    f.start();
    f.attachManager();
    f.connectRefreshRequests();
    QTRY_COMPARE_WITH_TIMEOUT(f.refresh_count, 1, 10000);

    // Nothing is checked, so the update needs no list and completes at once.
    f.manager->OnAutoRefreshTimer();
    QVERIFY(WorkerFixture::drainUntilIdle());
    QCOMPARE(f.terminals.size(), size_t(1));
    QVERIFY(std::holds_alternative<CompletedRefresh>(f.terminals[0].outcome));
    QVERIFY(f.notify_messages.isEmpty());
}

QTEST_GUILESS_MAIN(WorkerUpdateTest)

#include "tst_workerupdate.moc"
