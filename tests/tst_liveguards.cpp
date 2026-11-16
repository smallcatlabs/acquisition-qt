// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "liveguards.h"
#include "testsuite.h"

// The refusals of a live run (#378). A live run uses the real API and a real
// login, so what it refuses is what keeps it away from the user's own data
// directory, from another account, from the forum, and from a branch that
// changes the code the rate limits depend on.
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

    void aSettingComesFromTheCommandLineOrElseTheEnvironment();

    void acceptsAnOutputDirectoryOutsideTheRoots();
    void refusesAnOutputDirectoryThatDoesNotExist();
    void refusesAnOutputDirectoryInsideARoot();
    void refusesAnOutputLinkIntoARoot();

    // The branch refusal: which files are the refresh and network code.
    void patternsAreTheSourcePathsOfTheWorkflow();
    void theRealWorkflowNamesTheNetworkCode();
    void everyPatternOfTheRealWorkflowMatchesAFileThatExists();
    void aPatternMatchesWhatTheWorkflowWouldRunFor_data();
    void aPatternMatchesWhatTheWorkflowWouldRunFor();
    void refusesAPatternItCannotRead();
    void refusesWithoutPatterns();

    // The branch refusal on a repository.
    void acceptsABranchThatLeavesTheNetworkCodeAlone();
    void refusesACommittedChange();
    void refusesAStagedChange();
    void refusesAnUnstagedChange();
    void refusesANewFileNotYetAdded();
    void refusesANetworkFileMovedAway();
    void acceptsNetworkChangesMadeOnMasterSince();
    void refusesWithoutOriginMaster();
    void refusesOutsideARepository();
    void refusesAWorkflowThatNamesNoSourcePaths();
    void leavesTheRepositoryAsItFoundIt();

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

    // The top of the source tree these tests were built from.
    static QDir sourceTree() { return QDir(QFINDTESTDATA("../.github")).filePath(".."); }

    QString repo() const { return QDir(m_temp->path()).filePath("repo"); }

    void write(const QString &file, const QString &text)
    {
        const QString path = QDir(repo()).filePath(file);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly) || (out.write(text.toUtf8()) < 0)) {
            qFatal("cannot write %s", qPrintable(path));
        }
    }

    // Runs git in the repository; says what went wrong, or nothing.
    QString git(const QStringList &arguments)
    {
        QProcess process;
        process.setWorkingDirectory(repo());
        process.start("git", arguments);
        if (process.waitForFinished(30000) && (process.exitStatus() == QProcess::NormalExit)
            && (process.exitCode() == 0)) {
            return {};
        }
        return "git " + arguments.join(' ') + ": " + process.errorString() + ' '
               + QString::fromUtf8(process.readAllStandardError());
    }

    // A repository shaped like this one, with origin/master at its one
    // commit. Check QTest::currentTestFailed() after the call.
    void makeRepository();

    std::unique_ptr<QTemporaryDir> m_temp;
};

#define REPOSITORY() \
    do { \
        makeRepository(); \
        if (QTest::currentTestFailed()) { \
            return; \
        } \
    } while (false)

// The trigger of .github/workflows/sanitizers.yml, cut down.
static const QString WORKFLOW = R"(name: Sanitizers

on:
  push:
    tags: ['v*']
  pull_request:
    branches:
      - 'src/a-branch-not-a-path'
    paths:
      - 'src/ratelimit/**'
      # A comment does not end the list.

      - 'src/poe/poeapiclient.*' # nor does one after an entry
      - 'tests/fake*.h'
      - 'CMakeLists.txt'
  workflow_dispatch:

jobs:
  sanitizers:
    steps:
      - 'src/a-step-not-a-path'
)";

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

void LiveGuardsTest::aSettingComesFromTheCommandLineOrElseTheEnvironment()
{
    const char *variable = "ACQ_TEST_LIVE_SETTING";
    qunsetenv(variable);
    QCOMPARE(LiveSetting("given", variable), QString("given"));
    QCOMPARE(LiveSetting(QString(), variable), QString());
    qputenv(variable, "from the environment");
    QCOMPARE(LiveSetting(QString(), variable), QString("from the environment"));
    QCOMPARE(LiveSetting("given", variable), QString("given"));
    qunsetenv(variable);
}

