// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QBuffer>
#include <QFile>
#include <QMap>
#include <QSettings>
#include <QSqlQuery>
#include <QStandardPaths>

#include <memory>
#include <sstream>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "datastore/userstore.h"
#include "fakenetwork.h"
#include "fakenetworkmanager.h"
#include "itemsmanager.h"
#include "mainwindowfixture.h"
#include "poe/poeapiclient.h"
#include "shop.h"
#include "shoprender.h"
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
        itemsManager = std::make_unique<ItemsManager>(*settings, *buyoutFixture.manager);
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
    void morePagesThanThreadsWarnsWithoutReposting();
    void automaticShortfallWarnsOncePerSession();

    // #250: the shop refuses to post while the buyouts did not load.
    void unloadedBuyoutsBlockSubmission();
    void healthyEmptyStoreStillPostsEmpty();

    // #315: a forum reply never reaches the log beyond an error item's
    // text and the page's length.
    void forumErrorReplyIsNotLogged_data();
    void forumErrorReplyIsNotLogged();

    // #258: every form value reaches the forum as the user wrote it.
    void formValuesArePercentEncoded();
    void titleFromTheEditPageIsPostedDecoded();

    // #279: one admission gate. A manual request is always told why it
    // was refused; an automatic one hears each reason once per session.
    void automaticRefusalWarnsOncePerReason();
    void manualRequestWhileUpdatingTellsTheUser();
    void rejectedSessionReportsAutoUpdateOff();
    // tst_mainwindow has no shop test to sit beside, so the menu check
    // lives here with the main-window fixture.
    void autoUpdateMenuFollowsTheSetting();

    // #300: the rendered pages, pinned exactly before the renderer moves
    // out of Shop.
    void renderOnePage();
    void renderTwoPagesAtThePostLimit();
    void renderGroupsMixedBuyouts();
    void renderCharacterItem();
    void renderLeavesOutGameAndUnindexedItems();
    void renderShopNeverPostsAGamePrice();

    // #258: one failure exit, and a bounded conversation with the forum.
    void everyFailureEndsInAStatusAndANotice_data();
    void everyFailureEndsInAStatusAndANotice();
    void editPageTransportFailureDoesNotBlameTheThread();
    void expiredTokenRetriesAreBounded();
    void rateLimitedPostRefetchesTheEditPage();
    void longRateLimitFailsWithTheWait();
    void automaticFailureWarnsOncePerSession();

    // Review of #258: a rejected session is the status or the page's own
    // heading, never a phrase somewhere in the page's text.
    void phraseInPostTextIsNotARejectedSession_data();
    void phraseInPostTextIsNotARejectedSession();
    void loggedOutHeadingIsARejectedSession();
};

namespace {

    // The thread the captured forum pages belong to (tests/fixtures/forum/):
    // the page a successful edit returns links to it.
    constexpr const char *kThread = "3445874";

    // A Shop ready to submit: threads set and a POESESSID present, the two
    // preconditions SubmitShopToForum checks before it indexes anything.
    void armForSubmission(ShopFixture &fixture)
    {
        fixture.shop->SetThread({kThread});
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

    // Pages the forum sent, captured by the maintainer and trimmed to their
    // content block (each file starts with a comment saying what was
    // trimmed and scrubbed): the edit-thread page a submission GETs, and
    // the thread view the forum answers a successful edit POST with.
    QByteArray forumPage(const QString &name)
    {
        QFile file(QFINDTESTDATA("fixtures/forum/" + name));
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("tst_shop: cannot open the forum fixture %s", qPrintable(name));
        }
        return file.readAll();
    }
    QByteArray editThreadPage()
    {
        return forumPage("edit-thread.html");
    }
    QByteArray afterEditPage()
    {
        return forumPage("after-edit-thread.html");
    }

    // The scrubbed hash value in edit-thread.html, and its title as the
    // page's value attribute carries it.
    constexpr const char *kEditPageHash
        = "0123456789abcdef0123456789abcdef-fedcba9876543210fedcba9876543210";
    constexpr const char *kEditPageTitleAttribute
        = "value=\"GERWARIC&#x27;s&#x20;Standard&#x20;Shop\"";

