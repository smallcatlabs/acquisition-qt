// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "liveguards.h"

// The refusals of a live run (#378). A live run uses the real API and a real
// login, so what it refuses is what keeps it away from the user's own data
// directory, from another account, and from the forum.
class LiveGuardsTest : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void acceptsALoggedInDirectoryOutsideTheRoots();
    void refusesADirectoryThatDoesNotExist();
    void refusesAPlatformDataRoot();
    void refusesADirectoryInsideAPlatformDataRoot();
    void refusesAPlatformDataRootSpelledInAnotherCase();
    void refusesALinkIntoAPlatformDataRoot();
    void acceptsANeighbourWhoseNameStartsLikeARoot();
    void refusesADirectoryNobodyLoggedInTo();
    void refusesADirectoryLoggedInAsSomeoneElse();
    void refusesWithoutAnExpectedAccount();
    void platformDataRootsCoverTheApplicationDataLocation();

    void acceptsASessionWithNoShop();
    void refusesASessionForAnotherAccount();
    void refusesASessionWithAShopThread();
    void refusesASessionWithShopAutoUpdate();

private:
    // A directory holding what a login leaves behind: settings.ini naming
    // the account.
    QString loggedInDir(const QString &name, const QString &account)
    {
        QDir(m_temp->path()).mkpath(name);
        const QString path = QDir(m_temp->path()).filePath(name);
        QSettings settings(QDir(path).filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("account", account);
        settings.setValue("realm", "pc");
        settings.setValue("league", "Standard");
        settings.sync();
        return path;
    }

    QString root() const { return QDir(m_temp->path()).filePath("appdata"); }

    std::unique_ptr<QTemporaryDir> m_temp;
};

// An account name as the API gives it: a name and a discriminator.
static const QString ACCOUNT = "TestAccount#1234";

void LiveGuardsTest::init()
{
    m_temp = std::make_unique<QTemporaryDir>();
    QVERIFY(m_temp->isValid());
    QVERIFY(QDir(m_temp->path()).mkpath("appdata"));
}

void LiveGuardsTest::acceptsALoggedInDirectoryOutsideTheRoots()
{
    const QString dir = loggedInDir("live", ACCOUNT);
    QCOMPARE(LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}), QString());
}

void LiveGuardsTest::refusesADirectoryThatDoesNotExist()
{
    const QString dir = QDir(m_temp->path()).filePath("missing");
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::refusesAPlatformDataRoot()
{
    const QString dir = loggedInDir("appdata", ACCOUNT);
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::refusesADirectoryInsideAPlatformDataRoot()
{
    const QString dir = loggedInDir("appdata/Acquisition", ACCOUNT);
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::refusesAPlatformDataRootSpelledInAnotherCase()
{
    const QString dir = loggedInDir("appdata/Acquisition", ACCOUNT);
    const QString other_case = QDir(m_temp->path()).filePath("APPDATA");
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {other_case}).isEmpty());
}

void LiveGuardsTest::refusesALinkIntoAPlatformDataRoot()
{
    const QString target = loggedInDir("appdata/Acquisition", ACCOUNT);
    const QString link = QDir(m_temp->path()).filePath("looks-harmless");
    if (!QFile::link(target, link)) {
        QSKIP("this file system cannot create a symbolic link");
    }
    QVERIFY(!LiveDirectoryRefusal(QDir(link), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::acceptsANeighbourWhoseNameStartsLikeARoot()
{
    // "appdata-live" is beside "appdata", not inside it.
    const QString dir = loggedInDir("appdata-live", ACCOUNT);
    QCOMPARE(LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}), QString());
}

void LiveGuardsTest::refusesADirectoryNobodyLoggedInTo()
{
    QVERIFY(QDir(m_temp->path()).mkpath("empty"));
    const QString dir = QDir(m_temp->path()).filePath("empty");
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::refusesADirectoryLoggedInAsSomeoneElse()
{
    const QString dir = loggedInDir("live", "SomeoneElse#9999");
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), ACCOUNT, {root()}).isEmpty());
}

void LiveGuardsTest::refusesWithoutAnExpectedAccount()
{
    const QString dir = loggedInDir("live", ACCOUNT);
    QVERIFY(!LiveDirectoryRefusal(QDir(dir), QString(), {root()}).isEmpty());
}

void LiveGuardsTest::platformDataRootsCoverTheApplicationDataLocation()
{
    // Where the application keeps a user's data by default (src/main.cpp)
    // must be inside one of the roots, whatever the application is named.
    const QString app_data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QVERIFY(!app_data.isEmpty());
    bool covered = false;
    for (const QString &root : PlatformDataRoots()) {
        covered = covered || app_data.startsWith(root + '/', Qt::CaseInsensitive);
    }
    QVERIFY2(covered, qPrintable("not under any platform data root: " + app_data));
}

void LiveGuardsTest::acceptsASessionWithNoShop()
{
    QCOMPARE(LiveSessionRefusal(ACCOUNT, ACCOUNT, {}, false), QString());
}

void LiveGuardsTest::refusesASessionForAnotherAccount()
{
    QVERIFY(!LiveSessionRefusal("SomeoneElse#9999", ACCOUNT, {}, false).isEmpty());
}

void LiveGuardsTest::refusesASessionWithAShopThread()
{
    QVERIFY(!LiveSessionRefusal(ACCOUNT, ACCOUNT, {"1234567"}, false).isEmpty());
}

void LiveGuardsTest::refusesASessionWithShopAutoUpdate()
{
    QVERIFY(!LiveSessionRefusal(ACCOUNT, ACCOUNT, {}, true).isEmpty());
}

QTEST_GUILESS_MAIN(LiveGuardsTest)
#include "tst_liveguards.moc"
