// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QOAuthHttpServerReplyHandler>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <QVersionNumber>

#include <functional>
#include <memory>

#include <semver/semver.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "application.h"
#include "datastore/datastore.h"
#include "datastore/sqlitedatastore.h"
#include "fakenetworkmanager.h"
#include "imagecache.h"
#include "mainwindowfixture.h"
#include "testsuite.h"
#include "ui/logindialog.h"
#include "ui/mainwindow_bridge.h"
#include "ui/updateprompt.h"
#include "util/checkmsvc.h"
#include "util/crashreporting.h"
#include "util/databackup.h"
#include "util/datadirlock.h"
#include "util/fatalerror.h"
#include "util/logging.h"
#include "util/networkmanager.h"
#include "util/oauthmanager.h"
#include "util/oauthtoken.h"
#include "util/updatechecker.h"
#include "version_defines.h"

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void defaultDataDirIgnoresExecutableName();
    void crashReportingDefaultsOn();
    void crashReportingFollowsSetting_data();
    void crashReportingFollowsSetting();
    void logLevelParsing_data();
    void logLevelParsing();
    void logFileOpensUnderNonAnsiDirectory();
    void forgettingUserKeepsOtherSettings();
    void tokenWithoutAccountNameDoesNotLogIn();
    void backupSkippedWhenVersionCurrent();
    void backupSkippedWithoutDataDir();
    void backupSkippedForEmptyDataDir();
    void backupTakenOnNewVersion();
    void updateButtonOpensUpdatePrompt();
    void skippedReleaseIsNotOfferedAgain();
    void declinedUpdateBeforeLoginOpensOnePrompt();
    void prereleaseOfADeclinedReleaseIsNotOffered();
    void imageCacheDeletesReplies_data();
    void imageCacheDeletesReplies();
    void updateCheckerDeletesReplies_data();
    void updateCheckerDeletesReplies();
    void networkManagerHasNoDiskCache();
    void repeatedGetIsNotServedFromCache();
    void legacyNetworkCacheIsRemoved();
    void legacyNetworkCacheLinkIsNotFollowed();
    void loginListensOnLoopbackAfterClose();
    void everyLoginRedirectsToTheListenerItBinds();
    void startupRefreshClosesTheListener();
    void updatePromptAnswersAreApplied();
    void msvcCheckPassesWithoutLibraries();
    void msvcCheckAnswer_data();
    void msvcCheckAnswer();
    void fatalErrorReportsWithLoggingOff();
    void freshDataDirTakesNoBackup();
    void legacyStoreVersionNamesTheBackup();
    void fullBackupSeriesStillRecordsVersion();
    void clearedSettingsTakeNoBackup();
    void settingsVersionMovesIntoTheStore();
    void imageCacheSignalsFailedFetch();
    void imageCacheKeepsNoFileForUndecodableBody();
    void imageCacheKeysOnRequestedUrl();
    void dataDirLockRefusesSecondHolder();
    void dataDirLockTakesOverStaleLock();
    void dataDirLockRefusesForeignHostLock();
    void dataDirLockRefusalNamesHolderAndFile();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
};

void ShellTest::initTestCase()
{
    // Keep any QStandardPaths lookup in Qt's test sandbox.
    QStandardPaths::setTestModeEnabled(true);
    // MainWindowFixture needs the 'main' logger, with the dist-sink hub that
    // logging::init installs (F42) for LogPanel to attach to.
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_main_logger->sinks().push_back(std::make_shared<spdlog::sinks::dist_sink_mt>());
    spdlog::register_logger(m_main_logger);
}

void ShellTest::cleanupTestCase()
{
    spdlog::drop("main");
    m_main_logger.reset();
}

void ShellTest::defaultDataDirIgnoresExecutableName()
{
    // Until the application named itself, Qt took the name from the
    // executable, and a release AppImage's file name carries its version: each
    // release opened a new, empty data directory (#468).
    const QString suite_name = QCoreApplication::applicationName();
    const auto restore = qScopeGuard([&] { QCoreApplication::setApplicationName(suite_name); });
    const auto default_data_dir = [](const QString &executable_name) {
        QCoreApplication::setApplicationName(executable_name);
        SetApplicationIdentity();
        return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    };
    const QString data_dir = default_data_dir("acquisition");
    QCOMPARE(QFileInfo(data_dir).fileName(), "acquisition");
    QCOMPARE(default_data_dir("Acquisition-0.18.11-x86_64.AppImage"), data_dir);
    QCOMPARE(default_data_dir("Acquisition"), data_dir);
    QCOMPARE(QCoreApplication::applicationVersion(), APP_VERSION_STRING);
}

void ShellTest::crashReportingDefaultsOn()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    QVERIFY(!settings.contains("report_crashes"));
    QCOMPARE(CrashReportingEnabled(settings), true);
}

void ShellTest::crashReportingFollowsSetting_data()
{
    QTest::addColumn<bool>("stored");
    QTest::newRow("false") << false;
    QTest::newRow("true") << true;
}

void ShellTest::crashReportingFollowsSetting()
{
    QFETCH(bool, stored);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("settings.ini");
    {
        QSettings writer(path, QSettings::IniFormat);
        writer.setValue("report_crashes", stored);
    }
    // Read back through a fresh object, as main() does at startup.
    const QSettings settings(path, QSettings::IniFormat);
    QCOMPARE(CrashReportingEnabled(settings), stored);
}

void ShellTest::logLevelParsing_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<int>("expected");
    // The fallback is 'warn' so that it differs from every parsed case.
    QTest::newRow("DEBUG") << "DEBUG" << int(spdlog::level::debug);
    QTest::newRow("Info") << "Info" << int(spdlog::level::info);
    QTest::newRow("trace") << "trace" << int(spdlog::level::trace);
    QTest::newRow("nonsense") << "nonsense" << int(spdlog::level::warn);
    QTest::newRow("empty") << "" << int(spdlog::level::warn);
    QTest::newRow("off") << "off" << int(spdlog::level::off);
    QTest::newRow("OFF") << "OFF" << int(spdlog::level::off);
}

