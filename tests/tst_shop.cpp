// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QBuffer>
#include <QSettings>
#include <QSqlQuery>
#include <QStandardPaths>

#include <memory>

#include "datastore/userstore.h"
#include "fakenetwork.h"
#include "fakenetworkmanager.h"
#include "itemsmanager.h"
#include "poe/poeapiclient.h"
#include "shop.h"
#include "testfixtures.h"
#include "util/networkmanager.h"

class ShopFixture
{
public:
    ShopFixture()
    {
        // NetworkManager creates a disk cache in AppLocalDataLocation.
        QStandardPaths::setTestModeEnabled(true);
        settings = std::make_unique<QSettings>(buyoutFixture.tempDir.filePath("settings.ini"),
                                               QSettings::IniFormat);
        // Offline: the success path goes on to fetch the forum edit-thread
        // page, which against a real NetworkManager would be a live GET to
        // pathofexile.com from a unit test.
        networkManager = std::make_unique<FakeNetworkManager>();
        rateLimiter = std::make_unique<FakeRateLimiter>(*networkManager);
        api = std::make_unique<PoeApiClient>(*rateLimiter);
        itemsManager = std::make_unique<ItemsManager>(*settings,
                                                      *buyoutFixture.manager,
                                                      *buyoutFixture.data);
        shop = makeShop();
    }

    // A fresh Shop over the same datastore, to exercise the constructor's
    // thread-list load against whatever a previous Shop persisted.
    std::unique_ptr<Shop> makeShop()
    {
        return std::make_unique<Shop>(*settings,
                                      *networkManager,
                                      *api,
                                      *buyoutFixture.data,
                                      *itemsManager,
                                      *buyoutFixture.manager);
    }

    BuyoutManagerFixture buyoutFixture;
    std::unique_ptr<QSettings> settings;
    std::unique_ptr<FakeNetworkManager> networkManager;
    std::unique_ptr<FakeRateLimiter> rateLimiter;
    std::unique_ptr<PoeApiClient> api;
    std::unique_ptr<ItemsManager> itemsManager;
    std::unique_ptr<Shop> shop;
};

class ShopTest : public QObject
{
    Q_OBJECT

private slots:
    void freshDatastoreHasNoThreads();
    void settingAndClearingThreads();
    void threadsRoundTripThroughDatastore();
    void stashIndexFailureReleasesTheSubmitLock();
    void stashIndex403RejectsPoeSession();
    void stashIndexSuccessProceedsToShopSubmission();
    void editPage403RejectsPoeSession();
    void shopPost403RejectsPoeSession();
    void continuationDoesNotRunAfterShopDestruction();

    // Items-pipeline M2, D8: the single-active-job foundation (stage 2
    // front-load), plus the delta-dependent capture pins.
    void olderSubmissionCannotCleanNewerInput();
    void activeJobUnaffectedByLocalEdits();
    void shopSubmissionUsesCapturedSnapshot();
    void manualSubmissionRendersCapturedPublishedState();

    // Items-pipeline M2, D8/R1-1 (stage 7): the automatic-submission gate
    // on the typed terminal event.
    void shopSubmitsOnlyOnCleanCompletion();

    // Items-pipeline M2, D8 (stage 8): the latest-eligible multi-job
    // machinery — one active job, one waiting automatic capture, and the
    // drop/drain transitions.
    void newestCleanSnapshotSubmitsAfterActive();
    void automaticSubmissionCoalescesLatestEligible();
    void failedSubmissionDoesNotDrainPendingAutomatic();
    void disablingAutoUpdateDropsWaitingCapture();
    void skippedRefreshDoesNotInvalidateWaitingCapture();
    void expireDropsWaitingCapture();

    // #253: pages beyond the configured threads.
    void morePagesThanThreadsWarnsAndKeepsHash();

    // #250: the shop refuses to post while the buyouts did not load.
    void unloadedBuyoutsBlockSubmission();
    void healthyEmptyStoreStillPostsEmpty();
};

namespace {

    // A Shop ready to submit: threads set and a POESESSID present, the two
    // preconditions SubmitShopToForum checks before it indexes anything.
    void armForSubmission(ShopFixture &fixture)
    {
        fixture.shop->SetThread({"123"});
        fixture.settings->setValue("session_id", "fake-poesessid");
        fixture.settings->setValue("account", "someone");
        fixture.settings->setValue("realm", "pc");
        fixture.settings->setValue("league", "Standard");
    }

