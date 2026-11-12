// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QByteArray>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include <string>

#include <glaze/glaze.hpp>

#include "datastore/sqlitedatastore.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/oauthtoken.h"

class SqliteDataStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void openingDoesNotRebuildTheFile();
    void clearedTokenLeavesNoBytesInTheFile();
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

// The global file holds the OAuth token (oauth_token), serialized the way
// OAuthManager stores it; sign-out sets it to "". A cleared value must not
// survive in the file's free space, where anyone reading the file (or a
// version-change backup of it) could recover it.
void SqliteDataStoreTest::clearedTokenLeavesNoBytesInTheFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filename = dir.filePath("data-file");

    OAuthToken token;
    token.access_token = "access-7f3c9a1e5b2d40869e1a2b3c4d5e6f70";
    token.expires_in = 2592000;
    token.refresh_token = "refresh-0a1b2c3d4e5f60718293a4b5c6d7e8f9";
    token.scope = "account:profile account:characters account:stashes account:leagues";
    token.username = "Tester#1234";
    token.sub = "8c2f1d9e-3b4a-4c5d-9e6f-7a8b9c0d1e2f";
    token.token_type = "bearer";
    std::string serialized;
    QVERIFY(!glz::write_json(token, serialized));

    {
        SqliteDataStore store(filename);
        store.Set("oauth_token", QString::fromStdString(serialized));
        store.Set("oauth_token", QString());
    }

    QFile file(filename);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    QVERIFY(!bytes.contains(token.access_token.toLatin1()));
    QVERIFY(!bytes.contains(token.refresh_token.toLatin1()));
}

QTEST_GUILESS_MAIN(SqliteDataStoreTest)

#include "tst_sqlitedatastore.moc"