void ShellTest::logLevelParsing()
{
    QFETCH(QString, name);
    QFETCH(int, expected);
    QCOMPARE(int(logging::parseLevel(name, spdlog::level::warn)), expected);
}

void ShellTest::logFileOpensUnderNonAnsiDirectory()
{
    // A Windows profile whose name is outside the ANSI code page puts the
    // data directory there too, and the log must still open in it (#288).
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dir_path = QDir(root.path()).filePath(QStringLiteral("日本-журнал"));
    QVERIFY(QDir().mkpath(dir_path));
    const QString log_path = QDir(dir_path).filePath(QStringLiteral("log.txt"));

    // logging::init replaces the default logger and the registry's 'main'
    // entry; put back the suite's own before the directory goes, so that the
    // file sink is closed first.
    const auto previous = spdlog::default_logger();
    const auto restore = qScopeGuard([&] {
        spdlog::drop("main");
        spdlog::set_default_logger(previous);
        spdlog::register_logger(m_main_logger);
    });
    logging::init(log_path);
    spdlog::info("written under a non-ANSI directory");
    spdlog::default_logger()->flush();

    QVERIFY2(QFileInfo::exists(log_path), qPrintable(log_path));
    QFile file(log_path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.readAll().contains("written under a non-ANSI directory"));
}

// The services a LoginDialog needs, all offline and in a temporary directory.
struct LoginDialogServices
{
    LoginDialogServices()
        : settings(dir.filePath("settings.ini"), QSettings::IniFormat)
        , datastore(dir.filePath("data/global.db"))
        , oauth(network, datastore)
    {}

    std::unique_ptr<LoginDialog> makeDialog()
    {
        return std::make_unique<LoginDialog>(settings, network, oauth, datastore);
    }

    QTemporaryDir dir;
    QSettings settings;
    FakeNetworkManager network;
    SqliteDataStore datastore;
    OAuthManager oauth;
};

void ShellTest::forgettingUserKeepsOtherSettings()
{
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    auto &settings = services.settings;
    settings.setValue("remember_user", false);
    settings.setValue("session_id", "test-session-id");
    settings.setValue("account", "TestAccount");
    settings.setValue("league", "Standard");
    settings.setValue("realm", "pc");
    settings.setValue("theme", "dark");
    settings.setValue("version", "0.0.1-test");
    settings.setValue("report_crashes", false);
    services.datastore.Set("oauth_token", R"({"username":"TestAccount"})");

    services.makeDialog().reset();

    // Credentials are forgotten...
    QVERIFY(!settings.contains("session_id"));
    QVERIFY(!settings.contains("account"));
    QCOMPARE(services.datastore.Get("oauth_token", "absent"), QString());
    // ...and nothing else is.
    QCOMPARE(settings.value("theme").toString(), QString("dark"));
    QCOMPARE(settings.value("version").toString(), QString("0.0.1-test"));
    QCOMPARE(settings.value("report_crashes").toBool(), false);
    QCOMPARE(settings.value("remember_user").toBool(), false);
    QCOMPARE(settings.value("league").toString(), QString("Standard"));
    QCOMPARE(settings.value("realm").toString(), QString("pc"));
}

// #299: an OAuth token whose user name is empty cannot become a session's
// account. The dialog refuses the login with a message instead of letting an
// incomplete identity reach the session.
void ShellTest::tokenWithoutAccountNameDoesNotLogIn()
{
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    auto dialog = services.makeDialog();
    int completions = 0;
    QObject::connect(dialog.get(), &LoginDialog::LoginComplete, dialog.get(), [&] {
        ++completions;
    });

    OAuthToken token;
    token.access_expiration = QDateTime::currentDateTime().addSecs(3600);
    emit services.oauth.grantAccess(token);

    auto *league = dialog->findChild<QComboBox *>("leagueComboBox");
    auto *login = dialog->findChild<QPushButton *>("loginButton");
    auto *error = dialog->findChild<QLabel *>("errorLabel");
    QVERIFY(league);
    QVERIFY(login);
    QVERIFY(error);
    league->setEditable(true);
    league->setCurrentText("Standard");
    QVERIFY(login->isEnabled());
    login->click();

    QCOMPARE(completions, 0);
    QVERIFY(!error->text().isEmpty());
    // A refused login saves no part of the identity it refused.
    QVERIFY(!services.settings.contains("realm"));
    QVERIFY(!services.settings.contains("league"));
    QVERIFY(!services.settings.contains("account"));
}

namespace {

