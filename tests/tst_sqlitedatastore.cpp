// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include "datastore/sqlitedatastore.h"

class SqliteDataStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void openingDoesNotRebuildTheFile();
};

namespace {

    // PRAGMA freelist_count through a connection of the test's own.
    int freelistCount(const QString &filename)
    {
        const QString connection = "freelist-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        int count = -1;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(filename);
            if (db.open()) {
                QSqlQuery q(db);
                if (q.exec("PRAGMA freelist_count") && q.next()) {
                    count = q.value(0).toInt();
                }
                db.close();
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return count;
    }

} // namespace

// #292: opening a data file must not VACUUM it. A VACUUM rewrites the whole
// file, on the main thread at login, which is slow for a per-league file
// that still holds a pre-0.16 item table. Its one observable effect is an
// empty freelist, so pages freed before the open must still be free after.
void SqliteDataStoreTest::openingDoesNotRebuildTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filename = dir.filePath("data-file");

    {
        SqliteDataStore store(filename);
        store.Set("large", QString(256 * 1024, QChar('x')));
        store.Set("large", QString());
    }
    const int freed = freelistCount(filename);
    QVERIFY(freed > 0);

    {
        SqliteDataStore reopened(filename);
        QCOMPARE(reopened.Get("large", "missing"), QString());
    }
    QCOMPARE(freelistCount(filename), freed);
}

QTEST_GUILESS_MAIN(SqliteDataStoreTest)

#include "tst_sqlitedatastore.moc"