    // Walk the active job's forum flow to completion: finish the edit-page
    // GET at m_sent index `get_index`, wait out the 500ms CSRF delay for the
    // POST, and finish the POST cleanly.
    void completeForumSubmission(ShopFixture &fixture, int get_index)
    {
        QCOMPARE(fixture.networkManager->sent(get_index).op, QNetworkAccessManager::GetOperation);
        fixture.networkManager->sent(get_index).reply->finish({},
                                                              200,
                                                              QNetworkReply::NoError,
                                                              editThreadPage());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), get_index + 2, 5000);
        QCOMPARE(fixture.networkManager->sent(get_index + 1).op,
                 QNetworkAccessManager::PostOperation);
        fixture.networkManager->sent(get_index + 1)
            .reply->finish({}, 200, QNetworkReply::NoError, afterEditPage());
        drainEvents();
    }

    // The form fields of a forum POST, decoded the way a server decodes
    // application/x-www-form-urlencoded (the WHATWG URL standard's form
    // parser): '+' is a space, then percent-decoding as UTF-8. Deliberately
    // not QUrlQuery, which keeps '+' as itself and so cannot see the encoder
    // sending one. QNetworkAccessManager::post(request, QByteArray) wraps the
    // body in a QBuffer parented to the reply.
    QMap<QString, QString> postedForm(const FakeNetworkManager::Sent &sent)
    {
        QMap<QString, QString> fields;
        const auto *buffer = sent.reply ? sent.reply->findChild<QBuffer *>() : nullptr;
        if (!buffer) {
            return fields;
        }
        const auto decode = [](QByteArray part) {
            part.replace('+', ' ');
            return QString::fromUtf8(QByteArray::fromPercentEncoding(part));
        };
        for (const QByteArray &pair : buffer->data().split('&')) {
            const qsizetype equals = pair.indexOf('=');
            if (equals < 0) {
                fields.insert(decode(pair), QString());
            } else {
                fields.insert(decode(pair.left(equals)), decode(pair.mid(equals + 1)));
            }
        }
        return fields;
    }

    // A Shop over a real per-account UserStore instead of the in-memory
    // buyout fixture, sharing the fixture's settings, network and datastore.
    struct StoreBackedShop
    {
        StoreBackedShop(ShopFixture &fixture, const QDir &dir)
            : store(dir, "TESTER#1234")
            , buyouts(*fixture.buyoutFixture.data, store.buyouts())
            , items(*fixture.settings, buyouts)
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
        backed.shop.SetThread({kThread});
        fixture.settings->setValue("session_id", "fake-poesessid");
        fixture.settings->setValue("account", "someone");
        fixture.settings->setValue("realm", "pc");
        fixture.settings->setValue("league", "Standard");
    }

    // Swaps the default logger for one writing to a string, the way
    // tst_securitylogging does (F42: the default logger is replaced, its
    // sinks are never touched), and restores the previous one.
    class LogCapture
    {
    public:
        LogCapture()
            : m_previous(spdlog::default_logger())
        {
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output);
            auto logger = std::make_shared<spdlog::logger>("shop-log-capture", sink);
            logger->set_level(spdlog::level::trace);
            logger->set_pattern("%v");
            spdlog::set_default_logger(std::move(logger));
        }
        ~LogCapture() { spdlog::set_default_logger(std::move(m_previous)); }
        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        QString text() const
        {
            spdlog::default_logger()->flush();
            return QString::fromStdString(m_output.str());
        }

    private:
        std::ostringstream m_output;
        std::shared_ptr<spdlog::logger> m_previous;
    };

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
             QString("https://www.pathofexile.com/forum/edit-thread/") + kThread);

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
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
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

namespace {

    // Two priced items in one tab and one thread, with a template padded so
    // that the shop renders two pages.
    void armTwoPageShop(ShopFixture &fixture)
    {
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
    }

} // namespace

// #253: a shop that renders more pages than there are threads posts the
// pages that fit and tells the user how many more threads are needed. The
// hash is saved as usual, so unchanged pages are not posted again, and the
// warning is raised before the unchanged-hash exit so a standing shortfall
// is still reported.
void ShopTest::morePagesThanThreadsWarnsWithoutReposting()
{
    ShopFixture fixture;
    armTwoPageShop(fixture);

    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QCOMPARE(fixture.shop->shop_data().size(), 2);

    // The one thread receives page one. Read the POST body before its reply
    // finishes and is released.
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    QCOMPARE(fixture.networkManager->sent(1).request.url().toString(),
             QString("https://www.pathofexile.com/forum/edit-thread/") + kThread);
    QCOMPARE(postedForm(fixture.networkManager->sent(1)).value("content"),
             fixture.shop->shop_data().first());
    fixture.networkManager->sent(1).reply->finish({}, 200, QNetworkReply::NoError, afterEditPage());
    drainEvents();

    // Nothing else was sent and the user was told. The hash is saved as for
    // any completed post, so unchanged pages are not posted again.
    QCOMPARE(fixture.networkManager->count(), 2);
    QCOMPARE(warnings.count(), 1);
    QVERIFY(warnings.first().first().toString().contains("1 more"));
    QVERIFY(fixture.buyoutFixture.data->Get("shop_hash") != QString("previous-hash"));

    // A second manual submission of the same shop posts nothing, but the
    // shortfall is still there, so the user is told again.
    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->resolve(1, kStashIndexJson);
    drainEvents();
    QCOMPARE(fixture.networkManager->count(), 2);
    QCOMPARE(warnings.count(), 2);
}

// #253: an automatic submission warns about a standing shortfall once per
// session, however many clean refreshes follow.
void ShopTest::automaticShortfallWarnsOncePerSession()
{
    ShopFixture fixture;
    armTwoPageShop(fixture);
    fixture.shop->SetAutoUpdate(true);

    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    fixture.networkManager->sent(1).reply->finish({}, 200, QNetworkReply::NoError, afterEditPage());
    drainEvents();
    QCOMPARE(warnings.count(), 1);

    // Later clean refreshes with the same shop post nothing and stay quiet.
    for (size_t i = 1; i < 3; ++i) {
        fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        drainEvents();
        fixture.rateLimiter->resolve(i, kStashIndexJson);
        drainEvents();
    }
    QCOMPARE(fixture.networkManager->count(), 2);
    QCOMPARE(warnings.count(), 1);
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
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    QCOMPARE(postedForm(fixture.networkManager->sent(1)).value("content"), QString("Empty"));
    QCOMPARE(warnings.count(), 0);
}