    void drainEvents()
    {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // Publish one priced stash item the way a refresh does, and return the
    // published shared_ptr so a test can edit its buyout afterwards.
    std::shared_ptr<Item> publishPricedItem(ShopFixture &fixture, const char *id, double chaos)
    {
        auto item = std::make_shared<Item>(makeTestItem(id));
        const Items items{item};
        const std::vector<ItemLocation> tabs{item->location()};
        fixture.itemsManager->OnItemsRefreshed(items, tabs, false);
        fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(chaos));
        return item;
    }

    // The legacy stash index reply for makeTestStashLocation's tab. The uid
    // the shop keys on is the first 10 characters of the id.
    constexpr const char *kStashIndexJson = R"({"tabs":[{"n":"Test Tab","i":0,"id":"stash00001"}]})";

    // A forum edit-thread page carrying exactly the CSRF hash input and the
    // title input Shop::OnEditPageFinished parses.
    const QByteArray kEditThreadPage
        = "<input type=\"hidden\" name=\"hash\" value=\"fakecsrfhash\">\n"
          "<input type=\"text\" name=\"title\" id=\"title\" "
          "onkeypress=\"return&#x20;event.keyCode&#x21;&#x3D;13\" value=\"My Shop\">";

    // Walk the active job's forum flow to completion: finish the edit-page
    // GET at m_sent index `get_index`, wait out the 500ms CSRF delay for the
    // POST, and finish the POST cleanly.
    void completeForumSubmission(ShopFixture &fixture, int get_index)
    {
        QCOMPARE(fixture.networkManager->sent(get_index).op, QNetworkAccessManager::GetOperation);
        fixture.networkManager->sent(get_index).reply->finish({},
                                                              200,
                                                              QNetworkReply::NoError,
                                                              kEditThreadPage);
        QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), get_index + 2, 5000);
        QCOMPARE(fixture.networkManager->sent(get_index + 1).op,
                 QNetworkAccessManager::PostOperation);
        fixture.networkManager->sent(get_index + 1)
            .reply->finish({}, 200, QNetworkReply::NoError, QByteArray("ok"));
        drainEvents();
    }

    // The form body of a forum POST. QNetworkAccessManager::post(request,
    // QByteArray) wraps the body in a QBuffer parented to the reply.
    QUrlQuery postedForm(const FakeNetworkManager::Sent &sent)
    {
        const auto *buffer = sent.reply ? sent.reply->findChild<QBuffer *>() : nullptr;
        return buffer ? QUrlQuery(QString::fromUtf8(buffer->data())) : QUrlQuery();
    }

    // A Shop over a real per-account UserStore instead of the in-memory
    // buyout fixture, sharing the fixture's settings, network and datastore.
    struct StoreBackedShop
    {
        StoreBackedShop(ShopFixture &fixture, const QDir &dir)
            : store(dir, "TESTER#1234")
            , buyouts(*fixture.buyoutFixture.data, store.buyouts())
            , items(*fixture.settings, buyouts, *fixture.buyoutFixture.data)
            , shop(*fixture.settings,
                   *fixture.networkManager,
                   *fixture.api,
                   *fixture.buyoutFixture.data,
                   items,
                   buyouts)
        {}

        UserStore store;
        BuyoutManager buyouts;
        ItemsManager items;
        Shop shop;
    };

    // A store at the current schema version whose item_buyouts table is
    // gone: it opens and needs no migration, but every buyout read fails.
    void createStoreWithoutItemBuyouts(const QDir &dir)
    {
        {
            UserStore seed(dir, "TESTER#1234");
            QVERIFY(seed.isUsable());
        }
        const QString connection = "drop-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dir.absoluteFilePath("userstore-TESTER#1234.db"));
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("DROP TABLE item_buyouts"));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void armStoreBackedShop(ShopFixture &fixture, StoreBackedShop &backed)
    {
        backed.shop.SetThread({"123"});
        fixture.settings->setValue("session_id", "fake-poesessid");
        fixture.settings->setValue("account", "someone");
        fixture.settings->setValue("realm", "pc");
        fixture.settings->setValue("league", "Standard");
    }

} // namespace

// F45: an empty "shop" key must load as no threads, not as one empty
// thread. Before the fix, QString("").split(";") produced {""}, so
// m_threads.empty() was never true and the no-threads warning in
// SubmitShopToForum was unreachable.
void ShopTest::freshDatastoreHasNoThreads()
{
    ShopFixture fixture;
    QVERIFY(fixture.shop->threads().isEmpty());
}