    QStringList backupDirs(const QDir &app_data_dir)
    {
        return app_data_dir.entryList({"data-backup-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    }

    void writeFile(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
    }

} // namespace

void ShellTest::backupSkippedWhenVersionCurrent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QVERIFY(app_dir.mkpath("data"));
    writeFile(app_dir.filePath("data/userstore-TestAccount.db"), "rows");

    QCOMPARE(BackupDataOnNewVersion(app_dir, "1.2.3", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
}

void ShellTest::backupSkippedWithoutDataDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());

    QCOMPARE(BackupDataOnNewVersion(app_dir, "1.2.2", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
}

void ShellTest::backupSkippedForEmptyDataDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QVERIFY(app_dir.mkpath("data"));

    QCOMPARE(BackupDataOnNewVersion(app_dir, "1.2.2", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
}

void ShellTest::backupTakenOnNewVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QVERIFY(app_dir.mkpath("data"));
    writeFile(app_dir.filePath("data/userstore-TestAccount.db"), "rows");

    const QString backup = BackupDataOnNewVersion(app_dir, "1.2.2", "1.2.3");
    QCOMPARE(backup, app_dir.filePath("data-backup-1.2.2"));
    QCOMPARE(backupDirs(app_dir), QStringList{"data-backup-1.2.2"});
    QCOMPARE(QDir(backup).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList{"userstore-TestAccount.db"});
}

namespace {

    // Run `trigger`, and answer the first modal dialog of type T that it
    // opens with `answer`. Returns whether such a dialog was opened.
    template<typename T>
    bool answerModal(const std::function<void()> &trigger, const std::function<void(T &)> &answer)
    {
        bool answered = false;
        QTimer poll;
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (auto *modal = qobject_cast<T *>(QApplication::activeModalWidget())) {
                poll.stop();
                answered = true;
                answer(*modal);
            }
        });
        poll.start();
        trigger();
        return answered;
    }

    constexpr const char *NEWER_RELEASES
        = R"json([{"tag_name":"v99.0.0","draft":false,"prerelease":false}])json";

    QPushButton *findUpdateButton(MainWindow &window)
    {
        for (auto *button : window.findChildren<QPushButton *>()) {
            if (button->text() == "Update available") {
                return button;
            }
        }
        return nullptr;
    }

    // Answer the most recent update check with the given GitHub releases.
    void answerUpdateCheck(FakeNetworkManager &network, const QByteArray &releases)
    {
        QVERIFY(network.count() > 0);
        auto reply = network.sent(size_t(network.count() - 1)).reply;
        QVERIFY(reply);
        reply->finish({}, 200, QNetworkReply::NoError, releases);
    }

    // Press `button` and answer the update prompt by clicking the button
    // labelled `choice`. Returns the prompt's text, or a null string when no
    // prompt opened.
    QString answerUpdatePrompt(QPushButton &button, const QString &choice)
    {
        QString text;
        answerModal<QMessageBox>([&button] { button.click(); },
                                 [&](QMessageBox &prompt) {
                                     text = prompt.text();
                                     for (auto *option : prompt.buttons()) {
                                         if (option->text().trimmed() == choice) {
                                             option->click();
                                             return;
                                         }
                                     }
                                     prompt.reject();
                                 });
        return text;
    }

} // namespace

void ShellTest::updateButtonOpensUpdatePrompt()
{
    MainWindowFixture fixture;
    FakeNetworkManager network;
    UpdateChecker checker(*fixture.settings, network);
    ConnectUpdateChecker(*fixture.window, checker);

    checker.CheckForUpdates();
    answerUpdateCheck(network, NEWER_RELEASES);
    auto *button = findUpdateButton(*fixture.window);
    QVERIFY(button);
    QVERIFY(!button->isHidden());
    const int checks = network.count();

    const QString prompt = answerUpdatePrompt(*button, "Ignore");
    QVERIFY2(!prompt.isNull(), "clicking 'Update available' opened no prompt");
    QVERIFY(prompt.contains("99.0.0"));
    // The click asks the user; it does not start another check.
    QCOMPARE(network.count(), checks);
    // "Ignore" asks again later, so the button stays.
    QVERIFY(!button->isHidden());
}

void ShellTest::skippedReleaseIsNotOfferedAgain()
{
    MainWindowFixture fixture;
    FakeNetworkManager network;
    UpdateChecker checker(*fixture.settings, network);
    ConnectUpdateChecker(*fixture.window, checker);
    QSignalSpy available(&checker, &UpdateChecker::UpdateAvailable);

    checker.CheckForUpdates();
    answerUpdateCheck(network, NEWER_RELEASES);
    QCOMPARE(available.count(), 1);
    auto *button = findUpdateButton(*fixture.window);
    QVERIFY(button);

    const QString prompt = answerUpdatePrompt(*button, "Ignore, and don't ask again");
    QVERIFY2(!prompt.isNull(), "clicking 'Update available' opened no prompt");
    QCOMPARE(fixture.settings->value("skip_release").toString(), QString("99.0.0"));
    // #284: nothing newer is left to offer, so the button goes.
    QVERIFY(button->isHidden());

    QCOMPARE(available.count(), 2);
    QCOMPARE(available.last().at(0).toBool(), false);

    // The periodic check finds the same release: it is not offered again.
    checker.CheckForUpdates();
    answerUpdateCheck(network, NEWER_RELEASES);
    QCOMPARE(available.count(), 2);
    QVERIFY(button->isHidden());
}

// Before login, Application connects UpdateAvailable straight to the prompt.
// Declining with "don't ask again" reports that nothing is left to offer,
// which must not open a second prompt.
void ShellTest::declinedUpdateBeforeLoginOpensOnePrompt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    FakeNetworkManager network;
    UpdateChecker checker(settings, network);
    QObject::connect(&checker, &UpdateChecker::UpdateAvailable, &checker, [&checker] {
        AskUserToUpdate(checker);
    });

    // Answer every modal that opens: the first with "don't ask again", any
    // later one by closing it.
    int prompts = 0;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto *modal = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (!modal) {
            return;
        }
        if (++prompts == 1) {
            for (auto *option : modal->buttons()) {
                if (option->text().trimmed() == "Ignore, and don't ask again") {
                    option->click();
                    return;
                }
            }
        }
        modal->reject();
    });
    poll.start();
    checker.CheckForUpdates();
    answerUpdateCheck(network, NEWER_RELEASES);
    poll.stop();

    QCOMPARE(prompts, 1);
    QCOMPARE(settings.value("skip_release").toString(), QString("99.0.0"));
}