// The two replies below are NOT captures of forum pages: none is in the
// repository. Each is built from the shapes the parsing code documents —
// Shop::error_regex (an element whose class is "errors" or "input-error",
// then an <li> holding the message) and Util::GetCsrfToken's comment (the
// hash input may itself carry class="input-error").
void ShopTest::forumErrorReplyIsNotLogged_data()
{
    QTest::addColumn<QByteArray>("reply");

    // An error marker with no list item: the catch-all that logged the
    // whole page.
    QTest::newRow("unrecognised marker")
        << QByteArray("<form method=\"post\">\n"
                      "<input type=\"hidden\" name=\"hash\" value=\"leaky-csrf-token\">\n"
                      "<div class=\"errors\">Something the shop does not know.</div>\n"
                      "</form>");

    // The hash input carries the error class, so a match starting at the
    // first marker spans the token before reaching the list item.
    QTest::newRow("error class on the hash input")
        << QByteArray("<form method=\"post\">\n"
                      "<input type=\"hidden\" name=\"hash\" class=\"input-error\" "
                      "value=\"leaky-csrf-token\">\n"
                      "<ul class=\"errors\"><li>Something the shop does not know.</li></ul>\n"
                      "</form>");
}

void ShopTest::forumErrorReplyIsNotLogged()
{
    QFETCH(QByteArray, reply);

    ShopFixture fixture;
    armForSubmission(fixture);
    LogCapture log;

    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    fixture.networkManager->sent(1).reply->finish({}, 200, QNetworkReply::NoError, reply);
    drainEvents();

    const QString logged = log.text();
    QVERIFY2(!logged.contains("leaky-csrf-token"), qPrintable(logged));
    QVERIFY2(!logged.contains("<form"), qPrintable(logged));
}

// The forum decodes the POST body as a form: a '+' the encoder leaves
// alone arrives as a space, and a "%41" it leaves alone arrives as "A".
// The title comes from the edit page, HTML-escaped as the page carries it.
void ShopTest::formValuesArePercentEncoded()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    publishPricedItem(fixture, "item-a", 1);
    fixture.shop->SetShopTemplate("[items] +1 & 50% %41 é");

    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QByteArray edit_page = editThreadPage();
    QVERIFY(edit_page.contains(kEditPageTitleAttribute));
    edit_page.replace(kEditPageTitleAttribute,
                      "value=\"WTS&#x20;+1&#x20;gems&#x20;&amp;&#x20;50%&#x20;off\"");
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, edit_page);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);

    const auto form = postedForm(fixture.networkManager->sent(1));
    QCOMPARE(form.value("content"), fixture.shop->shop_data().first());
    QVERIFY(form.value("content").endsWith("[/spoiler] +1 & 50% %41 é"));
    QCOMPARE(form.value("title"), QString("WTS +1 gems & 50% off"));
    QCOMPARE(form.value("hash"), QString(kEditPageHash));
    QCOMPARE(form.value("notify_owner"), QString("0"));
    QCOMPARE(form.value("submit"), QString("Submit"));
}

// Clean refreshes with automatic updates on and a precondition missing:
// each missing precondition is reported once, however many refreshes
// follow; a manual request is told every time.
void ShopTest::automaticRefusalWarnsOncePerReason()
{
    ShopFixture fixture;
    fixture.settings->setValue("session_id", "fake-poesessid");
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);

    // No threads.
    for (int i = 0; i < 2; ++i) {
        fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        drainEvents();
    }
    QCOMPARE(warnings.count(), 1);
    QVERIFY(warnings.last().first().toString().contains("No forum threads"));

    // Threads, but no POESESSID: a different reason, reported once.
    fixture.shop->SetThread({kThread});
    fixture.settings->remove("session_id");
    for (int i = 0; i < 2; ++i) {
        fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        drainEvents();
    }
    QCOMPARE(warnings.count(), 2);
    QVERIFY(warnings.last().first().toString().contains("POESESSID"));

    // The user asked: told again.
    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(warnings.count(), 3);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(0));
}

void ShopTest::manualRequestWhileUpdatingTellsTheUser()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);

    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(warnings.count(), 0);

    // A second request while the first is still running is refused, and
    // the user hears why.
    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), size_t(1));
    QCOMPARE(warnings.count(), 1);
    QVERIFY(warnings.first().first().toString().contains("already"));
}

void ShopTest::rejectedSessionReportsAutoUpdateOff()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    QSignalSpy changes(fixture.shop.get(), &Shop::AutoUpdateChanged);
    fixture.shop->SetAutoUpdate(true);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(changes.last().first().toBool(), true);

    fixture.shop->SubmitShopToForum();
    fixture.rateLimiter->reject(0, RateLimit::FetchError::Kind::Http, "HTTP status 403", 403);
    drainEvents();

    QVERIFY(!fixture.shop->auto_update());
    QCOMPARE(changes.count(), 2);
    QCOMPARE(changes.last().first().toBool(), false);
}

