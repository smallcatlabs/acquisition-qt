// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest/QtTest>

#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "testfixtures.h"
#include "util/json_writers.h"

// Pins for the schema migration ladder: the 1 -> 2 step that introduced
// stashes.json_version / characters.json_version (and the payload-version
// check the column exists to serve), and the 2 -> 3 step that repairs
// databases created by v0.16.0-alpha.2 through alpha.6, whose composite
// primary keys break the ON CONFLICT(id) upserts and which predate the
// buyout tables.
//
// The two versions are deliberately independent: SCHEMA_VERSION describes the
// table shape and replays forward ('<'), json::PAYLOAD_VERSION describes the
// json inside json_data and is compared for equality so a blob written by a
// newer Acquisition is refetched after a downgrade rather than misparsed.

namespace {

    constexpr const char *kRealm = "pc";
    constexpr const char *kLeague = "Standard";
    constexpr const char *kAccount = "TESTER#1234";

    QString dbPath(const QTemporaryDir &dir)
    {
        return QDir(dir.path()).absoluteFilePath(QString("userstore-%1.db").arg(kAccount));
    }

    // The schema exactly as version 1 shipped it: no json_version column.
    void createVersion1Database(const QTemporaryDir &dir)
    {
        const QString connection = "v1-seed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            QVERIFY(db.open());

            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE stashes ("
                           " id TEXT PRIMARY KEY, realm TEXT NOT NULL, league TEXT NOT NULL,"
                           " parent TEXT, folder TEXT, name TEXT NOT NULL, type TEXT NOT NULL,"
                           " stash_index INTEGER,"
                           " meta_public INTEGER NOT NULL CHECK (meta_public IN (0,1)),"
                           " meta_folder INTEGER NOT NULL CHECK (meta_folder IN (0,1)),"
                           " meta_colour TEXT, listed_at TEXT,"
                           " json_fetched_at TEXT, json_data TEXT)"));
            QVERIFY(q.exec("CREATE TABLE characters ("
                           " id TEXT PRIMARY KEY, name TEXT NOT NULL, realm TEXT NOT NULL,"
                           " league TEXT, listed_at TEXT NOT NULL,"
                           " json_fetched_at TEXT, json_data TEXT)"));

            // A tab and a character with cached json, as a pre-3.29 install
            // would have them: readable bytes, but written by payload v1.
            QVERIFY(q.exec("INSERT INTO stashes (id, realm, league, name, type, meta_public,"
                           " meta_folder, json_fetched_at, json_data)"
                           " VALUES ('tab-a', 'pc', 'Standard', 'Tab A', 'PremiumStash', 0, 0,"
                           " '2026-01-01', '{\"id\":\"tab-a\",\"name\":\"Tab A\","
                           "\"type\":\"PremiumStash\"}')"));
            QVERIFY(q.exec("INSERT INTO characters (id, name, realm, league, listed_at,"
                           " json_fetched_at, json_data)"
                           " VALUES ('char-a', 'Alice', 'pc', 'Standard', '2026-01-01',"
                           " '2026-01-01', '{\"id\":\"char-a\",\"name\":\"Alice\","
                           "\"realm\":\"pc\"}')"));

            QVERIFY(q.exec("PRAGMA user_version=1"));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    // The schema exactly as v0.16.0-alpha.2 through alpha.6 shipped it:
    // composite primary keys that ON CONFLICT(id) cannot target, and no
    // buyout tables at all. Those alphas stamped user_version 1, so the 1 -> 2
    // step alone would leave the database broken while claiming it is current.
    void createCompositeKeyDatabase(const QTemporaryDir &dir)
    {
        const QString connection = "v1c-seed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            QVERIFY(db.open());

            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE stashes ("
                           " realm TEXT NOT NULL, league TEXT NOT NULL, id TEXT NOT NULL,"
                           " parent TEXT, folder TEXT, name TEXT NOT NULL, type TEXT NOT NULL,"
                           " stash_index INTEGER,"
                           " meta_public INTEGER NOT NULL DEFAULT 0 CHECK (meta_public IN (0,1)),"
                           " meta_folder INTEGER NOT NULL DEFAULT 0 CHECK (meta_folder IN (0,1)),"
                           " meta_colour TEXT, listed_at TEXT,"
                           " json_fetched_at TEXT, json_data TEXT,"
                           " PRIMARY KEY (realm, league, id))"));
            QVERIFY(q.exec("CREATE TABLE characters ("
                           " realm TEXT NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL,"
                           " league TEXT, listed_at TEXT NOT NULL,"
                           " json_fetched_at TEXT, json_data TEXT,"
                           " PRIMARY KEY (realm, id))"));