void ShopTest::settingAndClearingThreads()
{
    ShopFixture fixture;

    fixture.shop->SetThread({"123", "456"});
    QCOMPARE(fixture.shop->threads(), QStringList({"123", "456"}));

    fixture.shop->SetThread({});
    QVERIFY(fixture.shop->threads().isEmpty());
}

void ShopTest::threadsRoundTripThroughDatastore()
{
    ShopFixture fixture;

    fixture.shop->SetThread({"123", "456"});
    const auto reloaded = fixture.makeShop();
    QCOMPARE(reloaded->threads(), QStringList({"123", "456"}));

    fixture.shop->SetThread({});
    const auto cleared = fixture.makeShop();
    QVERIFY(cleared->threads().isEmpty());
}

void ShopTest::stashIndexFailureReleasesTheSubmitLock()
{
    // A classified failure must release the submit lock. Before the facade
    // this function open-coded a transport check, a status check, and a
    // parse check; now it branches once on a FetchError — and the pin that
    // matters is that the branch still resets m_submitting, or the shop
    // could never be submitted again without a restart.
    ShopFixture fixture;
    armForSubmission(fixture);

    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(fixture.rateLimiter->pendingFuture(0).endpoint,
             QString("https://www.pathofexile.com/character-window/get-stash-items"));

    fixture.rateLimiter->reject(0, RateLimit::FetchError::Kind::Http, "HTTP status 503");
    drainEvents();

    // The failure stops the update: nothing is sent to the forum.
    QCOMPARE(fixture.networkManager->count(), 0);

    // The lock is released: a second submission starts a new index request.
    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
}

void ShopTest::stashIndex403RejectsPoeSession()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy rejected(fixture.shop.get(), &Shop::PoeSessionRejected);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);

    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->reject(0, RateLimit::FetchError::Kind::Http, "HTTP status 403", 403);
    drainEvents();

    QCOMPARE(rejected.count(), 1);
    QCOMPARE(warnings.count(), 1);
    QVERIFY(!fixture.shop->auto_update());
    QCOMPARE(fixture.networkManager->count(), 0);
}

void ShopTest::stashIndexSuccessProceedsToShopSubmission()
{
    // The success path parses below the facade and carries on into the forum
    // submission, which goes through the NetworkManager rather than the
    // limiter. The forum GET is the pin that actually proves the continuation
    // RAN: the submission lock staying held would look identical if the
    // continuation had never executed at all.
    ShopFixture fixture;
    armForSubmission(fixture);

    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(fixture.networkManager->count(), 0);

    fixture.rateLimiter->resolve(0, R"({"tabs":[{"n":"one","i":0,"id":"0123456789abcdef"}]})");
    drainEvents();

    // Exactly one forum request: the edit-thread page for the single
    // configured thread, fetched for its CSRF token.
    QCOMPARE(fixture.networkManager->count(), 1);
    QCOMPARE(fixture.networkManager->sent(0).op, QNetworkAccessManager::GetOperation);
    QCOMPARE(fixture.networkManager->sent(0).request.url().toString(),
             QString("https://www.pathofexile.com/forum/edit-thread/123"));

    // And the lock is still held while that submission is in flight.
    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
}

void ShopTest::editPage403RejectsPoeSession()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy rejected(fixture.shop.get(), &Shop::PoeSessionRejected);

    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QCOMPARE(fixture.networkManager->count(), 1);

    fixture.networkManager->sent(0).reply->finish({},
                                                  403,
                                                  QNetworkReply::ContentAccessDenied,
                                                  QByteArray("Content Denied"));
    drainEvents();

    QCOMPARE(rejected.count(), 1);
    QVERIFY(!fixture.shop->auto_update());
    QCOMPARE(fixture.networkManager->count(), 1);
}

void ShopTest::shopPost403RejectsPoeSession()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy rejected(fixture.shop.get(), &Shop::PoeSessionRejected);

    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, kEditThreadPage);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);

    fixture.networkManager->sent(1).reply->finish({},
                                                  403,
                                                  QNetworkReply::ContentAccessDenied,
                                                  QByteArray("Content Denied"));
    drainEvents();

    QCOMPARE(rejected.count(), 1);
    QVERIFY(!fixture.shop->auto_update());
}