// The window's own network stack is real, so nothing here may submit: the
// setting is changed directly, as the rejection path above changes it.
void ShopTest::autoUpdateMenuFollowsTheSetting()
{
    // MainWindow's log panel attaches through the "main" logger's sink hub
    // (F42), as in tst_mainwindow.
    auto hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    auto main_logger = std::make_shared<spdlog::logger>("main");
    main_logger->sinks().push_back(hub);
    spdlog::register_logger(main_logger);
    const auto drop_main = qScopeGuard([] { spdlog::drop("main"); });

    {
        MainWindowFixture fixture;
        auto *action = fixture.window->findChild<QAction *>("actionSetAutomaticallyShopUpdate");
        QVERIFY(action);
        QVERIFY(!action->isChecked());

        action->trigger();
        QVERIFY(action->isChecked());
        QVERIFY(fixture.shop->auto_update());

        fixture.shop->SetAutoUpdate(false);
        QVERIFY(!action->isChecked());

        fixture.shop->SetAutoUpdate(true);
        QVERIFY(action->isChecked());
    }
}

namespace {

    // The legacy stash index as the website sends it (T24: `n`, `i`, `id`):
    // the second tab is the fourth the website lists, so its items link as
    // Stash4. A third tab, "stash00003", is deliberately absent.
    constexpr const char *kGoldenIndexJson = R"({"tabs":[{"n":"Test Tab","i":0,"id":"stash00001"},)"
                                             R"({"n":"Second Tab","i":3,"id":"stash00002"}]})";

    // An item at (x, 0) in `loc`. A stash item's inventoryId is "Stash1"
    // whatever its tab (T13); a character item's is its slot.
    std::shared_ptr<Item> makeShopItem(const char *id,
                                       const ItemLocation &loc,
                                       int x,
                                       const char *inventory_id = "Stash1")
    {
        const QByteArray json = QString(R"({"verified": false, "w": 1, "h": 1,
            "icon": "https://web.poecdn.com/image/test.png", "id": "%1", "name": "",
            "typeLine": "Iron Ring", "baseType": "Iron Ring", "identified": true,
            "ilvl": 1, "x": %2, "y": 0, "inventoryId": "%3"})")
                                    .arg(id)
                                    .arg(x)
                                    .arg(inventory_id)
                                    .toUtf8();
        return std::make_shared<Item>(makeTestItem(json.constData(), loc));
    }

    Buyout makeBuyout(double value,
                      Buyout::BuyoutType type,
                      CurrencyType currency = CurrencyType::CURRENCY_CHAOS_ORB,
                      Buyout::BuyoutSource source = Buyout::BUYOUT_SOURCE_MANUAL)
    {
        Buyout buyout(value, type, currency, QDateTime::fromSecsSinceEpoch(1));
        buyout.source = source;
        return buyout;
    }

    // Publish the items with their buyouts, submit, answer the index, and
    // return the pages the job rendered. The edit-page GET is left open.
    QStringList renderedPages(ShopFixture &fixture,
                              const std::vector<std::pair<std::shared_ptr<Item>, Buyout>> &priced,
                              const std::vector<ItemLocation> &tabs)
    {
        armForSubmission(fixture);
        Items items;
        for (const auto &[item, buyout] : priced) {
            items.push_back(item);
        }
        fixture.itemsManager->OnItemsRefreshed(items, tabs, false);
        for (const auto &[item, buyout] : priced) {
            fixture.buyoutFixture.manager->Set(*item, buyout);
        }
        fixture.shop->SubmitShopToForum(true);
        fixture.rateLimiter->resolve(0, kGoldenIndexJson);
        drainEvents();
        return fixture.shop->shop_data();
    }

    // Note for every expected page below: today's renderer closes each
    // price spoiler when the next price starts, but never closes the last
    // one, so a page ends with a single "[/spoiler]" and the page spoiler
    // the template wraps around it stays open. These tests pin that output
    // unchanged across the renderer's move; the defect is reported, not
    // fixed here.
    QString stashLink(int stash_number, int x)
    {
        return QString(R"([linkItem location="Stash%1" league="Standard" x="%2" y="0" realm="pc"])")
            .arg(stash_number)
            .arg(x);
    }

} // namespace

void ShopTest::renderOnePage()
{
    ShopFixture fixture;
    const ItemLocation tab = makeTestStashLocation();
    const QStringList pages = renderedPages(fixture,
                                            {{makeShopItem("one", tab, 0),
                                              makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)}},
                                            {tab});
    QCOMPARE(pages,
             QStringList{R"([spoiler][spoiler=" ~b/o  1 chaos"])" + stashLink(1, 0) + "[/spoiler]"});
}

// T23: a post holds at most 50,000 characters. The template is padded so
// that one item fits a page and two do not.
void ShopTest::renderTwoPagesAtThePostLimit()
{
    ShopFixture fixture;
    armTwoPageShop(fixture);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    const QString padding = fixture.shop->shop_template().mid(QStringLiteral("[items]").size());
    const QString head = R"([spoiler][spoiler=" ~b/o  1 chaos"])";
    // A page cut at the limit closes its price spoiler; the last page does
    // not (see stashLink).
    const QStringList expected{head + stashLink(1, 0) + "[/spoiler][/spoiler]" + padding,
                               head + stashLink(1, 1) + "[/spoiler]" + padding};
    QCOMPARE(fixture.shop->shop_data(), expected);
    for (const QString &page : expected) {
        QVERIFY(page.size() <= 50000);
    }
}