// The button and the prompt agree on what there is to offer. Running R, the
// user declined release R+1 for good before any prerelease existed; then
// R+1-beta.1 is listed. It is older than the declined release, so it is not
// offered: no button, and before login no prompt at all (it used to open
// "No updates appear to be available").
void ShellTest::prereleaseOfADeclinedReleaseIsNotOffered()
{
    const QVersionNumber running = QVersionNumber::fromString(APP_VERSION_STRING);
    const QString next = QString("%1.%2.%3")
                             .arg(running.majorVersion())
                             .arg(running.minorVersion())
                             .arg(running.microVersion() + 1);
    const QByteArray releases = QString(R"json([
        {"tag_name":"v%1","draft":false,"prerelease":false},
        {"tag_name":"v%1-beta.1","draft":false,"prerelease":true}])json")
                                    .arg(next)
                                    .toUtf8();

    MainWindowFixture fixture;
    // What "Ignore, and don't ask again" stores when no prerelease exists.
    fixture.settings->setValue("skip_release", next);
    fixture.settings->setValue("skip_prerelease", QString::fromStdString(semver::version().str()));
    FakeNetworkManager network;
    UpdateChecker checker(*fixture.settings, network);
    ConnectUpdateChecker(*fixture.window, checker);
    // The connection Application makes before login.
    QObject::connect(&checker, &UpdateChecker::UpdateAvailable, &checker, [&checker] {
        AskUserToUpdate(checker);
    });
    QSignalSpy available(&checker, &UpdateChecker::UpdateAvailable);

    int prompts = 0;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (auto *modal = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            ++prompts;
            modal->reject();
        }
    });
    poll.start();
    checker.CheckForUpdates();
    answerUpdateCheck(network, releases);
    poll.stop();

    QCOMPARE(prompts, 0);
    QCOMPARE(available.count(), 0);
    auto *button = findUpdateButton(*fixture.window);
    QVERIFY(button);
    QVERIFY(button->isHidden());
}

void ShellTest::imageCacheDeletesReplies_data()
{
    QTest::addColumn<bool>("fails");
    QTest::newRow("success") << false;
    QTest::newRow("network error") << true;
}

void ShellTest::imageCacheDeletesReplies()
{
    QFETCH(bool, fails);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    ImageCache cache(network, dir.filePath("cache"));

    QByteArray png;
    {
        QImage image(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
    }

    cache.fetch("https://web.poecdn.com/image/Art/2DItems/Rings/Ring1.png");
    QCOMPARE(network.count(), 1);
    QPointer<InFlightReply> reply = network.sent(0).reply;
    QVERIFY(reply);
    if (fails) {
        reply->finish({}, 404, QNetworkReply::ContentNotFoundError);
    } else {
        reply->finish({}, 200, QNetworkReply::NoError, png);
    }
    QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 500);
}

void ShellTest::updateCheckerDeletesReplies_data()
{
    QTest::addColumn<bool>("fails");
    QTest::newRow("success") << false;
    QTest::newRow("network error") << true;
}

void ShellTest::updateCheckerDeletesReplies()
{
    QFETCH(bool, fails);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    FakeNetworkManager network;
    UpdateChecker checker(settings, network);

    checker.CheckForUpdates();
    QCOMPARE(network.count(), 1);
    QPointer<InFlightReply> reply = network.sent(0).reply;
    QVERIFY(reply);
    if (fails) {
        reply->finish({}, 503, QNetworkReply::ServiceUnavailableError);
    } else {
        reply->finish({}, 200, QNetworkReply::NoError, "[]");
    }
    QTRY_VERIFY_WITH_TIMEOUT(reply.isNull(), 500);
}

void ShellTest::networkManagerHasNoDiskCache()
{
    // The old cache lived under AppLocalDataLocation whatever --data-dir
    // said (#232). In test mode that location is Qt's test sandbox, never
    // the user's real data directory.
    QVERIFY(QStandardPaths::isTestModeEnabled());
    const QString location = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    // Qt's test directory: ~/.qttest on Unix and macOS, a "qttest" directory
    // under %APPDATA% on Windows. Refuse to delete anything outside it.
    const QStringList segments = QDir::fromNativeSeparators(location).split('/');
    QVERIFY2(segments.contains(".qttest") || segments.contains("qttest"), qPrintable(location));
    QDir cache_dir(location + "/network_cache");
    QVERIFY(cache_dir.removeRecursively());

    NetworkManager network;
    QVERIFY(!cache_dir.exists());
    QVERIFY(network.cache() == nullptr);
}

// A one-shot HTTP server on loopback that answers every request with the
// same cacheable reply and counts the requests it received.
class CacheableReplyServer : public QTcpServer
{
public:
    int requests = 0;

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
            m_buffer += socket->readAll();
            if (!m_buffer.contains("\r\n\r\n")) {
                return;
            }
            m_buffer.clear();
            ++requests;
            socket->write("HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain\r\n"
                          "Cache-Control: public, max-age=3600\r\n"
                          "Last-Modified: Mon, 02 Nov 2026 00:00:00 GMT\r\n"
                          "Content-Length: 5\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "hello");
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

private:
    QByteArray m_buffer;
};

void ShellTest::repeatedGetIsNotServedFromCache()
{
    CacheableReplyServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QUrl url(QString("http://127.0.0.1:%1/cacheable").arg(server.serverPort()));

    NetworkManager network;
    for (int i = 0; i < 2; ++i) {
        QNetworkReply *reply = network.get(QNetworkRequest(url));
        QSignalSpy finished(reply, &QNetworkReply::finished);
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->error(), QNetworkReply::NoError);
        QCOMPARE(reply->readAll(), QByteArray("hello"));
        QVERIFY(!reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool());
        reply->deleteLater();
    }
    QCOMPARE(server.requests, 2);
}

