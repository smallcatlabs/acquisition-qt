// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QSettings>
#include <QTemporaryDir>

#include "util/crashreporting.h"
#include "util/logging.h"

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    void crashReportingDefaultsOn();
    void crashReportingFollowsSetting_data();
    void crashReportingFollowsSetting();
    void logLevelParsing_data();
    void logLevelParsing();
};

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

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