// One spoiler per price, ordered by buyout type, then currency, then value;
// items with the same price share a spoiler. A no-price item sits under an
// empty title (T21).
void ShopTest::renderGroupsMixedBuyouts()
{
    ShopFixture fixture;
    const ItemLocation tab = makeTestStashLocation();
    const QStringList pages = renderedPages(
        fixture,
        {{makeShopItem("two-chaos", tab, 0), makeBuyout(2, Buyout::BUYOUT_TYPE_BUYOUT)},
         {makeShopItem("one-chaos", tab, 1), makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)},
         {makeShopItem("fixed", tab, 2), makeBuyout(1, Buyout::BUYOUT_TYPE_FIXED)},
         {makeShopItem("divine", tab, 3),
          makeBuyout(1.5, Buyout::BUYOUT_TYPE_BUYOUT, CurrencyType::CURRENCY_DIVINE_ORB)},
         {makeShopItem("no-price", tab, 4), makeBuyout(0, Buyout::BUYOUT_TYPE_NO_PRICE)},
         {makeShopItem("one-chaos-too", tab, 5), makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)}},
        {tab});
    QCOMPARE(pages,
             QStringList{"[spoiler]"
                         R"([spoiler=" ~b/o  1 chaos"])"
                         + stashLink(1, 1) + stashLink(1, 5)
                         + "[/spoiler]"
                           R"([spoiler=" ~b/o  2 chaos"])"
                         + stashLink(1, 0)
                         + "[/spoiler]"
                           R"([spoiler=" ~b/o  1.5 divine"])"
                         + stashLink(1, 3)
                         + "[/spoiler]"
                           R"([spoiler=" ~price  1 chaos"])"
                         + stashLink(1, 2)
                         + "[/spoiler]"
                           R"([spoiler=""])"
                         + stashLink(1, 4) + "[/spoiler]"});
}

// A character item links by its slot and the character's name (T7, T8).
void ShopTest::renderCharacterItem()
{
    ShopFixture fixture;
    poe::Character character;
    character.id = "character-0001";
    character.name = "I_Exist";
    const ItemLocation inventory(character, 0);
    const QStringList pages = renderedPages(fixture,
                                            {{makeShopItem("armour", inventory, 0, "BodyArmour"),
                                              makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)}},
                                            {inventory});
    QCOMPARE(pages,
             QStringList{R"([spoiler][spoiler=" ~b/o  1 chaos"])"
                         R"([linkItem location="BodyArmour" character="I_Exist" x="0" y="0" )"
                         R"(realm="pc"])"
                         "[/spoiler]"});
}

// A price read from the game is never posted (AGENTS.md, T15). An item in a
// tab the stash index does not list cannot be linked and is left out.
void ShopTest::renderLeavesOutGameAndUnindexedItems()
{
    ShopFixture fixture;
    const ItemLocation first = makeTestStashLocation("stash00001", "Test Tab", 0);
    const ItemLocation second = makeTestStashLocation("stash00002", "Second Tab", 4);
    const ItemLocation unlisted = makeTestStashLocation("stash00003", "Unlisted Tab", 5);
    const QStringList pages = renderedPages(fixture,
                                            {{makeShopItem("manual", first, 0),
                                              makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)},
                                             {makeShopItem("game", first, 1),
                                              makeBuyout(1,
                                                         Buyout::BUYOUT_TYPE_BUYOUT,
                                                         CurrencyType::CURRENCY_CHAOS_ORB,
                                                         Buyout::BUYOUT_SOURCE_GAME)},
                                             {makeShopItem("second-tab", second, 0),
                                              makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)},
                                             {makeShopItem("unlisted", unlisted, 0),
                                              makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)}},
                                            {first, second, unlisted});
    QCOMPARE(pages,
             QStringList{R"([spoiler][spoiler=" ~b/o  1 chaos"])" + stashLink(1, 0)
                         + stashLink(4, 0) + "[/spoiler]"});
}

// The real edit page writes the title with hex character references, the
// spaces included; the POST carries it back as plain text.
void ShopTest::titleFromTheEditPageIsPostedDecoded()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    QVERIFY(editThreadPage().contains(kEditPageTitleAttribute));
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);

    const auto form = postedForm(fixture.networkManager->sent(1));
    QCOMPARE(form.value("title"), QString("GERWARIC's Standard Shop"));
    QCOMPARE(form.value("hash"), QString(kEditPageHash));
}