            QVERIFY(q.exec("PRAGMA user_version=1"));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    bool hasTable(const QTemporaryDir &dir, const QString &table)
    {
        bool found = false;
        const QString connection = "tbl-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            if (db.open()) {
                QSqlQuery q(db);
                q.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name=?");
                q.addBindValue(table);
                if (q.exec() && q.next()) {
                    found = true;
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return found;
    }

    int readUserVersion(const QTemporaryDir &dir)
    {
        int version = -1;
        const QString connection = "v-read-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec("PRAGMA user_version") && q.next()) {
                    version = q.value(0).toInt();
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return version;
    }

    bool hasColumn(const QTemporaryDir &dir, const QString &table, const QString &column)
    {
        bool found = false;
        const QString connection = "col-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec(QString("PRAGMA table_info(%1)").arg(table))) {
                    while (q.next()) {
                        if (q.value(1).toString() == column) {
                            found = true;
                            break;
                        }
                    }
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return found;
    }

    // Row count is what proves the migration ALTERed rather than dropped:
    // buyouts and tab metadata must survive a payload invalidation.
    int rowCount(const QTemporaryDir &dir, const QString &table)
    {
        int count = -1;
        const QString connection = "cnt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(dbPath(dir));
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec("SELECT COUNT(*) FROM " + table) && q.next()) {
                    count = q.value(0).toInt();
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return count;
    }

    poe::StashTab makeTab(const QString &id)
    {
        poe::StashTab tab;
        tab.id = id;
        tab.name = "Tab " + id;
        tab.type = "PremiumStash";
        return tab;
    }

} // namespace

class UserStoreMigrationTest : public QObject
{
    Q_OBJECT

private slots:
    void migratesVersion1ToCurrent();
    void staleRowsKeepMetadataButYieldNoJson();
    void freshDatabaseGetsTheColumnWithoutAnAlter();
    void savedRowsRoundTripAtTheCurrentPayloadVersion();
    void aRowFromANewerPayloadVersionIsNotRead();
    void compositeKeyDatabaseIsRebuilt();
    void buyoutRowsSurviveTheRepairStep();

    // #250/#257: the store reports whether it opened and migrated.
    void healthyStoreIsUsable();
    void newerSchemaIsNotUsableAndNotMigrated();
    void newerSchemaReceivesNoPriceWrites();
};

void UserStoreMigrationTest::migratesVersion1ToCurrent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createVersion1Database(dir);
    QCOMPARE(readUserVersion(dir), 1);

    {
        UserStore store(QDir(dir.path()), kAccount);
    }

    QCOMPARE(readUserVersion(dir), 3);
    QVERIFY(hasColumn(dir, "stashes", "json_version"));
    QVERIFY(hasColumn(dir, "characters", "json_version"));

    // A healthy database must ALTER, not reset: dropping the tables here
    // would take the tab and character metadata with it. The 2 -> 3 repair
    // step only rebuilds tables whose primary key is actually wrong.
    QCOMPARE(rowCount(dir, "stashes"), 1);
    QCOMPARE(rowCount(dir, "characters"), 1);

    // The repair step also fills in the buyout tables, which version-1
    // databases from the earliest alphas never had.
    QVERIFY(hasTable(dir, "item_buyouts"));
    QVERIFY(hasTable(dir, "location_buyouts"));
}