void ShellTest::legacyNetworkCacheIsRemoved()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    QVERIFY(data_dir.mkpath("network_cache/data9/0"));
    QVERIFY(data_dir.mkpath("cache"));
    for (const QString name : {"network_cache/data9/0/entry.d", "cache/icon.png", "settings.ini"}) {
        QFile file(data_dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
    }

    NetworkManager::removeLegacyDiskCache(data_dir);
    QVERIFY(!data_dir.exists("network_cache"));
    QVERIFY(data_dir.exists("cache/icon.png"));
    QVERIFY(data_dir.exists("settings.ini"));

    // Idempotent: nothing to remove is not an error.
    NetworkManager::removeLegacyDiskCache(data_dir);
    QVERIFY(!data_dir.exists("network_cache"));
}

// Receives the URLs QDesktopServices would open, so an OAuth grant() in a
// test never starts a browser.
class UrlCatcher : public QObject
{
    Q_OBJECT

public:
    explicit UrlCatcher(const QString &scheme)
        : m_scheme(scheme)
    {
        QDesktopServices::setUrlHandler(m_scheme, this, "open");
    }
    ~UrlCatcher() override { QDesktopServices::unsetUrlHandler(m_scheme); }

    QList<QUrl> urls;

public slots:
    void open(const QUrl &url) { urls.append(url); }

private:
    QString m_scheme;
};

// The address the OAuth callback server is bound to, or a null address
// when it is not listening.
static QHostAddress callbackAddress(const OAuthManager &oauth)
{
    const auto *handler = oauth.findChild<QOAuthHttpServerReplyHandler *>();
    if (!handler || !handler->isListening()) {
        return {};
    }
    const auto servers = handler->findChildren<QTcpServer *>();
    for (const QTcpServer *server : servers) {
        if (server->isListening()) {
            return server->serverAddress();
        }
    }
    return {};
}

void ShellTest::legacyNetworkCacheLinkIsNotFollowed()
{
    // A network_cache that is a link to a directory elsewhere: the clean-up
    // must not delete what the link points at.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir root(dir.path());
    QVERIFY(root.mkpath("elsewhere"));
    QVERIFY(root.mkpath("data"));
    {
        QFile keep(root.filePath("elsewhere/keep.txt"));
        QVERIFY(keep.open(QIODevice::WriteOnly));
        keep.write("x");
    }
    const QString link = root.filePath("data/network_cache");
    if (!QFile::link(root.filePath("elsewhere"), link) || !QFileInfo(link).isSymbolicLink()) {
        QSKIP("symbolic links to directories cannot be created here");
    }

    NetworkManager::removeLegacyDiskCache(QDir(root.filePath("data")));
    QVERIFY(root.exists("elsewhere/keep.txt"));
    QVERIFY(!QFileInfo(link).isSymbolicLink());
    QVERIFY(!QFileInfo::exists(link));
}

void ShellTest::loginListensOnLoopbackAfterClose()
{
    UrlCatcher browser("https");
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    auto *handler = services.oauth.findChild<QOAuthHttpServerReplyHandler *>();
    QVERIFY(handler);

    // As constructed, nothing listens: only a browser login that waits does.
    QVERIFY(!handler->isListening());

    // A login listens on loopback, not on every interface; and again after
    // the listener was closed.
    QVERIFY(services.oauth.initLogin());
    QVERIFY(callbackAddress(services.oauth).isLoopback());
    services.oauth.endLogin();
    QVERIFY(!handler->isListening());
    QVERIFY(services.oauth.initLogin());
    const QHostAddress address = callbackAddress(services.oauth);
    QVERIFY2(address.isLoopback(), qPrintable(address.toString()));
    QCOMPARE(browser.urls.size(), 2);
}

void ShellTest::everyLoginRedirectsToTheListenerItBinds()
{
    // Each login binds IPv4 loopback itself, and the authorization url sent
    // to the browser names the port that listener has.
    UrlCatcher browser("https");
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    auto *handler = services.oauth.findChild<QOAuthHttpServerReplyHandler *>();
    QVERIFY(handler);

    for (int login = 1; login <= 2; ++login) {
        QVERIFY(services.oauth.initLogin());
        QCOMPARE(callbackAddress(services.oauth), QHostAddress(QHostAddress::LocalHost));
        QCOMPARE(browser.urls.size(), login);
        const QUrl redirect(
            QUrlQuery(browser.urls.last()).queryItemValue("redirect_uri", QUrl::FullyDecoded));
        QCOMPARE(redirect.host(), QString("127.0.0.1"));
        QCOMPARE(redirect.port(), int(handler->port()));
    }
}

void ShellTest::startupRefreshClosesTheListener()
{
    // A stored token is refreshed when OAuthManager is constructed. Nothing
    // listens for a redirect then, nor once the refresh is granted: the
    // listener is open only while a browser login waits, and the login
    // dialog's "Re-authenticate" opens it in initLogin().
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    SqliteDataStore datastore(dir.filePath("data/global.db"));
    datastore.Set("oauth_token", R"({"username":"TestAccount","refresh_token":"old-refresh"})");
    OAuthManager oauth(network, datastore);
    QSignalSpy granted(&oauth, &OAuthManager::grantAccess);
    QVERIFY(callbackAddress(oauth).isNull());

    QTRY_COMPARE_WITH_TIMEOUT(network.count(), 1, 1000);
    QVERIFY(network.sent(0).request.url().path().endsWith("/oauth/token"));
    QPointer<InFlightReply> reply = network.sent(0).reply;
    QVERIFY(reply);
    reply->finish({{"Content-Type", "application/json"}},
                  200,
                  QNetworkReply::NoError,
                  R"({"access_token":"new-access","expires_in":36000,"token_type":"bearer",)"
                  R"("scope":"account:leagues account:stashes account:characters",)"
                  R"("username":"TestAccount","sub":"id","refresh_token":"new-refresh"})");

    QTRY_COMPARE_WITH_TIMEOUT(granted.count(), 1, 1000);
    QVERIFY(callbackAddress(oauth).isNull());
}