// AGENTS.md: prices set in the game are never posted to the forum. The
// renderer filters by itself, so a caller that hands it a game-sourced
// price still cannot get it posted; an item whose tab the stash index does
// not list comes back as unlinked instead of vanishing.
void ShopTest::renderShopNeverPostsAGamePrice()
{
    const ItemLocation tab = makeTestStashLocation();
    const ItemLocation unlisted = makeTestStashLocation("stash00003", "Unlisted Tab", 5);
    const auto manual = makeShopItem("manual", tab, 0);
    const auto game = makeShopItem("game", tab, 1);
    const auto lost = makeShopItem("lost", unlisted, 0);

    ShopRenderInput input;
    input.shop_template = "[items]";
    input.realm = "pc";
    input.league = "Standard";
    input.tab_index = {{"stash00001", 0}};
    input.items
        = {{game->PrettyName(),
            game->location(),
            makeBuyout(5,
                       Buyout::BUYOUT_TYPE_FIXED,
                       CurrencyType::CURRENCY_DIVINE_ORB,
                       Buyout::BUYOUT_SOURCE_GAME)},
           {manual->PrettyName(), manual->location(), makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)},
           {lost->PrettyName(), lost->location(), makeBuyout(1, Buyout::BUYOUT_TYPE_BUYOUT)}};

    const RenderedShop shop = RenderShop(input);
    QCOMPARE(shop.pages,
             QStringList{R"([spoiler][spoiler=" ~b/o  1 chaos"])" + stashLink(1, 0) + "[/spoiler]"});
    QVERIFY(!shop.pages.join("").contains("divine"));
    QCOMPARE(shop.unlinked.size(), size_t(1));
    QCOMPARE(shop.unlinked.front().location.id(), QString("stash00003"));

    // A game price alone renders nothing at all.
    input.items.erase(input.items.begin() + 1, input.items.end());
    QVERIFY(RenderShop(input).pages.isEmpty());
}

namespace {

    // Error replies to the edit POST. The maintainer has no capture of any
    // forum error page. Each of these is the captured edit page with one
    // element inserted after the form's opening <fieldset>: an error list
    // in the shape Shop's parsing documents (an element whose class is
    // "errors", holding the message in an <li>), with the message texts
    // its checks look for. The inserted element, its position, and the
    // message wording are derived from that code, not captured; everything
    // around them is what the forum sent.
    QByteArray editPageWithError(const QByteArray &error_element)
    {
        QByteArray page = editThreadPage();
        const QByteArray anchor
            = "<form method=\"POST\" class=\"t1&#x20;formForumThread\"><fieldset>";
        const qsizetype at = page.indexOf(anchor);
        if (at < 0) {
            qFatal("tst_shop: the edit page fixture has no form to insert an error into");
        }
        page.insert(at + anchor.size(), "\n" + error_element);
        return page;
    }
    QByteArray errorListPage(const QByteArray &message)
    {
        return editPageWithError("<ul class=\"errors\"><li>" + message + "</li></ul>");
    }

    // Where in the conversation a failure arrives, and what arrives.
    enum class FailAt { StashIndex, StashIndexBody, EditPage, Post };

    // Run a manual submission up to `at` and answer it with the failure.
    void failSubmissionAt(ShopFixture &fixture,
                          FailAt at,
                          int status,
                          QNetworkReply::NetworkError error,
                          const QByteArray &body)
    {
        armForSubmission(fixture);
        fixture.shop->SubmitShopToForum(true);
        if (at == FailAt::StashIndex) {
            fixture.rateLimiter->reject(0, RateLimit::FetchError::Kind::Network, "network error");
            drainEvents();
            return;
        }
        fixture.rateLimiter->resolve(0,
                                     at == FailAt::StashIndexBody ? QByteArray("{}")
                                                                  : QByteArray(kStashIndexJson));
        drainEvents();
        if (at == FailAt::StashIndexBody) {
            return;
        }
        if (at == FailAt::EditPage) {
            fixture.networkManager->sent(0).reply->finish({}, status, error, body);
            drainEvents();
            return;
        }
        fixture.networkManager->sent(0).reply->finish({},
                                                      200,
                                                      QNetworkReply::NoError,
                                                      editThreadPage());
        QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
        fixture.networkManager->sent(1).reply->finish({}, status, error, body);
        drainEvents();
    }

    QString lastStatus(const QSignalSpy &statuses)
    {
        return statuses.isEmpty() ? QString() : statuses.last().at(1).toString();
    }

} // namespace

Q_DECLARE_METATYPE(FailAt)

// Every way a submission can fail now ends in the one failure exit: the
// status line stops saying "Sending…", the user of a manual request is
// told why, and the job is released.
void ShopTest::everyFailureEndsInAStatusAndANotice_data()
{
    QTest::addColumn<FailAt>("at");
    QTest::addColumn<int>("status");
    QTest::addColumn<QNetworkReply::NetworkError>("error");
    QTest::addColumn<QByteArray>("body");

    const auto ok = QNetworkReply::NoError;
    QTest::newRow("stash index error") << FailAt::StashIndex << 0 << ok << QByteArray();
    QTest::newRow("stash index without tabs") << FailAt::StashIndexBody << 0 << ok << QByteArray();
    QTest::newRow("edit page timed out")
        << FailAt::EditPage << 0 << QNetworkReply::OperationCanceledError << QByteArray();
    QTest::newRow("edit page not found")
        << FailAt::EditPage << 404 << QNetworkReply::ContentNotFoundError
        << QByteArray("Not Found");
    QByteArray no_title = editThreadPage();
    no_title.replace("name=\"title\"", "name=\"subject\"");
    QTest::newRow("edit page without a title") << FailAt::EditPage << 200 << ok << no_title;
    QTest::newRow("post connection lost")
        << FailAt::Post << 0 << QNetworkReply::RemoteHostClosedError << QByteArray();
    QTest::newRow("post failed to find item")
        << FailAt::Post << 200 << ok << errorListPage("Failed to find item.");
    QTest::newRow("post unknown error")
        << FailAt::Post << 200 << ok << errorListPage("Something the shop does not know.");
    QTest::newRow("post rate limit without a wait")
        << FailAt::Post << 200 << ok << errorListPage("Rate limiting is in effect.");
    QTest::newRow("post error marker without a message")
        << FailAt::Post << 200 << ok
        << editPageWithError("<div class=\"errors\">Something went wrong.</div>");
    QTest::newRow("post input error")
        << FailAt::Post << 200 << ok
        << editPageWithError("<div class=\"input-error\">Something went wrong.</div>");

    // Replies with no known error that are not this thread's page either:
    // nothing shows the post was saved.
    QTest::newRow("post answered with nothing") << FailAt::Post << 200 << ok << QByteArray();
    QByteArray other_thread = afterEditPage();
    other_thread.replace(kThread, "1111111");
    QTest::newRow("post answered with another thread") << FailAt::Post << 200 << ok << other_thread;
    QTest::newRow("post answered with the edit form again")
        << FailAt::Post << 200 << ok << editThreadPage();
}