void LiveGuardsTest::acceptsAnOutputDirectoryOutsideTheRoots()
{
    QVERIFY(QDir(m_temp->path()).mkpath("record"));
    const QString out = QDir(m_temp->path()).filePath("record");
    QCOMPARE(LiveOutputRefusal(QDir(out), {root()}), QString());
}

void LiveGuardsTest::refusesAnOutputDirectoryThatDoesNotExist()
{
    // Not made on the way: a directory that is not there yet cannot be
    // compared with the roots once its links are resolved.
    const QString out = QDir(m_temp->path()).filePath("record");
    QVERIFY(!LiveOutputRefusal(QDir(out), {root()}).isEmpty());
    QVERIFY(!QDir(out).exists());
}

void LiveGuardsTest::refusesAnOutputDirectoryInsideARoot()
{
    QVERIFY(QDir(m_temp->path()).mkpath("appdata/record"));
    const QString out = QDir(m_temp->path()).filePath("appdata/record");
    const QString refusal = LiveOutputRefusal(QDir(out), {root()});
    QVERIFY2(refusal.contains(root()), qPrintable(refusal));
    QVERIFY(!LiveOutputRefusal(QDir(root()), {root()}).isEmpty());
}

void LiveGuardsTest::refusesAnOutputLinkIntoARoot()
{
    QVERIFY(QDir(m_temp->path()).mkpath("appdata/record"));
    const QString target = QDir(m_temp->path()).filePath("appdata/record");
    const QString link = QDir(m_temp->path()).filePath("looks-harmless");
    if (!QFile::link(target, link)) {
        QSKIP("this file system cannot create a symbolic link");
    }
    QVERIFY(!LiveOutputRefusal(QDir(link), {root()}).isEmpty());
}

void LiveGuardsTest::patternsAreTheSourcePathsOfTheWorkflow()
{
    QCOMPARE(LiveNetworkPatterns(WORKFLOW),
             QStringList({"src/ratelimit/**", "src/poe/poeapiclient.*"}));
}

void LiveGuardsTest::theRealWorkflowNamesTheNetworkCode()
{
    QFile workflow(sourceTree().filePath(".github/workflows/sanitizers.yml"));
    QVERIFY2(workflow.open(QIODevice::ReadOnly), qPrintable(workflow.fileName()));
    const QStringList patterns = LiveNetworkPatterns(QString::fromUtf8(workflow.readAll()));
    for (const char *expected : {"src/ratelimit/**",
                                 "src/poe/poeapiclient.*",
                                 "src/itemsmanagerworker.*",
                                 "src/util/networkmanager.*"}) {
        QVERIFY2(patterns.contains(expected), expected);
    }
    for (const QString &pattern : patterns) {
        QVERIFY2(pattern.startsWith("src/"), qPrintable(pattern));
    }
}

void LiveGuardsTest::everyPatternOfTheRealWorkflowMatchesAFileThatExists()
{
    // A pattern that matches nothing guards nothing: a file was renamed, or
    // the pattern is read differently here than by the workflow.
    QFile workflow(sourceTree().filePath(".github/workflows/sanitizers.yml"));
    QVERIFY(workflow.open(QIODevice::ReadOnly));
    const QStringList patterns = LiveNetworkPatterns(QString::fromUtf8(workflow.readAll()));
    QVERIFY(!patterns.isEmpty());

    QStringList files;
    QDirIterator walk(sourceTree().filePath("src"), QDir::Files, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        files << sourceTree().relativeFilePath(walk.next());
    }
    QVERIFY(files.contains("src/item.cpp"));
    for (const QString &pattern : patterns) {
        QVERIFY2(!LiveChangeRefusal(files, {pattern}).isEmpty(), qPrintable(pattern));
    }
}