void ShopTest::continuationDoesNotRunAfterShopDestruction()
{
    // The continuation is context-bound to the Shop (R6-2). Destroying the
    // Shop while its index request is in flight must drop the continuation
    // rather than run a handler against freed memory — the lifetime
    // property the design calls out explicitly. Completing the promise
    // afterwards must be safe and must reach nothing.
    ShopFixture fixture;
    armForSubmission(fixture);

    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    fixture.shop.reset();
    drainEvents();

    // Completing after destruction is safe, and the handler never runs —
    // proved directly: a continuation that ran would have gone on to fetch
    // the forum edit-thread page, so zero requests is the evidence. (No
    // crash alone would not distinguish "dropped" from "ran and happened
    // not to fault".)
    fixture.rateLimiter->resolve(0, R"({"tabs":[{"n":"one","i":0,"id":"0123456789abcdef"}]})");
    drainEvents();
    QCOMPARE(fixture.networkManager->count(), 0);
    // The promise did settle, so the test cannot pass by never completing.
    QVERIFY(fixture.rateLimiter->pendingFuture(0).promise == nullptr);
}

// M2 D8 (stage 2 foundation): monotonic input/cache revisions replace the
// single outdated flag. Completing job N advances only N's revision — an
// input expired to N+1 while N was active stays dirty through N's successful
// completion, and only a job that captured N+1 renders it clean. The old
// flag reset (shop.cpp's unconditional m_shop_data_outdated = false on
// render) would have marked the newer input clean here.
void ShopTest::olderSubmissionCannotCleanNewerInput()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    publishPricedItem(fixture, "item-a", 1);

    // Fresh shop: nothing rendered yet, so the cache is outdated.
    QVERIFY(fixture.shop->shop_data_outdated());

    // Job N captures the current input revision, then the input moves on
    // (a local edit path calls ExpireShopData) while N is still active.
    fixture.shop->SubmitShopToForum();
    fixture.shop->ExpireShopData();

    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    // N rendered and published the preview cache for its own revision; the
    // newer input revision is still dirty.
    QVERIFY(!fixture.shop->shop_data().isEmpty());
    QVERIFY(fixture.shop->shop_data_outdated());

    // Finishing N successfully still cannot clean the newer revision.
    completeForumSubmission(fixture, 0);
    QVERIFY(fixture.shop->shop_data_outdated());

    // A job that captures the newer revision renders it clean. (Same items,
    // so the unchanged-hash exit releases it without posting.)
    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(!fixture.shop->shop_data_outdated());
}

// M2 D8/R6-4 (stage 2 foundation): the active job's capture is immutable —
// a buyout edit made while the job is in flight does not reach its rendered
// output; the edit follows in a later submission. Today's continuation read
// live buyout state after the index arrived, so the edit would have leaked
// into the post.
void ShopTest::activeJobUnaffectedByLocalEdits()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    // The job captures the 1-chaos buyout at request time.
    fixture.shop->SubmitShopToForum();

    // A local edit while the job is active: reprice and expire, the
    // MainWindow buyout-edit path.
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->ExpireShopData();

    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    // The rendered output is the captured state, not the edited state.
    QVERIFY(!fixture.shop->shop_data().isEmpty());
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));
    QVERIFY(!fixture.shop->shop_data().first().contains(" 2 chaos"));

    // The job completes without re-rendering: the posted state is unchanged.
    completeForumSubmission(fixture, 0);
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));
}

// M2 D8/R2-1 (stage 2): submission input is captured by value at request
// time. Submission for update N is requested, the stash-index future is held,
// N+1 streams a delta that empties the published state — and the generated
// shop still reflects N's captured input. (Value capture also makes the
// rebase hazard moot: no shared Item object is retained, so a later
// FinishUpdate rebasing locations in place cannot reach the capture.)
void ShopTest::shopSubmissionUsesCapturedSnapshot()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    // Request submission for N: the capture happens now, the index is held.
    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // N+1 streams a delta emptying item-a's fetch source: the published
    // state no longer holds any postable item.
    fixture.itemsManager->OnTabRefreshed(item->location(), {});
    QVERIFY(fixture.itemsManager->items().empty());

    // The index arrives; the job renders its capture, not the published
    // state — the shop still holds the captured item.
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QVERIFY(!fixture.shop->shop_data().isEmpty());
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));
    QVERIFY(fixture.shop->shop_data().first().contains("linkItem"));
}