void ShopTest::everyFailureEndsInAStatusAndANotice()
{
    QFETCH(FailAt, at);
    QFETCH(int, status);
    QFETCH(QNetworkReply::NetworkError, error);
    QFETCH(QByteArray, body);

    ShopFixture fixture;
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    failSubmissionAt(fixture, at, status, error, body);

    QVERIFY2(!lastStatus(statuses).isEmpty(), "no status after the failure");
    QVERIFY2(!lastStatus(statuses).startsWith("Sending"), qPrintable(lastStatus(statuses)));
    QCOMPARE(warnings.count(), 1);
    // Nothing was posted, so the next update posts again.
    QCOMPARE(fixture.buyoutFixture.data->Get("shop_hash"), QString());

    // The job is released: a new request starts a new stash index fetch.
    const size_t before = fixture.rateLimiter->futureCount();
    fixture.shop->SubmitShopToForum(true);
    QCOMPARE(fixture.rateLimiter->futureCount(), before + 1);
}

// A timeout says nothing about the thread number: the notice must not send
// the user to check it.
void ShopTest::editPageTransportFailureDoesNotBlameTheThread()
{
    ShopFixture fixture;
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    failSubmissionAt(fixture,
                     FailAt::EditPage,
                     0,
                     QNetworkReply::OperationCanceledError,
                     QByteArray());

    QCOMPARE(warnings.count(), 1);
    const QString notice = warnings.first().first().toString() + lastStatus(statuses);
    QVERIFY2(!notice.contains(kThread), qPrintable(notice));
    QVERIFY2(!notice.contains("thread number", Qt::CaseInsensitive), qPrintable(notice));
    QVERIFY2(!notice.contains("thread ID", Qt::CaseInsensitive), qPrintable(notice));

    // Nor does a server error: the site failed, not the thread number.
    ShopFixture server_error;
    QSignalSpy server_warnings(server_error.shop.get(), &Shop::UserWarning);
    failSubmissionAt(server_error,
                     FailAt::EditPage,
                     503,
                     QNetworkReply::ServiceUnavailableError,
                     QByteArray("Service Unavailable"));
    QCOMPARE(server_warnings.count(), 1);
    const QString server_notice = server_warnings.first().first().toString();
    QVERIFY2(!server_notice.contains("thread number", Qt::CaseInsensitive),
             qPrintable(server_notice));
    QVERIFY2(server_notice.contains("503"), qPrintable(server_notice));
}

// The forum answering every POST with an expired token: each retry fetches
// the edit page again for a fresh token, and after three attempts the job
// fails. At most two requests per attempt.
void ShopTest::expiredTokenRetriesAreBounded()
{
    ShopFixture fixture;
    fixture.shop->SetRetryDelay(std::chrono::milliseconds(0));
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    armForSubmission(fixture);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    const QByteArray expired = errorListPage("Security token has expired.");
    // Answer whatever the shop sends until it stops sending.
    for (int answered = 0; answered < 20; ++answered) {
        if (answered == fixture.networkManager->count()) {
            QTest::qWait(1500);
            if (answered == fixture.networkManager->count()) {
                break;
            }
        }
        const auto &sent = fixture.networkManager->sent(answered);
        sent.reply->finish({},
                           200,
                           QNetworkReply::NoError,
                           sent.op == QNetworkAccessManager::GetOperation ? editThreadPage()
                                                                          : expired);
        drainEvents();
    }

    QCOMPARE(fixture.networkManager->count(), 6);
    for (int i = 0; i < 6; ++i) {
        QCOMPARE(fixture.networkManager->sent(i).op,
                 i % 2 == 0 ? QNetworkAccessManager::GetOperation
                            : QNetworkAccessManager::PostOperation);
    }
    QVERIFY2(!lastStatus(statuses).startsWith("Sending"), qPrintable(lastStatus(statuses)));
    QCOMPARE(warnings.count(), 1);
}