void LiveGuardsTest::aPatternMatchesWhatTheWorkflowWouldRunFor_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("file");
    QTest::addColumn<bool>("matches");

    // clang-format off
    QTest::newRow("two stars, in the directory")    << "src/ratelimit/**"         << "src/ratelimit/ratelimiter.cpp"  << true;
    QTest::newRow("two stars, deeper")              << "src/ratelimit/**"         << "src/ratelimit/detail/gate.h"    << true;
    QTest::newRow("a star, the source")             << "src/poe/poeapiclient.*"   << "src/poe/poeapiclient.cpp"       << true;
    QTest::newRow("a star, the header")             << "src/poe/poeapiclient.*"   << "src/poe/poeapiclient.h"         << true;
    QTest::newRow("a star stops at a directory")    << "src/poe/poeapiclient.*"   << "src/poe/poeapiclient.d/a.cpp"   << false;
    QTest::newRow("a dot is a dot")                 << "src/poe/poeapiclient.*"   << "src/poe/poeapiclient_mock.cpp"  << false;
    QTest::newRow("a file")                         << "src/replytimeout.h"       << "src/replytimeout.h"             << true;
    QTest::newRow("a file, and more after it")      << "src/replytimeout.h"       << "src/replytimeout.hpp"           << false;
    QTest::newRow("a file, and more before it")     << "src/replytimeout.h"       << "tests/src/replytimeout.h"       << false;
    // clang-format on
}

void LiveGuardsTest::aPatternMatchesWhatTheWorkflowWouldRunFor()
{
    QFETCH(QString, pattern);
    QFETCH(QString, file);
    QFETCH(bool, matches);
    const QString refusal = LiveChangeRefusal({"docs/README.md", file}, {pattern});
    QCOMPARE(!refusal.isEmpty(), matches);
    if (matches) {
        QVERIFY2(refusal.contains(file), qPrintable(refusal));
    }
}

void LiveGuardsTest::refusesAPatternItCannotRead()
{
    // Read as written, these would match nothing and let everything through.
    QVERIFY(!LiveChangeRefusal({"src/item.cpp"}, {"src/poe/poeapiclient.?pp"}).isEmpty());
    QVERIFY(!LiveChangeRefusal({"src/item.cpp"}, {"src/poe/[a-z]*.cpp"}).isEmpty());
}

void LiveGuardsTest::refusesWithoutPatterns()
{
    QVERIFY(!LiveChangeRefusal({"src/item.cpp"}, {}).isEmpty());
}

void LiveGuardsTest::makeRepository()
{
    QVERIFY(QDir().mkpath(repo()));
    QCOMPARE(git({"init", "--quiet"}), QString());
    // Nothing of the machine's own git settings may decide these tests.
    for (const QStringList &setting : {QStringList{"user.name", "A Test"},
                                       QStringList{"user.email", "test@example.invalid"},
                                       QStringList{"commit.gpgsign", "false"},
                                       QStringList{"core.autocrlf", "false"}}) {
        QCOMPARE(git(QStringList{"config"} + setting), QString());
    }
    write(".github/workflows/sanitizers.yml", WORKFLOW);
    write("src/ratelimit/ratelimiter.cpp", "// the rate limiter\n");
    write("src/poe/poeapiclient.cpp", "// the API client\n");
    write("src/item.cpp", "// an item\n");
    write("tests/fakeclient.h", "// a fake\n");
    QCOMPARE(git({"add", "--all"}), QString());
    QCOMPARE(git({"commit", "--quiet", "--message", "the start"}), QString());
    QCOMPARE(git({"update-ref", "refs/remotes/origin/master", "HEAD"}), QString());
}

void LiveGuardsTest::acceptsABranchThatLeavesTheNetworkCodeAlone()
{
    REPOSITORY();
    write("src/item.cpp", "// an item, changed\n");
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "work"}), QString());
    write("tests/fakeclient.h", "// a fake, changed\n");
    write("notes.txt", "not added\n");

    // The changes are seen, and none of them is the network code.
    LiveChanges changes = LiveChangesSinceMaster(QDir(repo()));
    QCOMPARE(changes.error, QString());
    changes.files.sort();
    QCOMPARE(changes.files, QStringList({"notes.txt", "src/item.cpp", "tests/fakeclient.h"}));
    QCOMPARE(LiveBranchRefusal(QDir(repo())), QString());
}

void LiveGuardsTest::refusesACommittedChange()
{
    REPOSITORY();
    write("src/ratelimit/ratelimiter.cpp", "// the rate limiter, changed\n");
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "work"}), QString());
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("src/ratelimit/ratelimiter.cpp"), qPrintable(refusal));
    // master may have moved since it was fetched; the reader is told what
    // the branch was compared with.
    QVERIFY2(refusal.contains("origin/master as last fetched"), qPrintable(refusal));
}

