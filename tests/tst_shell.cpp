// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QApplication>
#include <QFileDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>
#include <memory>

#include "datastore/sqlitedatastore.h"
#include "fakenetworkmanager.h"
#include "ui/logindialog.h"
#include "util/crashreporting.h"
#include "util/databackup.h"
#include "util/logging.h"
#include "util/oauthmanager.h"

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
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
};

void ShellTest::initTestCase()
{
    // NetworkManager creates a disk cache in AppLocalDataLocation.
    QStandardPaths::setTestModeEnabled(true);
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

    // Press the login dialog's data-directory button and answer the file
    // dialog it opens with `answer` once that dialog is showing.
    bool pressUserDirButton(LoginDialog &dialog, const std::function<void(QFileDialog &)> &answer)
    {
        auto *button = dialog.findChild<QPushButton *>("userDirButton");
        if (!button) {
            return false;
        }
        bool answered = false;
        QTimer poll;
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                poll.stop();
                answered = true;
                answer(*picker);
            }
        });
        poll.start();
        emit button->pressed();
        return answered;
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

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