// M2 D8/R3-1 (stage 2): a forced manual request during an item refresh
// renders its captured published state even when the preview cache's
// revision still appears current — deltas deliberately do not advance the
// input revision (R4-2), so the old cached-data shortcut would have posted
// pre-delta text.
void ShopTest::manualSubmissionRendersCapturedPublishedState()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    const auto item_a = publishPricedItem(fixture, "item-a", 1);

    // A completed submission leaves a preview cache rendered from the
    // current input revision — the cache "appears current".
    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    completeForumSubmission(fixture, 0);
    QVERIFY(!fixture.shop->shop_data_outdated());
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));

    // A streamed delta replaces the published item with a repriced one; the
    // cache revision still appears current.
    auto item_b = std::make_shared<Item>(makeTestItem("item-b"));
    fixture.itemsManager->OnTabRefreshed(item_a->location(), {item_b});
    fixture.buyoutFixture.manager->Set(*item_b, makeChaosBuyout(2));
    QVERIFY(!fixture.shop->shop_data_outdated());

    // A forced manual submission renders its own capture of the current
    // published state instead of reusing the cached pre-delta text.
    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 2 chaos"));
    QVERIFY(!fixture.shop->shop_data().first().contains(" 1 chaos"));
}

// M2 D8/R1-1 (stage 7): automatic submission fires only on a CLEAN
// completed refresh delivered through the typed terminal event. A failed
// refresh, a completed-with-skips refresh, or auto-update disabled all
// submit nothing; a clean completion with auto-update on starts the
// submission (observable as the legacy stash-index fetch).
void ShopTest::shopSubmitsOnlyOnCleanCompletion()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);

    // A failed refresh never auto-posts.
    RateLimit::FetchError network_error;
    network_error.kind = RateLimit::FetchError::Kind::Network;
    fixture.shop->OnRefreshFinished(RefreshOutcome{FailedRefresh{network_error}});
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));

    // A completed-with-skips refresh never auto-posts: the skipped tabs'
    // published contents are stale and must not reach the forum as fresh.
    RateLimit::FetchError parse_error;
    parse_error.kind = RateLimit::FetchError::Kind::Parse;
    CompletedRefresh with_skips;
    with_skips.skipped.push_back(
        SkippedSource{FetchSourceKey{ItemLocationType::STASH, "stash00001"}, parse_error});
    fixture.shop->OnRefreshFinished(RefreshOutcome{with_skips});
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));

    // Auto-update off: even a clean completion submits nothing.
    fixture.shop->SetAutoUpdate(false);
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));

    // A clean completion with auto-update on submits: the stash-index
    // fetch goes out.
    fixture.shop->SetAutoUpdate(true);
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(fixture.rateLimiter->pendingFuture(0).endpoint,
             QString("https://www.pathofexile.com/character-window/get-stash-items"));
}

// M2 D8/R3-1 (stage 8): a clean refresh completing while a job is active
// becomes the waiting eligible capture; the active job's SUCCESS drains it,
// and the drained submission posts the newer captured state.
void ShopTest::newestCleanSnapshotSubmitsAfterActive()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    // Clean refresh 1: the automatic job goes active (index fetch out).
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // The published state changes; clean refresh 2 completes while the job
    // is active: captured NOW, held as the waiting capture — no second
    // index fetch yet.
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // The active job runs to a successful completion on its own capture.
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));
    completeForumSubmission(fixture, 0);

    // Success drains the waiting capture: a second index fetch goes out,
    // and the drained job renders the NEWER captured state.
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 2 chaos"));
    completeForumSubmission(fixture, 2);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2)); // nothing else waits
}

// M2 D8/R3-1 (stage 8): intermediate clean snapshots coalesce — the newest
// clean capture replaces the waiting one, and after the active job
// completes, exactly ONE drained submission posts the newest state.
void ShopTest::automaticSubmissionCoalescesLatestEligible()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // Two more clean refreshes while the job is active: the second replaces
    // the first as the single waiting capture.
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(3));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    completeForumSubmission(fixture, 0);

    // Exactly one drained job, rendering the NEWEST capture; the
    // intermediate clean snapshot is never posted.
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 3 chaos"));
    QVERIFY(!fixture.shop->shop_data().first().contains(" 2 chaos"));
    completeForumSubmission(fixture, 2);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
}

