// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QApplication>
#include <QBuffer>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QOAuthHttpServerReplyHandler>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "datastore/sqlitedatastore.h"
#include "fakenetworkmanager.h"
#include "imagecache.h"
#include "mainwindowfixture.h"
#include "ui/logindialog.h"
#include "ui/mainwindow_bridge.h"
#include "util/crashreporting.h"
#include "util/databackup.h"
#include "util/logging.h"
#include "util/networkmanager.h"
#include "util/oauthmanager.h"
#include "util/updatechecker.h"

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void crashReportingDefaultsOn();
    void crashReportingFollowsSetting_data();
    void crashReportingFollowsSetting();
    void logLevelParsing_data();
    void logLevelParsing();
    void forgettingUserKeepsOtherSettings();
    void backupSkippedWhenVersionCurrent();
    void backupSkippedWithoutDataDirRecordsVersion();
    void backupSkippedForEmptyDataDirRecordsVersion();
    void backupTakenOnNewVersion();
    void cancellingDataDirPickerChangesNothing();
    void choosingDataDirEmitsIt();
    void updateButtonOpensUpdatePrompt();
    void skippedReleaseIsNotOfferedAgain();
    void imageCacheDeletesReplies_data();
    void imageCacheDeletesReplies();
    void updateCheckerDeletesReplies_data();
    void updateCheckerDeletesReplies();
    void networkManagerHasNoDiskCache();
    void repeatedGetIsNotServedFromCache();
    void legacyNetworkCacheIsRemoved();
    void legacyNetworkCacheLinkIsNotFollowed();
    void loginListensOnLoopbackAfterClose();
    void startupRefreshClosesTheListener();

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
        return std::make_unique<LoginDialog>(QDir(dir.path()), settings, network, oauth, datastore);
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
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("version", "1.2.3");

    QCOMPARE(BackupDataOnNewVersion(app_dir, settings, "UNKNOWN-VERSION", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
    QCOMPARE(settings.value("version").toString(), QString("1.2.3"));
}

void ShellTest::backupSkippedWithoutDataDirRecordsVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("version", "1.2.2");

    QCOMPARE(BackupDataOnNewVersion(app_dir, settings, "UNKNOWN-VERSION", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
    QCOMPARE(settings.value("version").toString(), QString("1.2.3"));
}

void ShellTest::backupSkippedForEmptyDataDirRecordsVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QVERIFY(app_dir.mkpath("data"));
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("version", "1.2.2");

    QCOMPARE(BackupDataOnNewVersion(app_dir, settings, "UNKNOWN-VERSION", "1.2.3"), QString());
    QCOMPARE(backupDirs(app_dir), QStringList());
    QCOMPARE(settings.value("version").toString(), QString("1.2.3"));
}

void ShellTest::backupTakenOnNewVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QDir app_dir(dir.path());
    QVERIFY(app_dir.mkpath("data"));
    writeFile(app_dir.filePath("data/userstore-TestAccount.db"), "rows");
    QSettings settings(dir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("version", "1.2.2");

    const QString backup = BackupDataOnNewVersion(app_dir, settings, "UNKNOWN-VERSION", "1.2.3");
    QCOMPARE(backup, app_dir.filePath("data-backup-1.2.2"));
    QCOMPARE(backupDirs(app_dir), QStringList{"data-backup-1.2.2"});
    QCOMPARE(QDir(backup).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
             QStringList{"userstore-TestAccount.db"});
    QCOMPARE(settings.value("version").toString(), QString("1.2.3"));
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

    // Press the login dialog's data-directory button and answer the file
    // dialog it opens with `answer` once that dialog is showing.
    bool pressUserDirButton(LoginDialog &dialog, const std::function<void(QFileDialog &)> &answer)
    {
        auto *button = dialog.findChild<QPushButton *>("userDirButton");
        if (!button) {
            return false;
        }
        return answerModal<QFileDialog>([button] { emit button->pressed(); }, answer);
    }

} // namespace

void ShellTest::cancellingDataDirPickerChangesNothing()
{
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    auto dialog = services.makeDialog();
    QSignalSpy changed(dialog.get(), &LoginDialog::ChangeUserDir);

    QVERIFY(pressUserDirButton(*dialog, [](QFileDialog &picker) { picker.reject(); }));
    QCOMPARE(changed.count(), 0);
}

void ShellTest::choosingDataDirEmitsIt()
{
    LoginDialogServices services;
    QVERIFY(services.dir.isValid());
    QTemporaryDir other;
    QVERIFY(other.isValid());
    auto dialog = services.makeDialog();
    QSignalSpy changed(dialog.get(), &LoginDialog::ChangeUserDir);

    QVERIFY(pressUserDirButton(*dialog, [&](QFileDialog &picker) {
        picker.setDirectory(other.path());
        static_cast<QDialog &>(picker).accept(); // QFileDialog::accept is protected
    }));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(QFileInfo(changed.at(0).at(0).toString()).canonicalFilePath(),
             QFileInfo(other.path()).canonicalFilePath());
}

namespace {

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

    // The periodic check finds the same release: it is not offered again.
    checker.CheckForUpdates();
    answerUpdateCheck(network, NEWER_RELEASES);
    QCOMPARE(available.count(), 1);
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

    // As constructed, the handler listens on loopback.
    QVERIFY(callbackAddress(services.oauth).isLoopback());

    // A grant closes it (OAuthManager::receiveGrant); the next login must
    // listen on loopback again, not on every interface.
    handler->close();
    QVERIFY(!handler->isListening());
    QVERIFY(services.oauth.initLogin());
    const QHostAddress address = callbackAddress(services.oauth);
    QVERIFY2(address.isLoopback(), qPrintable(address.toString()));
    QCOMPARE(browser.urls.size(), 1);
}

void ShellTest::startupRefreshClosesTheListener()
{
    // A stored token is refreshed when OAuthManager is constructed. A
    // successful refresh emits granted(), which closes the callback server,
    // so the login dialog's "Re-authenticate" goes through the re-listen
    // path in initLogin().
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    SqliteDataStore datastore(dir.filePath("data/global.db"));
    datastore.Set("oauth_token", R"({"username":"TestAccount","refresh_token":"old-refresh"})");
    OAuthManager oauth(network, datastore);
    QSignalSpy granted(&oauth, &OAuthManager::grantAccess);
    QVERIFY(callbackAddress(oauth).isLoopback());

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

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
