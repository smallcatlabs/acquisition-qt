// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <glaze/glaze.hpp>

#include "cacheload.h"
#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "poe/types/item.h"
#include "testfixtures.h"
#include "util/glaze_qt.h" // IWYU pragma: keep

class WorkerParseTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesCachedStashItems();
    void specialChildItemsKeyedByChildFetchId();
    void characterAlongsideSpecialChildLoads();
    void unusableStoreLoadsNothingAndSaysSo();
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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString account = "worker-parse-account";
    const QString realm = "pc";
    const QString league = "Worker Parse League";
    const QString dataDir = dir.filePath("data");

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

    const CachedItems result = LoadCachedItems(QDir(dataDir),
                                               makeTestIdentity(account, realm, league),
                                               {});
    QVERIFY(result.loaded);

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
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString account = "worker-parse-account-2";
    const QString realm = "pc";
    const QString league = "Worker Parse League";
    const QString dataDir = dir.filePath("data");

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

    const CachedItems result = LoadCachedItems(QDir(dataDir),
                                               makeTestIdentity(account, realm, league),
                                               {});
    QVERIFY(result.loaded);

    // Only the parent is a tab; the child is not.
    QCOMPARE(result.tabs.size(), 1);
    QCOMPARE(result.tabs[0].id(), parent.id);

    // The child's item displays under the parent but is keyed by the child.
    QCOMPARE(result.items.size(), 1);
    QCOMPARE(result.items[0]->id(), QString("child-item"));
    QCOMPARE(result.items[0]->location().id(), parent.id);
    QCOMPARE(result.items[0]->location().fetch_id(), child.id);
}

// #294: a character cached alongside a special stash's child loads as its own
// tab with its own items; the child row is not a tab and its items stay
// under the parent.
void WorkerParseTest::characterAlongsideSpecialChildLoads()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString account = "worker-parse-account-3";
    const QString realm = "pc";
    const QString league = "Worker Parse League";
    const QString dataDir = dir.filePath("data");

    poe::StashTab parent;
    parent.id = "parent0001";
    parent.name = "Uniques";
    parent.type = "UniqueStash";
    parent.index = 0;

    poe::StashTab child;
    child.id = "child00001";
    child.name = "Rings";
    child.type = "UniqueStash";
    child.parent = parent.id;
    child.items = std::vector<poe::Item>{makePoeItem("child-item")};

    poe::Character character;
    character.id = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    character.name = "ParseTester";
    character.realm = realm;
    character.class_ = "Witch";
    character.league = league;
    character.level = 90;
    character.experience = 0;
    character.inventory = std::vector<poe::Item>{makePoeItem("character-item")};

    {
        UserStore store(QDir(dataDir), account);
        QVERIFY(store.stashes().saveStashList({parent, child}, realm, league));
        QVERIFY(saveStashFixture(store.stashes(), child, realm, league));
        QVERIFY(store.characters().saveCharacterList({character}));
        QVERIFY(saveCharacterFixture(store.characters(), character));
    }

    const CachedItems result = LoadCachedItems(QDir(dataDir),
                                               makeTestIdentity(account, realm, league),
                                               {});
    QVERIFY(result.loaded);

    QCOMPARE(result.tabs.size(), 2);
    QCOMPARE(result.tabs[0].id(), parent.id);
    QCOMPARE(result.tabs[1].id(), character.id);
    QCOMPARE(result.tabs[1].type(), ItemLocationType::CHARACTER);
    QCOMPARE(result.tabs[1].character(), character.name);
    QVERIFY(result.tab_id_index.contains(parent.id));
    QVERIFY(result.tab_id_index.contains(character.id));
    QVERIFY(!result.tab_id_index.contains(child.id));

    QCOMPARE(result.items.size(), 2);
    QCOMPARE(result.items[0]->id(), QString("child-item"));
    QCOMPARE(result.items[0]->location().id(), parent.id);
    QCOMPARE(result.items[0]->location().fetch_id(), child.id);
    QCOMPARE(result.items[1]->id(), QString("character-item"));
    QCOMPARE(result.items[1]->location().type(), ItemLocationType::CHARACTER);
    QCOMPARE(result.items[1]->location().id(), character.id);
    QCOMPARE(result.items[1]->location().fetch_id(), character.id);
    QCOMPARE(result.items[1]->location().character(), character.name);
}

// A store the load cannot open (here: written by a newer build) gives an
// empty result marked not loaded, and the load leaves the file as it was.
void WorkerParseTest::unusableStoreLoadsNothingAndSaysSo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString account = "worker-parse-account-4";
    QDir data_dir(dir.filePath("data"));
    QVERIFY(data_dir.mkpath("."));
    const QString file = data_dir.filePath("userstore-" + account + ".db");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "newer-store");
        db.setDatabaseName(file);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("PRAGMA user_version = 999"));
        db.close();
    }
    QSqlDatabase::removeDatabase("newer-store");

    const CachedItems result = LoadCachedItems(data_dir,
                                               makeTestIdentity(account, "pc", "Standard"),
                                               {});
    QVERIFY(!result.loaded);
    QVERIFY(result.tabs.empty());
    QVERIFY(result.items.empty());

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "newer-store-check");
        db.setDatabaseName(file);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("PRAGMA user_version") && q.next());
        QCOMPARE(q.value(0).toInt(), 999);
        db.close();
    }
    QSqlDatabase::removeDatabase("newer-store-check");
}

QTEST_GUILESS_MAIN(WorkerParseTest)

#include "tst_workerparse.moc"