// The update prompt as the user sees it, and what its other two answers do:
// "Go to Github" opens the downloads page, and "Ignore", like it, forgets a
// release declined for good earlier. Either way the button stays.
void ShellTest::updatePromptAnswersAreApplied()
{
    MainWindowFixture fixture;
    // An older release, declined for good earlier.
    fixture.settings->setValue("skip_release", "98.0.0");
    fixture.settings->setValue("skip_prerelease", "98.0.0");
    FakeNetworkManager network;
    UpdateChecker checker(*fixture.settings, network);
    ConnectUpdateChecker(*fixture.window, checker);
    UrlCatcher browser("https");

    checker.CheckForUpdates();
    answerUpdateCheck(network,
                      R"json([{"tag_name":"v99.0.0","draft":false,"prerelease":false},
                              {"tag_name":"v100.0.0-beta.1","draft":false,"prerelease":true}])json");
    auto *button = findUpdateButton(*fixture.window);
    QVERIFY(button);
    QVERIFY(!button->isHidden());

    QString text;
    QString default_button;
    QStringList options;
    const bool prompted
        = answerModal<QMessageBox>([&] { button->click(); },
                                   [&](QMessageBox &prompt) {
                                       text = prompt.text();
                                       default_button = prompt.defaultButton()->text();
                                       for (auto *option : prompt.buttons()) {
                                           options << option->text();
                                       }
                                       for (auto *option : prompt.buttons()) {
                                           if (option->text().trimmed() == "Go to Github") {
                                               option->click();
                                               return;
                                           }
                                       }
                                       prompt.reject();
                                   });
    QVERIFY(prompted);
    QCOMPARE(text,
             QString("A newer release is available:\n   99.0.0\n\n"
                     "A newer prerelease is available:\n   100.0.0-beta.1"));
    options.sort();
    QCOMPARE(options,
             (QStringList{"  Go to Github  ", "  Ignore  ", "  Ignore, and don't ask again  "}));
    QCOMPARE(default_button, QString("  Ignore, and don't ask again  "));
    QCOMPARE(browser.urls,
             QList<QUrl>{QUrl("https://github.com/smallcatlabs/acquisition-qt/releases")});
    QCOMPARE(fixture.settings->value("skip_release").toString(), QString());
    QCOMPARE(fixture.settings->value("skip_prerelease").toString(), QString());
    QVERIFY(!button->isHidden());

    // "Ignore" forgets a declined release too, and opens nothing.
    fixture.settings->setValue("skip_release", "98.0.0");
    fixture.settings->setValue("skip_prerelease", "98.0.0");
    QVERIFY(!answerUpdatePrompt(*button, "Ignore").isNull());
    QCOMPARE(fixture.settings->value("skip_release").toString(), QString());
    QCOMPARE(fixture.settings->value("skip_prerelease").toString(), QString());
    QCOMPARE(browser.urls.size(), 1);
    QVERIFY(!button->isHidden());
}

void ShellTest::msvcCheckPassesWithoutLibraries()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    bool result = false;
    const bool prompted = answerModal<QMessageBox>(
        [&] { result = checkApplicationDirectory(QDir(dir.path()), {"msvcp140.dll"}); },
        [](QMessageBox &box) { box.reject(); });
    QVERIFY(!prompted);
    QVERIFY(result);
}

void ShellTest::msvcCheckAnswer_data()
{
    QTest::addColumn<QString>("choice");
    QTest::addColumn<bool>("continues");
    QTest::addColumn<bool>("opensFolder");
    QTest::newRow("ignore") << "Ignore and continue" << true << false;
    QTest::newRow("quit") << "Quit" << false << false;
    QTest::newRow("open folder") << "Open folder and quit" << false << true;
}

void ShellTest::msvcCheckAnswer()
{
    // An app-local runtime DLL beside the executable: the user's choice
    // decides, and quitting is a clean exit, not an abort.
    QFETCH(QString, choice);
    QFETCH(bool, continues);
    QFETCH(bool, opensFolder);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    {
        QFile dll(dir.filePath("vcruntime140.dll"));
        QVERIFY(dll.open(QIODevice::WriteOnly));
    }
    UrlCatcher folder("file");

    bool result = !continues;
    const bool prompted = answerModal<QMessageBox>(
        [&] {
            result = checkApplicationDirectory(QDir(dir.path()),
                                               {"msvcp140.dll", "vcruntime140.dll"});
        },
        [&](QMessageBox &box) {
            for (auto *button : box.buttons()) {
                if (button->text() == choice) {
                    button->click();
                    return;
                }
            }
            box.reject();
        });
    QVERIFY(prompted);
    QCOMPARE(result, continues);
    QCOMPARE(folder.urls.size(), opensFolder ? 1 : 0);
}

void ShellTest::fatalErrorReportsWithLoggingOff()
{
    // "off" is a selectable log level; the report must not depend on it.
    const auto level = spdlog::get_level();
    spdlog::set_level(spdlog::level::off);
    std::FILE *out = std::tmpfile();
    QVERIFY(out);
    ReportFatalError("invariant broken: test", out);
    spdlog::set_level(level);

    std::fflush(out);
    std::rewind(out);
    char buffer[256] = {};
    const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, out);
    std::fclose(out);
    QVERIFY2(QByteArray(buffer, qsizetype(n)).contains("invariant broken: test"), buffer);
}

void ShellTest::freshDataDirTakesNoBackup()
{
    // A first run has nothing to back up: opening the global store must not
    // make its own brand-new database look like old data (#292).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    QSettings settings(data_dir.filePath("settings.ini"), QSettings::IniFormat);

    const auto store = OpenGlobalDataStore(data_dir, settings);
    QVERIFY(store);
    const QStringList backups = data_dir.entryList({"data-backup-*"}, QDir::AllEntries);
    QVERIFY2(backups.isEmpty(), qPrintable(backups.join(", ")));
    QCOMPARE(store->Get("version", ""), QString(APP_VERSION_STRING));
}