// A short wait the forum asks for is honoured, then the post starts over
// from the edit page and goes through.
void ShopTest::rateLimitedPostRefetchesTheEditPage()
{
    ShopFixture fixture;
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    armForSubmission(fixture);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);
    fixture.networkManager->sent(1)
        .reply->finish({},
                       200,
                       QNetworkReply::NoError,
                       errorListPage("Rate limiting is in effect. You must wait 1 seconds."));
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 3, 5000);
    QCOMPARE(fixture.networkManager->sent(2).op, QNetworkAccessManager::GetOperation);

    fixture.networkManager->sent(2).reply->finish({}, 200, QNetworkReply::NoError, editThreadPage());
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 4, 5000);
    fixture.networkManager->sent(3).reply->finish({}, 200, QNetworkReply::NoError, afterEditPage());
    drainEvents();
    QCOMPARE(lastStatus(statuses), QString("Shop threads updated"));
}

// A wait longer than a minute is not held inside the job: it fails, and the
// notice says how long to wait.
void ShopTest::longRateLimitFailsWithTheWait()
{
    ShopFixture fixture;
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);
    failSubmissionAt(fixture,
                     FailAt::Post,
                     200,
                     QNetworkReply::NoError,
                     errorListPage("Rate limiting is in effect. You must wait 120 seconds."));

    QCOMPARE(warnings.count(), 1);
    QVERIFY2(warnings.first().first().toString().contains("120 seconds"),
             qPrintable(warnings.first().first().toString()));
    QTest::qWait(200);
    QCOMPARE(fixture.networkManager->count(), 2);
}

// An automatic submission that fails tells the user once per session; the
// status line reports every failure.
void ShopTest::automaticFailureWarnsOncePerSession()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    QSignalSpy warnings(fixture.shop.get(), &Shop::UserWarning);

    for (size_t i = 0; i < 2; ++i) {
        fixture.shop->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        QCOMPARE(fixture.rateLimiter->futureCount(), i + 1);
        fixture.rateLimiter->reject(i, RateLimit::FetchError::Kind::Network, "network error");
        drainEvents();
    }
    QCOMPARE(warnings.count(), 1);
    QCOMPARE(statuses.count(), 2);
    QVERIFY(fixture.shop->auto_update());
}

void ShopTest::phraseInPostTextIsNotARejectedSession_data()
{
    QTest::addColumn<QByteArray>("phrase");
    QTest::addColumn<bool>("in_post_reply");
    for (const char *phrase : {"Login Required", "Permission Denied", "Content Denied"}) {
        QTest::addRow("%s in the edit page's textarea", phrase) << QByteArray(phrase) << false;
        QTest::addRow("%s in a post on the thread", phrase) << QByteArray(phrase) << true;
    }
}

// The edit page carries the user's own post in its textarea, and the page
// a successful edit returns shows everyone's posts in the thread. Either
// may contain the phrases a logged-out page uses; that is not a rejected
// session.
void ShopTest::phraseInPostTextIsNotARejectedSession()
{
    QFETCH(QByteArray, phrase);
    QFETCH(bool, in_post_reply);

    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy rejected(fixture.shop.get(), &Shop::PoeSessionRejected);
    QSignalSpy statuses(fixture.shop.get(), &Shop::StatusUpdate);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    QByteArray edit_page = editThreadPage();
    const QByteArray textarea_end = "</textarea>";
    QVERIFY(edit_page.contains(textarea_end));
    if (!in_post_reply) {
        edit_page.replace(textarea_end, " " + phrase + textarea_end);
    }
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, edit_page);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.networkManager->count(), 2, 5000);

    QByteArray after_edit = afterEditPage();
    const QByteArray post_body = "<div class=\"content\">";
    const qsizetype at = after_edit.lastIndexOf(post_body);
    QVERIFY(at > 0);
    if (in_post_reply) {
        after_edit.insert(at + post_body.size(), phrase);
    }
    fixture.networkManager->sent(1).reply->finish({}, 200, QNetworkReply::NoError, after_edit);
    drainEvents();

    QCOMPARE(rejected.count(), 0);
    QVERIFY(fixture.shop->auto_update());
    QCOMPARE(lastStatus(statuses), QString("Shop threads updated"));
}

// No logged-out page has been captured. This one is the captured edit
// page with its heading and title replaced by the phrase, the shape the
// session check looks for; the heading's shape is derived, not captured.
void ShopTest::loggedOutHeadingIsARejectedSession()
{
    ShopFixture fixture;
    armForSubmission(fixture);
    fixture.shop->SetAutoUpdate(true);
    QSignalSpy rejected(fixture.shop.get(), &Shop::PoeSessionRejected);
    fixture.shop->SubmitShopToForum(true);
    fixture.rateLimiter->resolve(0, kStashIndexJson);
    drainEvents();

    QByteArray page = editThreadPage();
    const QByteArray heading = "<h1 class=\"topBar last layoutBoxTitle\">Edit Thread</h1>";
    QVERIFY(page.contains(heading));
    page.replace(heading, "<h1 class=\"topBar last layoutBoxTitle\">Login Required</h1>");
    fixture.networkManager->sent(0).reply->finish({}, 200, QNetworkReply::NoError, page);
    drainEvents();

    QCOMPARE(rejected.count(), 1);
    QVERIFY(!fixture.shop->auto_update());
    QCOMPARE(fixture.networkManager->count(), 1);
}

QTEST_MAIN(ShopTest)

#include "tst_shop.moc"
