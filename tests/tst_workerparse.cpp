// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include <QSettings>

#include <glaze/glaze.hpp>

#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "itemsmanagerworker.h"
#include "poe/poeapiclient.h"
#include "poe/types/item.h"
#include "ratelimit/ratelimiter.h"
#include "testfixtures.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/networkmanager.h"

class WorkerParseTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesCachedStashItems();
    void specialChildItemsKeyedByChildFetchId();
};

static poe::Item makePoeItem(const char *id)
{
    const QByteArray json = QString(R"({
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
                                .arg(id)
                                .toUtf8();
    const std::string_view input(json.constData(), json.size());
    auto item = glz::read_json<poe::Item>(input);
    if (!item) {
        qFatal("makeItem: could not parse the test item json: %s",
               glz::format_error(item.error(), input).c_str());
    }
    return *item;
}

void WorkerParseTest::parsesCachedStashItems()
{
    BuyoutManagerFixture fixture;
    const QString account = "worker-parse-account";
    const QString realm = "pc";
    const QString league = "Worker Parse League";
    const QString dataDir = fixture.tempDir.filePath("data");

    poe::StashTab stash;
    stash.id = "stash00001";
    stash.name = "Parse Test";
    stash.type = "PremiumStash";
    stash.index = 0;
    stash.items = std::vector<poe::Item>{makePoeItem("worker-parse-item")};

    {
        UserStore store(QDir(dataDir), account);
        QVERIFY(store.stashes().saveStashList({stash}, realm, league));
        QVERIFY(saveStashFixture(store.stashes(), stash, realm, league));
    }

    QSettings settings(fixture.tempDir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("account", account);
    settings.setValue("realm", realm);
    settings.setValue("league", league);
    settings.sync();

    // The worker needs a PoeApiClient, but ParseCachedItems touches neither
    // it nor the limiter — this is a pure cache-parse test.
    NetworkManager network;
    RateLimiter rateLimiter(network);
    PoeApiClient api(rateLimiter);
    ItemsManagerWorker worker(settings, *fixture.manager, api);

    const ParseResult result = worker.ParseCachedItems(dataDir);

    QCOMPARE(result.tabs.size(), 1);
    QCOMPARE(result.tabs[0].id(), stash.id);
    QCOMPARE(result.tabs[0].tab_label(), stash.name);
    QVERIFY(result.tab_id_index.contains(stash.id));
    QCOMPARE(result.items.size(), 1);
    QCOMPARE(result.items[0]->id(), QString("worker-parse-item"));
    QCOMPARE(result.items[0]->location().id(), stash.id);
    // A normal tab's items are fetched by the tab itself (items-pipeline M1).
    QCOMPARE(result.items[0]->location().fetch_id(), stash.id);
}

// Items in children of MapStash/UniqueStash tabs display under the parent
// location but are fetched per child; the parse must key them by the child's
// id so the per-reply atomic replacement can target exactly one child
// (items-pipeline M1).
void WorkerParseTest::specialChildItemsKeyedByChildFetchId()
{
    BuyoutManagerFixture fixture;
    const QString account = "worker-parse-account-2";
    const QString realm = "pc";
    const QString league = "Worker Parse League";
    const QString dataDir = fixture.tempDir.filePath("data");

    poe::StashTab parent;
    parent.id = "parent0001";
    parent.name = "Maps";
    parent.type = "MapStash";
    parent.index = 0;

    poe::StashTab child;
    child.id = "child00001";
    child.name = "Tier 1";
    child.type = "MapStash";
    child.parent = parent.id;
    child.items = std::vector<poe::Item>{makePoeItem("child-item")};

    {
        UserStore store(QDir(dataDir), account);
        QVERIFY(store.stashes().saveStashList({parent, child}, realm, league));
        QVERIFY(saveStashFixture(store.stashes(), child, realm, league));
    }

    QSettings settings(fixture.tempDir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("account", account);
    settings.setValue("realm", realm);
    settings.setValue("league", league);
    settings.sync();

    // The worker needs a PoeApiClient, but ParseCachedItems touches neither
    // it nor the limiter — this is a pure cache-parse test.
    NetworkManager network;
    RateLimiter rateLimiter(network);
    PoeApiClient api(rateLimiter);
    ItemsManagerWorker worker(settings, *fixture.manager, api);

    const ParseResult result = worker.ParseCachedItems(dataDir);

    // Only the parent is a tab; the child is not.
    QCOMPARE(result.tabs.size(), 1);
    QCOMPARE(result.tabs[0].id(), parent.id);

    // The child's item displays under the parent but is keyed by the child.
    QCOMPARE(result.items.size(), 1);
    QCOMPARE(result.items[0]->id(), QString("child-item"));
    QCOMPARE(result.items[0]->location().id(), parent.id);
    QCOMPARE(result.items[0]->location().fetch_id(), child.id);
}

QTEST_GUILESS_MAIN(WorkerParseTest)

#include "tst_workerparse.moc"