// M2 D8/R4-3 (stage 8): a terminal submission failure drains nothing and
// discards the waiting capture — automatic convergence halts until the
// next clean refresh or a manual request, which recaptures fresh state.
void ShopTest::failedSubmissionDoesNotDrainPendingAutomatic()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // The active job fails at its index fetch: no drain, nothing sent to
    // the forum, the waiting capture discarded.
    fixture.rateLimiter->reject(0, RateLimit::FetchError::Kind::Http, "HTTP status 503");
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(fixture.networkManager->count(), 0);

    // A manual request afterwards starts fresh — it recaptures the latest
    // published state rather than draining a stale snapshot.
    fixture.shop->SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 2 chaos"));
}

// M2 D8/R5-6 (stage 8): disabling shop auto-update drops the waiting
// automatic capture; the active job finishes normally but no new automatic
// job may start after the user turned automation off.
void ShopTest::disablingAutoUpdateDropsWaitingCapture()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    fixture.shop->SetAutoUpdate(false);

    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    completeForumSubmission(fixture, 0);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1)); // no drain
}

// M2 D8/R5-6 keep-and-drain (stage 8): a completed-with-skips or failed
// refresh arriving before the waiting clean capture drains does not
// invalidate it — the gate's invariant is cleanliness, not recency.
void ShopTest::skippedRefreshDoesNotInvalidateWaitingCapture()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // A completed-with-skips refresh arrives, snapshot first (as in
    // production: ItemsRefreshed then RefreshFinished), then a failed one.
    // Neither invalidates the waiting clean capture.
    fixture.shop->OnPublishedSnapshot();
    RateLimit::FetchError parse_error;
    parse_error.kind = RateLimit::FetchError::Kind::Parse;
    CompletedRefresh with_skips;
    with_skips.skipped.push_back(
        SkippedSource{FetchSourceKey{ItemLocationType::STASH, "stash00001"}, parse_error});
    fixture.shop->OnRefreshFinished(RefreshOutcome{with_skips});
    RateLimit::FetchError network_error;
    network_error.kind = RateLimit::FetchError::Kind::Network;
    fixture.shop->OnRefreshFinished(RefreshOutcome{FailedRefresh{network_error}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // The active job succeeds: the kept capture drains.
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    completeForumSubmission(fixture, 0);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(2));
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 2 chaos"));
    completeForumSubmission(fixture, 2);
}

// M2 D8/R6-4 (stage 8): an output-affecting local edit (ExpireShopData)
// drops the waiting automatic capture; the active job's immutable capture
// is deliberately unaffected and posts as captured.
void ShopTest::expireDropsWaitingCapture()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    const auto item = publishPricedItem(fixture, "item-a", 1);

    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(2));
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));

    // The user edits an output-affecting input: the waiting capture is
    // dropped — draining it after the edit would post pre-edit data.
    fixture.shop->ExpireShopData();

    // The active job is unaffected: it renders and posts its own capture.
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QVERIFY(fixture.shop->shop_data().first().contains(" 1 chaos"));
    completeForumSubmission(fixture, 0);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1)); // no drain
}