void UserStoreMigrationTest::staleRowsKeepMetadataButYieldNoJson()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createVersion1Database(dir);

    UserStore store(QDir(dir.path()), kAccount);

    // json_version is NULL on a migrated row, which never equals the current
    // payload version, so the cached json is treated as never fetched.
    QVERIFY(!store.stashes().getStash("tab-a", kRealm, kLeague).has_value());
    QVERIFY(!store.characters().getCharacter("Alice", kRealm).has_value());

    // The scalar columns are untouched, so the tab and character still show
    // up in the lists that drive the UI - they are empty, not missing.
    const auto stashes = store.stashes().getStashList(kRealm, kLeague);
    QCOMPARE(stashes.size(), size_t(1));
    QCOMPARE(stashes[0].id, QString("tab-a"));
    QCOMPARE(stashes[0].name, QString("Tab A"));

    const auto characters = store.characters().getCharacterList(kRealm, kLeague);
    QCOMPARE(characters.size(), size_t(1));
    QCOMPARE(characters[0].name, QString("Alice"));
}

void UserStoreMigrationTest::freshDatabaseGetsTheColumnWithoutAnAlter()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    {
        UserStore store(QDir(dir.path()), kAccount);
    }

    QCOMPARE(readUserVersion(dir), 3);
    QVERIFY(hasColumn(dir, "stashes", "json_version"));
    QVERIFY(hasColumn(dir, "characters", "json_version"));
}

void UserStoreMigrationTest::savedRowsRoundTripAtTheCurrentPayloadVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    UserStore store(QDir(dir.path()), kAccount);
    QVERIFY(saveStashFixture(store.stashes(), makeTab("tab-b"), kRealm, kLeague));

    const auto stash = store.stashes().getStash("tab-b", kRealm, kLeague);
    QVERIFY(stash.has_value());
    QCOMPARE(stash->name, QString("Tab tab-b"));

    poe::Character character;
    character.id = "char-b";
    character.name = "Bob";
    character.realm = kRealm;
    character.league = kLeague;
    QVERIFY(store.characters().saveCharacterList({character}));
    QVERIFY(saveCharacterFixture(store.characters(), character));

    const auto reloaded = store.characters().getCharacter("Bob", kRealm);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->name, QString("Bob"));
}

void UserStoreMigrationTest::aRowFromANewerPayloadVersionIsNotRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    {
        UserStore store(QDir(dir.path()), kAccount);
        QVERIFY(saveStashFixture(store.stashes(), makeTab("tab-c"), kRealm, kLeague));
        QVERIFY(store.stashes().getStash("tab-c", kRealm, kLeague).has_value());
    }

    // Simulate a downgrade: the row on disk was written by a build whose
    // payload version is higher than ours. '<' would read it and misparse it;
    // '!=' refetches instead.
    const QString connection = "bump-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(dbPath(dir));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(
            q.exec(QString("UPDATE stashes SET json_version = %1").arg(json::PAYLOAD_VERSION + 1)));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);

    UserStore store(QDir(dir.path()), kAccount);
    QVERIFY(!store.stashes().getStash("tab-c", kRealm, kLeague).has_value());
}

void UserStoreMigrationTest::compositeKeyDatabaseIsRebuilt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createCompositeKeyDatabase(dir);
    QCOMPARE(readUserVersion(dir), 1);

    UserStore store(QDir(dir.path()), kAccount);
    QCOMPARE(readUserVersion(dir), 3);

    // The rebuilt tables must accept the ON CONFLICT(id) upserts, which the
    // composite-key schema rejected at prepare time. Saving the same id twice
    // exercises the conflict path itself, not just the insert.
    QVERIFY(saveStashFixture(store.stashes(), makeTab("tab-a"), kRealm, kLeague));
    QVERIFY(saveStashFixture(store.stashes(), makeTab("tab-a"), kRealm, kLeague));
    QCOMPARE(rowCount(dir, "stashes"), 1);

    poe::Character character;
    character.id = "char-a";
    character.name = "Alice";
    character.realm = kRealm;
    character.league = kLeague;
    QVERIFY(store.characters().saveCharacterList({character}));
    QVERIFY(store.characters().saveCharacterList({character}));
    QCOMPARE(rowCount(dir, "characters"), 1);

    // These alphas predate BuyoutRepo entirely.
    QVERIFY(hasTable(dir, "item_buyouts"));
    QVERIFY(hasTable(dir, "location_buyouts"));
}

