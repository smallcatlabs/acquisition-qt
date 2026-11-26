// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Which tabs are open once a filter has come and gone (#387), as scenario
// tests on the real Application (appfixture.h). One scenario per process.
//
// The scene's item levels, which the ilvl filter here goes by: tab #16 holds
// 69 and 72, tab #22 holds 70, 78 and 80, tab #24 holds 46.

#include <QTest>

#include <QComboBox>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "appfixture.h"
#include "testsuite.h"
#include "viewtranscript.h"

class AppExpansionTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // The steps of the table on #387, to the one that removes the filter.
    void tabsOpenedWithExpandAllReturnAfterAFilter();
    // The tabs that were closed before the filter are closed after it.
    void tabsOpenedOneByOneReturnAfterAFilter();
    // What is opened or closed while a filter is on stays with the filter.
    void changesUnderAFilterStayWithTheFilter();
    // By Item is left the same way a filter is.
    void openedTabsReturnAfterByItem();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> m_sink_hub;
};

namespace {

    // Name and Price, without the selection marks: these scenarios select
    // nothing.
    QString shown(const AppFixture &fixture)
    {
        QStringList lines;
        for (const QString &row : viewTranscript(fixture.tree(), 2)) {
            lines << row.mid(2);
        }
        return lines.join('\n');
    }

    // What a scenario must end without: a request the script did not
    // answer, or a dialog box. Empty when there was neither.
    QString surprises(const AppFixture &fixture)
    {
        QStringList found;
        for (const QString &url : fixture.network->unanswered()) {
            found << "unanswered request: " + url;
        }
        for (const QString &dialog : fixture.dialogs) {
            found << "dialog: " + dialog;
        }
        return found.join('\n');
    }

    // Replaces what the box holds with `text`, as a user who selects it all
    // and types; an empty `text` deletes it.
    void retype(QLineEdit *box, const QString &text)
    {
        box->selectAll();
        if (text.isEmpty()) {
            QTest::keyClick(box, Qt::Key_Backspace);
        } else {
            QTest::keyClicks(box, text);
        }
    }

    const char *const kAllClosed = "[+] #16, \"KB/CF Champ\"\n"
                                   "[+] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                                   "[+] #24, \"Market\"";

    const char *const kAllOpen = "[-] #16, \"KB/CF Champ\"\n"
                                 "      Viper Suit Loricated Ringmail\n"
                                 "      Iron Will Support\n"
                                 "      Hollow Vision Hypnotic Eye Jewel\n"
                                 "[-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                                 "      Sol Buckle Chain Belt | price 2 chaos\n"
                                 "      Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                                 "      Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                                 "[-] #24, \"Market\"\n"
                                 "      Dragon Crown Close Helmet | price 3 chaos";

    // Item level 75 and up: one tab has a match.
    const char *const kFrom75 = "[-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                                "      Sol Buckle Chain Belt | price 2 chaos\n"
                                "      Hypnotic Finger Moonstone Ring | price 2 chaos";

} // namespace

void AppExpansionTest::initTestCase()
{
    // The log panel attaches its sink through the hub logging::init installs
    // on the main logger (F42), so the test's logger needs one too.
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    m_main_logger->sinks().push_back(m_sink_hub);
    spdlog::register_logger(m_main_logger);
}

void AppExpansionTest::cleanupTestCase()
{
    spdlog::drop("main");
}

void AppExpansionTest::tabsOpenedWithExpandAllReturnAfterAFilter()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    QCOMPARE(shown(fixture), QString(kAllClosed));

    fixture.menuAction("Expand All").trigger();
    QCOMPARE(shown(fixture), QString(kAllOpen));

    QLineEdit *minimum = fixture.numberBoxes("ilvl").first();
    retype(minimum, "75");
    QTRY_COMPARE(shown(fixture), QString(kFrom75));

    // Not a number, so no filter: every tab is back, open as it was.
    retype(minimum, "5%");
    QTRY_COMPARE(shown(fixture), QString(kAllOpen));
    QCOMPARE(surprises(fixture), QString());
}

