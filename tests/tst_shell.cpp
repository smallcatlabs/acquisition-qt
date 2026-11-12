// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "datastore/sqlitedatastore.h"
#include "fakenetworkmanager.h"
#include "mainwindowfixture.h"
#include "ui/logindialog.h"
#include "ui/mainwindow_bridge.h"
#include "util/crashreporting.h"
#include "util/databackup.h"
#include "util/logging.h"
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

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
};

void ShellTest::initTestCase()
{
    // NetworkManager creates a disk cache in AppLocalDataLocation.
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

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