void UserStoreMigrationTest::buyoutRowsSurviveTheRepairStep()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createVersion1Database(dir);

    // A version-1 database from alpha.7 or later has the buyout tables, and
    // they hold user-authored prices that no migration step may drop.
    const QString connection = "buyout-seed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(dbPath(dir));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TABLE item_buyouts ("
                       " item_id TEXT PRIMARY KEY, location_id TEXT NOT NULL,"
                       " location_type TEXT NOT NULL"
                       "   CHECK (location_type IN ('character', 'stash')),"
                       " currency TEXT NOT NULL,"
                       " inherited INTEGER NOT NULL CHECK (inherited IN (0,1)),"
                       " last_update INTEGER NOT NULL, source TEXT NOT NULL,"
                       " type TEXT NOT NULL, value REAL NOT NULL)"));
        QVERIFY(q.exec("INSERT INTO item_buyouts (item_id, location_id, location_type,"
                       " currency, inherited, last_update, source, type, value)"
                       " VALUES ('item-a', 'tab-a', 'stash', 'chaos', 0, 0, 'manual',"
                       " 'b/o', 5.0)"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);

    UserStore store(QDir(dir.path()), kAccount);
    QCOMPARE(readUserVersion(dir), 3);

    const auto buyouts = store.buyouts().loadItemBuyouts().value();
    QCOMPARE(buyouts.size(), size_t(1));
    QVERIFY(buyouts.contains("item-a"));
    QCOMPARE(buyouts.at("item-a").value, 5.0);
}

void UserStoreMigrationTest::healthyStoreIsUsable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    {
        UserStore fresh(QDir(dir.path()), kAccount);
        QVERIFY(fresh.isUsable());
    }
    UserStore reopened(QDir(dir.path()), kAccount);
    QVERIFY(reopened.isUsable());
}

// #257: a database written by a newer build is not this build's to migrate
// or use. Its shape must be left exactly as found: no buyout tables created,
// no columns added, no version stamped.
void UserStoreMigrationTest::newerSchemaIsNotUsableAndNotMigrated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    createVersion1Database(dir);

    const QString connection = "newer-seed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(dbPath(dir));
        QVERIFY(db.open());
        QSqlQuery q(db);
        // SCHEMA_VERSION + 1; the existing pins hard-code the current 3.
        QVERIFY(q.exec("PRAGMA user_version=4"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);

    {
        UserStore store(QDir(dir.path()), kAccount);
        QVERIFY(!store.isUsable());
    }

    QCOMPARE(readUserVersion(dir), 4);
    QVERIFY(!hasTable(dir, "item_buyouts"));
    QVERIFY(!hasTable(dir, "location_buyouts"));
    QVERIFY(!hasColumn(dir, "stashes", "json_version"));
    QCOMPARE(rowCount(dir, "stashes"), 1);
    QCOMPARE(rowCount(dir, "characters"), 1);
}

// #257: prices edited while the store is unusable must not be written
// into a database a newer build owns. The seed has buyout tables, so a
// write that was attempted would succeed and leave a row behind.
void UserStoreMigrationTest::newerSchemaReceivesNoPriceWrites()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        UserStore fresh(QDir(dir.path()), kAccount);
        QVERIFY(fresh.isUsable());
    }
    const QString connection = "newer-seed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(dbPath(dir));
        QVERIFY(db.open());
        QSqlQuery q(db);
        // SCHEMA_VERSION + 1, as in newerSchemaIsNotUsableAndNotMigrated.
        QVERIFY(q.exec("PRAGMA user_version=4"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);

    {
        UserStore store(QDir(dir.path()), kAccount);
        QVERIFY(!store.isUsable());
        SqliteDataStore data(dir.filePath("league-data"));
        BuyoutManager manager(data, store.buyouts());
        QVERIFY(!manager.BuyoutsLoaded());

        const Item item = makeTestItem("item-257");
        manager.Set(item, makeChaosBuyout(9.0));
        manager.SetTab(item.location(), makeChaosBuyout(4.0));

        QVERIFY(manager.Get(item).IsNull());
        QVERIFY(manager.GetTab(item.location()).IsNull());
    }

    QCOMPARE(readUserVersion(dir), 4);
    QCOMPARE(rowCount(dir, "item_buyouts"), 0);
    QCOMPARE(rowCount(dir, "location_buyouts"), 0);
}

QTEST_MAIN(UserStoreMigrationTest)
#include "tst_userstoremigration.moc"