void AppExpansionTest::tabsOpenedOneByOneReturnAfterAFilter()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};

    // Two tabs opened by their headers; the one between them stays closed.
    fixture.tree().expand(fixture.row("KB/CF Champ"));
    fixture.tree().expand(fixture.row("Market"));
    const QString before = shown(fixture);
    QCOMPARE(before,
             QString("[-] #16, \"KB/CF Champ\"\n"
                     "      Viper Suit Loricated Ringmail\n"
                     "      Iron Will Support\n"
                     "      Hollow Vision Hypnotic Eye Jewel\n"
                     "[+] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "[-] #24, \"Market\"\n"
                     "      Dragon Crown Close Helmet | price 3 chaos"));

    // The filter shows the closed tab alone, and opens it.
    QLineEdit *minimum = fixture.numberBoxes("ilvl").first();
    retype(minimum, "75");
    QTRY_COMPARE(shown(fixture), QString(kFrom75));

    // The filter removed: the two tabs are open again, and the tab the
    // filter opened is closed as the user left it.
    retype(minimum, "");
    QTRY_COMPARE(shown(fixture), before);
    QCOMPARE(surprises(fixture), QString());
}

void AppExpansionTest::changesUnderAFilterStayWithTheFilter()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};

    // Before the filter: #16 closed, #22 and #24 open.
    fixture.tree().expand(fixture.row("~price 2 chaos"));
    fixture.tree().expand(fixture.row("Market"));
    const QString before = shown(fixture);
    QCOMPARE(before,
             QString("[+] #16, \"KB/CF Champ\"\n"
                     "[-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "      Sol Buckle Chain Belt | price 2 chaos\n"
                     "      Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                     "      Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                     "[-] #24, \"Market\"\n"
                     "      Dragon Crown Close Helmet | price 3 chaos"));

    // Item level 71 and up: two tabs have a match, and the filter opens both.
    QLineEdit *minimum = fixture.numberBoxes("ilvl").first();
    retype(minimum, "71");
    QTRY_COMPARE(shown(fixture),
                 QString("[-] #16, \"KB/CF Champ\"\n"
                         "      Hollow Vision Hypnotic Eye Jewel\n"
                         "[-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                         "      Sol Buckle Chain Belt | price 2 chaos\n"
                         "      Hypnotic Finger Moonstone Ring | price 2 chaos"));

    // Under the filter the user closes the tab that was open before it, and
    // closes and opens again the one that was closed before it.
    fixture.tree().collapse(fixture.row("~price 2 chaos"));
    fixture.tree().collapse(fixture.row("KB/CF Champ"));
    fixture.tree().expand(fixture.row("KB/CF Champ"));
    QCOMPARE(shown(fixture),
             QString("[-] #16, \"KB/CF Champ\"\n"
                     "      Hollow Vision Hypnotic Eye Jewel\n"
                     "[+] #22, \"~price 2 chaos\" [price 2 chaos]"));

    // The filter removed: the view is as it was before the filter. Neither
    // change made under the filter came along.
    retype(minimum, "");
    QTRY_COMPARE(shown(fixture), before);
    QCOMPARE(surprises(fixture), QString());
}

void AppExpansionTest::openedTabsReturnAfterByItem()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};

    fixture.tree().expand(fixture.row("Market"));
    const QString before = shown(fixture);
    QCOMPARE(before,
             QString("[+] #16, \"KB/CF Champ\"\n"
                     "[+] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "[-] #24, \"Market\"\n"
                     "      Dragon Crown Close Helmet | price 3 chaos"));

    // By Item and back, as picked from the view selector.
    auto *selector = fixture.window().findChild<QComboBox *>("viewComboBox");
    QVERIFY(selector);
    const auto pick = [selector](const QString &mode) {
        const int index = selector->findText(mode);
        selector->setCurrentIndex(index);
        emit selector->activated(index);
    };
    pick("By Item");
    QVERIFY(shown(fixture) != before);
    pick("By Tab");
    QCOMPARE(shown(fixture), before);
    QCOMPARE(surprises(fixture), QString());
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppExpansionTest)
#include "tst_appexpansion.moc"
