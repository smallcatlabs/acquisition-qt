// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include "legacy/legacydatastore.h"
#include "testsuite.h"

class LegacyDataStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void readsRealShapedItemsAndSkipsBadRows();
};

void LegacyDataStoreTest::readsRealShapedItemsAndSkipsBadRows()
{
    const QString fixture_path = QFINDTESTDATA("fixtures/legacy-items-real-shaped.json");
    QVERIFY2(!fixture_path.isEmpty(), "real-shaped legacy fixture was not found");
    QFile fixture(fixture_path);
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const QByteArray real_shaped_items = fixture.readAll();

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString database_path = dir.filePath("legacy-v4.db");
    const QString connection_name = "legacy-fixture:"
                                    + QUuid::createUuid().toString(QUuid::WithoutBraces);

    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
        db.setDatabaseName(database_path);
        QVERIFY2(db.open(), qPrintable(db.lastError().text()));
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TABLE data (key TEXT PRIMARY KEY, value BLOB)"));
        QVERIFY(query.exec("CREATE TABLE tabs (type INTEGER PRIMARY KEY, value BLOB)"));
        QVERIFY(query.exec("CREATE TABLE items (loc TEXT PRIMARY KEY, value BLOB)"));

        query.prepare("INSERT INTO data (key, value) VALUES (?, ?)");
        const auto insert_data = [&query](const QString &key, const QByteArray &value) {
            query.bindValue(0, key);
            query.bindValue(1, value);
            return query.exec();
        };
        QVERIFY(insert_data("db_version", "4"));
        // Deliberately omit `version`: missing data keys are non-fatal.
        QVERIFY(insert_data("buyouts",
                            R"json({
                "good":{"value":2,"last_update":1,"type":"b/o","currency":"chaos","source":"manual","inherited":false},
                "bad":{"value":"not-a-number"}
            })json"));
        QVERIFY(insert_data("tab_buyouts", "{}"));

        query.prepare("INSERT INTO tabs (type, value) VALUES (?, ?)");
        query.bindValue(0, 0);
        query.bindValue(
            1,
            R"([{"id":"abc123def0","name":"Priced","type":"PremiumStash","index":0,"metadata":{"public":true,"colour":"7f7f7f"},"unknown_tab_field":true}])");
        QVERIFY(query.exec());
        // Deliberately omit the character-list row too.

        query.prepare("INSERT INTO items (loc, value) VALUES (?, ?)");
        query.bindValue(0, "abc123def0");
        query.bindValue(1, real_shaped_items);
        QVERIFY(query.exec());
        query.bindValue(0, "bad-row");
        query.bindValue(1, "[{not valid json]");
        QVERIFY(query.exec());

        db.close();
    }
    QSqlDatabase::removeDatabase(connection_name);

    const LegacyDataStore store(database_path);

    QVERIFY(store.isValid());
    QCOMPARE(store.data().db_version, QString("4"));
    QCOMPARE(store.data().buyouts.size(), std::size_t(1));
    QVERIFY(store.data().buyouts.contains("good"));
    QCOMPARE(store.itemCount(), 1);
    QCOMPARE(store.items().size(), std::size_t(1));
    QVERIFY(store.items().contains("abc123def0"));
    QCOMPARE(store.items().at("abc123def0").front().typeLine, QString("Chaos Orb"));
    QCOMPARE(store.tabs().stashes.size(), std::size_t(1));
    // Missing version, malformed buyout element, missing characters, malformed items row.
    QCOMPARE(store.skippedRowCount(), 4);
}

ACQ_TEST_GUILESS_MAIN(LegacyDataStoreTest)

#include "tst_legacydatastore.moc"
