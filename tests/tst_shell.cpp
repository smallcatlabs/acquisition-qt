// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QSettings>
#include <QTemporaryDir>

#include "util/crashreporting.h"

class ShellTest : public QObject
{
    Q_OBJECT

private slots:
    void crashReportingDefaultsOn();
    void crashReportingFollowsSetting_data();
    void crashReportingFollowsSetting();
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

QTEST_MAIN(ShellTest)
#include "tst_shell.moc"