void LiveGuardsTest::refusesAStagedChange()
{
    REPOSITORY();
    write("src/poe/poeapiclient.cpp", "// the API client, changed\n");
    QCOMPARE(git({"add", "src/poe/poeapiclient.cpp"}), QString());
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("src/poe/poeapiclient.cpp"), qPrintable(refusal));
}

void LiveGuardsTest::refusesAnUnstagedChange()
{
    REPOSITORY();
    write("src/poe/poeapiclient.cpp", "// the API client, changed\n");
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("src/poe/poeapiclient.cpp"), qPrintable(refusal));
}

void LiveGuardsTest::refusesANewFileNotYetAdded()
{
    REPOSITORY();
    write("src/ratelimit/gate.h", "// new\n");
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("src/ratelimit/gate.h"), qPrintable(refusal));
}

void LiveGuardsTest::refusesANetworkFileMovedAway()
{
    // Its new name is not the network code; the name it left is.
    REPOSITORY();
    QCOMPARE(git({"mv", "src/poe/poeapiclient.cpp", "src/client.cpp"}), QString());
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "moved"}), QString());
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("src/poe/poeapiclient.cpp"), qPrintable(refusal));
}

void LiveGuardsTest::acceptsNetworkChangesMadeOnMasterSince()
{
    // master changed the rate limiter after this branch left it. That is
    // master's change, not the branch's.
    REPOSITORY();
    write("src/ratelimit/ratelimiter.cpp", "// the rate limiter, changed on master\n");
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "master moves on"}), QString());
    QCOMPARE(git({"update-ref", "refs/remotes/origin/master", "HEAD"}), QString());
    QCOMPARE(git({"checkout", "--quiet", "--detach", "HEAD~1"}), QString());
    write("src/item.cpp", "// an item, changed\n");
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "work"}), QString());

    QCOMPARE(LiveChangesSinceMaster(QDir(repo())).files, QStringList({"src/item.cpp"}));
    QCOMPARE(LiveBranchRefusal(QDir(repo())), QString());
}

void LiveGuardsTest::refusesWithoutOriginMaster()
{
    REPOSITORY();
    QCOMPARE(git({"update-ref", "-d", "refs/remotes/origin/master"}), QString());
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("origin/master"), qPrintable(refusal));
}

void LiveGuardsTest::refusesOutsideARepository()
{
    // The files of a source tree, but no history to compare them with.
    write(".github/workflows/sanitizers.yml", WORKFLOW);
    write("src/item.cpp", "// an item\n");
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("git"), qPrintable(refusal));
}

void LiveGuardsTest::refusesAWorkflowThatNamesNoSourcePaths()
{
    REPOSITORY();
    write(".github/workflows/sanitizers.yml", "name: Sanitizers\non:\n  workflow_dispatch:\n");
    QCOMPARE(git({"commit", "--quiet", "--all", "--message", "no paths"}), QString());
    QCOMPARE(git({"update-ref", "refs/remotes/origin/master", "HEAD"}), QString());
    const QString refusal = LiveBranchRefusal(QDir(repo()));
    QVERIFY2(refusal.contains("sanitizers.yml"), qPrintable(refusal));
}

void LiveGuardsTest::leavesTheRepositoryAsItFoundIt()
{
    // A file written again with the same text: git has to read it to know
    // it is unchanged, and would then note that in its index.
    REPOSITORY();
    const QString file = QDir(repo()).filePath("src/item.cpp");
    write("src/item.cpp", "// an item\n");
    QFile touched(file);
    QVERIFY(touched.open(QIODevice::ReadWrite));
    QVERIFY(touched.setFileTime(QDateTime::currentDateTime().addSecs(60),
                                QFileDevice::FileModificationTime));
    touched.close();

    QFile index(QDir(repo()).filePath(".git/index"));
    QVERIFY(index.open(QIODevice::ReadOnly));
    const QByteArray before = index.readAll();
    index.close();

    QCOMPARE(LiveBranchRefusal(QDir(repo())), QString());

    QVERIFY(index.open(QIODevice::ReadOnly));
    QVERIFY(index.readAll() == before);
}

ACQ_TEST_GUILESS_MAIN(LiveGuardsTest)
#include "tst_liveguards.moc"