void ShellTest::legacyStoreVersionNamesTheBackup()
{
    // The version in the global store names the backup.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    const QString global_file = data_dir.filePath("data/" + SqliteDataStore::MakeFilename("", ""));
    SqliteDataStore(global_file).Set("version", "0.9.0");
    QSettings settings(data_dir.filePath("settings.ini"), QSettings::IniFormat);

    const auto store = OpenGlobalDataStore(data_dir, settings);
    QVERIFY(store);
    QVERIFY(data_dir.exists("data-backup-0.9.0"));
    QCOMPARE(store->Get("version", ""), QString(APP_VERSION_STRING));
}

void ShellTest::fullBackupSeriesStillRecordsVersion()
{
    // Every backup name for the stored version is taken (#446): no backup can
    // be made, and the version must be recorded all the same, or every later
    // start comes back here.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    QVERIFY(data_dir.mkpath("data"));
    writeFile(data_dir.filePath("data/userstore-TestAccount.db"), "rows");
    QVERIFY(data_dir.mkpath("data-backup-UNKNOWN-VERSION"));
    for (int n = 1; n <= 20; ++n) {
        QVERIFY(data_dir.mkpath(QString("data-backup-UNKNOWN-VERSION-%1").arg(n)));
    }
    QSettings settings(data_dir.filePath("settings.ini"), QSettings::IniFormat);

    {
        const auto store = OpenGlobalDataStore(data_dir, settings);
        QVERIFY(store);
        QCOMPARE(store->Get("version", ""), QString(APP_VERSION_STRING));
    }
    QCOMPARE(backupDirs(data_dir).size(), 21);

    // With a name free again, a start that still saw an unknown version would
    // fill it.
    QVERIFY(QDir(data_dir.filePath("data-backup-UNKNOWN-VERSION-20")).removeRecursively());
    const auto store = OpenGlobalDataStore(data_dir, settings);
    QVERIFY(store);
    QVERIFY(!data_dir.exists("data-backup-UNKNOWN-VERSION-20"));
}

void ShellTest::clearedSettingsTakeNoBackup()
{
    // The version is kept with the data it describes: losing settings.ini
    // does not make the data look like it came from an unknown version (#446).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    QSettings settings(data_dir.filePath("settings.ini"), QSettings::IniFormat);

    {
        const auto store = OpenGlobalDataStore(data_dir, settings);
        QVERIFY(store);
    }
    writeFile(data_dir.filePath("data/userstore-TestAccount.db"), "rows");
    settings.clear();

    const auto store = OpenGlobalDataStore(data_dir, settings);
    QVERIFY(store);
    QCOMPARE(backupDirs(data_dir), QStringList());
}

void ShellTest::settingsVersionMovesIntoTheStore()
{
    // v0.16 to v0.18 recorded the version in settings.ini. It still names the
    // backup of an upgrade from one of them, and then moves into the store.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    const QString global_file = data_dir.filePath("data/" + SqliteDataStore::MakeFilename("", ""));
    // Left in the store by a version older still; the setting is the later word.
    SqliteDataStore(global_file).Set("version", "0.9.0");
    QSettings settings(data_dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("version", "0.0.1-old");

    const auto store = OpenGlobalDataStore(data_dir, settings);
    QVERIFY(store);
    QCOMPARE(backupDirs(data_dir), QStringList{"data-backup-0.0.1-old"});
    QVERIFY(!settings.contains("version"));
    QCOMPARE(store->Get("version", ""), QString(APP_VERSION_STRING));
}

void ShellTest::imageCacheSignalsFailedFetch()
{
    // The item panel waits for imageReady to replace "Loading..."; a failed
    // fetch must say it is done too.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    ImageCache cache(network, dir.filePath("cache"));
    QSignalSpy ready(&cache, &ImageCache::imageReady);

    const QString url = "https://web.poecdn.com/image/Art/2DItems/Rings/Ring1.png";
    cache.fetch(url);
    QCOMPARE(network.count(), 1);
    network.sent(0).reply->finish({}, 404, QNetworkReply::ContentNotFoundError);

    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.at(0).at(0).toString(), url);
    QVERIFY(!cache.contains(url));
    QVERIFY(cache.load(url).isNull());
}

// A reply whose url() is not the one requested, as after a redirect.
class RedirectedReply : public InFlightReply
{
public:
    RedirectedReply(const QNetworkRequest &request, const QUrl &final_url, QObject *parent)
        : InFlightReply(request, parent)
    {
        setUrl(final_url);
    }
};

class RedirectingNetworkManager : public NetworkManager
{
public:
    QList<QPointer<InFlightReply>> replies;

protected:
    QNetworkReply *createRequest(QNetworkAccessManager::Operation,
                                 const QNetworkRequest &request,
                                 QIODevice *) override
    {
        auto *reply = new RedirectedReply(request,
                                          QUrl("https://web.poecdn.com/redirected/Ring1.png"),
                                          this);
        replies.append(reply);
        return reply;
    }
};

void ShellTest::imageCacheKeysOnRequestedUrl()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RedirectingNetworkManager network;
    ImageCache cache(network, dir.filePath("cache"));
    QSignalSpy ready(&cache, &ImageCache::imageReady);

    QByteArray png;
    {
        QImage image(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::red);
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
    }

    const QString url = "https://web.poecdn.com/image/Art/2DItems/Rings/Ring1.png";
    cache.fetch(url);
    QCOMPARE(network.replies.size(), 1);
    QVERIFY(network.replies.at(0)->url() != QUrl(url));
    network.replies.at(0)->finish({}, 200, QNetworkReply::NoError, png);

    QCOMPARE(ready.count(), 1);
    QCOMPARE(ready.at(0).at(0).toString(), url);
    QVERIFY(cache.contains(url));
    QVERIFY(!cache.load(url).isNull());
}