// #253: a shop that renders more pages than there are threads posts the
// pages that fit, tells the user how many more threads are needed, and does
// not persist the hash — so the next submission reports it again instead of
// seeing an unchanged hash and doing nothing.
void ShopTest::morePagesThanThreadsWarnsAndKeepsHash()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.buyoutFixture.data->Set("shop_hash", "previous-hash");

    // Two priced items in one tab, and a template padded so that one item
    // fits a 50000-character post and two do not: two pages.
    const ItemLocation tab = makeTestStashLocation();
    const QByteArray json_a = R"({"verified": false, "w": 1, "h": 1,
        "icon": "https://web.poecdn.com/image/test.png", "id": "page-a", "name": "",
        "typeLine": "Iron Ring", "baseType": "Iron Ring", "identified": true, "ilvl": 1,
        "x": 0, "y": 0})";
    QByteArray json_b = json_a;
    json_b.replace("page-a", "page-b").replace(R"("x": 0)", R"("x": 1)");
    const auto item_a = std::make_shared<Item>(makeTestItem(json_a.constData(), tab));
    const auto item_b = std::make_shared<Item>(makeTestItem(json_b.constData(), tab));
    fixture.itemsManager->OnItemsRefreshed({item_a, item_b}, {tab}, false);
    fixture.buyoutFixture.manager->Set(*item_a, makeChaosBuyout(1));
    fixture.buyoutFixture.manager->Set(*item_b, makeChaosBuyout(1));

    const qsizetype item_length = tab.GetForumCode("pc", "Standard", 0).size();
    const qsizetype header_length = QStringLiteral(R"([spoiler="~b/o 1 chaos"])").size();
    const qsizetype overhead = 19 + QStringLiteral("[/spoiler]").size();
    const qsizetype padding = 50000 - overhead - header_length - item_length - item_length / 2
                              - QStringLiteral("[items]").size();
    fixture.shop->SetShopTemplate("[items]" + QString(padding, 'x'));

    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QCOMPARE(fixture.shop->shop_data().size(), 2);

    // The one thread receives page one. Read the POST body before its reply
    // finishes and is released.
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, kEditThreadPage);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    QCOMPARE(fixture.networkManager->sent(1).request.url().toString(),
             QString("https://www.pathofexile.com/forum/edit-thread/123"));
    QCOMPARE(postedForm(fixture.networkManager->sent(1))
                 .queryItemValue("content", QUrl::FullyDecoded),
             fixture.shop->shop_data().first());
    fixture.networkManager->sent(1).reply->finish({}, 200, QNetworkReply::NoError, QByteArray("ok"));
    drainEvents();

    // Nothing else was sent, the user was told once, and the hash was kept.
    QCOMPARE(fixture.networkManager->count(), 2);
    QCOMPARE(warnings.count(), 1);
    QVERIFY(warnings.first().first().toString().contains("1 more"));
    QCOMPARE(fixture.buyoutFixture.data->Get("shop_hash"), QString("previous-hash"));
}

// #250: when the buyout store could not be read, the manager holds no
// prices, and a clean refresh with auto-update on would overwrite every
// forum thread with "Empty". Neither path may post; the user is told —
// once per session for the automatic path, every time for the manual one.
void ShopTest::unloadedBuyoutsBlockSubmission()
{
    ShopFixture fixture;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createStoreWithoutItemBuyouts(QDir(dir.path()));

    StoreBackedShop backed(fixture, QDir(dir.path()));
    QVERIFY(backed.store.isUsable());
    QVERIFY(!backed.buyouts.BuyoutsLoaded());

    armStoreBackedShop(fixture, backed);
    backed.shop.SetAutoUpdate(true);
    fixture.buyoutFixture.data->Set("shop_hash", "previous-hash");
    const auto item = std::make_shared<Item>(makeTestItem("item-250"));
    backed.items.OnItemsRefreshed({item}, {item->location()}, false);

    QSignalSpy warnings(&backed.shop, &Shop::UserWarning);
    for (int i = 0; i < 2; ++i) {
        backed.shop.OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        drainEvents();
        if (fixture.rateLimiter->futureCount() > size_t(i)) {
            fixture.rateLimiter->resolve(i, kStashIndexJson);
            drainEvents();
        }
    }
    QCOMPARE(fixture.networkManager->count(), 0);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));
    QCOMPARE(warnings.count(), 1);
    QVERIFY(warnings.first().first().toString().contains("not updated"));

    backed.shop.SubmitShopToForum(true);
    backed.shop.SubmitShopToForum(true);
    drainEvents();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));
    QCOMPARE(fixture.networkManager->count(), 0);
    QCOMPARE(warnings.count(), 3);
    QCOMPARE(fixture.buyoutFixture.data->Get("shop_hash"), QString("previous-hash"));
}

// A healthy store with no prices is loaded, and an empty shop still posts
// "Empty": that is what the user asked for when they hold no priced items.
void ShopTest::healthyEmptyStoreStillPostsEmpty()
{
    ShopFixture fixture;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    StoreBackedShop backed(fixture, QDir(dir.path()));
    QVERIFY(backed.store.isUsable());
    QVERIFY(backed.buyouts.BuyoutsLoaded());

    armStoreBackedShop(fixture, backed);
    QSignalSpy warnings(&backed.shop, &Shop::UserWarning);
    backed.shop.SubmitShopToForum();
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    QCOMPARE(fixture.networkManager->count(), 1);
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, kEditThreadPage);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    QCOMPARE(postedForm(fixture.networkManager->sent(1)).queryItemValue("content"),
             QString("Empty"));
    QCOMPARE(warnings.count(), 0);
}

QTEST_MAIN(ShopTest)

#include "tst_shop.moc"