void ShellTest::imageCacheKeepsNoFileForUndecodableBody()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    ImageCache cache(network, dir.filePath("cache"));
    QSignalSpy ready(&cache, &ImageCache::imageReady);

    const QString url = "https://web.poecdn.com/image/Art/2DItems/Rings/Ring1.png";
    cache.fetch(url);
    QCOMPARE(network.count(), 1);
    network.sent(0).reply->finish({}, 200, QNetworkReply::NoError, "<html>not an image</html>");

    QCOMPARE(ready.count(), 1);
    QVERIFY(!cache.contains(url));
    const QStringList files = QDir(dir.filePath("cache")).entryList(QDir::Files);
    QVERIFY2(files.isEmpty(), qPrintable(files.join(", ")));
}

void ShellTest::dataDirLockRefusesSecondHolder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.filePath("fresh")); // not created yet

    auto first = LockDataDirectory(data_dir);
    QVERIFY(first);
    QVERIFY(first->isLocked());
    QVERIFY(LockDataDirectory(data_dir) == nullptr);

    first.reset();
    auto again = LockDataDirectory(data_dir);
    QVERIFY(again);
    QVERIFY(again->isLocked());
}

void ShellTest::dataDirLockTakesOverStaleLock()
{
    // A crashed instance leaves its lock file behind. Build that file from
    // a real lock file's content with the PID of a process that has exited.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());

    // A process that has exited: this suite, asked only for its help text,
    // which needs no shell or other program on any platform. The application
    // name is the suite's name (testsuite.h).
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {QCoreApplication::applicationName(), "-help"});
    QVERIFY(child.waitForStarted());
    const qint64 dead_pid = child.processId();
    QVERIFY(child.waitForFinished());
    {
        QLockFile probe(data_dir.filePath("probe.lock"));
        QVERIFY(probe.lock());
        qint64 pid = 0;
        QString host;
        QString app;
        QVERIFY(probe.getLockInfo(&pid, &host, &app));
        QFile file(data_dir.filePath("probe.lock"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        file.close();
        // Control: the same file naming a live process is not stale.
        {
            QFile live(data_dir.filePath("acquisition.lock"));
            QVERIFY(live.open(QIODevice::WriteOnly));
            live.write(content);
        }
        QVERIFY(LockDataDirectory(data_dir) == nullptr);

        // Replace the PID in its own line (the first) only: its digits may
        // also occur in the host name or machine id lines.
        QList<QByteArray> lines = content.split('\n');
        QCOMPARE(lines.at(0), QByteArray::number(pid));
        lines[0] = QByteArray::number(dead_pid);
        content = lines.join('\n');
        QFile stale(data_dir.filePath("acquisition.lock"));
        QVERIFY(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
        stale.write(content);
    }

    auto lock = LockDataDirectory(data_dir);
    QVERIFY(lock);
    QVERIFY(lock->isLocked());
}

// Writes acquisition.lock as a lock taken on another computer would be:
// a real lock file's content with the host name line replaced.
static bool writeForeignHostLock(const QDir &data_dir)
{
    QLockFile probe(data_dir.filePath("probe.lock"));
    if (!probe.lock()) {
        return false;
    }
    QFile file(data_dir.filePath("probe.lock"));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QList<QByteArray> lines = file.readAll().split('\n');
    if (lines.size() < 3) {
        return false;
    }
    lines[2] = "another-computer.example";
    QFile foreign(data_dir.filePath("acquisition.lock"));
    return foreign.open(QIODevice::WriteOnly) && (foreign.write(lines.join('\n')) > 0);
}

void ShellTest::dataDirLockRefusesForeignHostLock()
{
    // A lock taken on another computer (a shared or synced data directory)
    // cannot be judged stale from here, so it is refused, not taken over.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    QVERIFY(writeForeignHostLock(data_dir));
    QVERIFY(LockDataDirectory(data_dir) == nullptr);

    const QString message = DataDirLockRefusal(data_dir);
    QVERIFY2(message.contains("another-computer.example"), qPrintable(message));
    QVERIFY2(message.contains(QDir::toNativeSeparators(data_dir.filePath("acquisition.lock"))),
             qPrintable(message));
}

void ShellTest::dataDirLockRefusalNamesHolderAndFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir data_dir(dir.path());
    const auto held = LockDataDirectory(data_dir);
    QVERIFY(held && held->isLocked());

    const QString message = DataDirLockRefusal(data_dir);
    const QString lock_path = QDir::toNativeSeparators(data_dir.filePath("acquisition.lock"));
    QVERIFY2(message.contains(QDir::toNativeSeparators(data_dir.absolutePath())),
             qPrintable(message));
    QVERIFY2(message.contains(lock_path), qPrintable(message));
    QVERIFY2(message.contains(QString::number(QCoreApplication::applicationPid())),
             qPrintable(message));

    // An unreadable lock file: the message still names the file.
    QTemporaryDir other;
    QVERIFY(other.isValid());
    {
        QFile garbage(QDir(other.path()).filePath("acquisition.lock"));
        QVERIFY(garbage.open(QIODevice::WriteOnly));
        garbage.write("not a lock file");
    }
    const QString unknown = DataDirLockRefusal(QDir(other.path()));
    QVERIFY2(unknown.contains(
                 QDir::toNativeSeparators(QDir(other.path()).filePath("acquisition.lock"))),
             qPrintable(unknown));
}

ACQ_TEST_MAIN(ShellTest)
#include "tst_shell.moc"
