// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// The release smoke list as scenario tests (#377; the "first sitting" steps
// on #320), on the real Application (appfixture.h). One scenario per process.

#include <QtTest/QtTest>

#include <QAction>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "appfixture.h"
#include "testsuite.h"
#include "viewtranscript.h"

class AppSmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Step 2: start with cached items; the window is usable while they load.
    void startsWithCachedItems();
    // Step 3: a refresh keeps selection, the current row and collapsed tabs.
    void refreshCheckedKeepsTheView();
    void refreshSelectedKeepsTheView();
    // Step 4: the price editor saves on Enter, not per keystroke.
    void typedPriceIsSavedOnEnter();
    // Steps 5 and 6: a price set in the game is the one in force, and the
    // user's own price returns when it goes.
    void notePriceOverridesAndGivesWay();
    void tabNamePriceOverridesAndGivesWay();
    // Step 7: the Priced filter follows the prices.
    void pricedFilterFollowsPrices();
    // Step 8: number fields take a decimal comma and refuse a percent sign.
    void numberFieldsTakeACommaAndRefuseAPercent();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> m_sink_hub;
};

namespace {

    // Name and Price: the two columns the smoke list is about.
    QStringList shown(const AppFixture &fixture)
    {
        return viewTranscript(fixture.tree(), 2);
    }

    // What the tree shows now against `before`, one changed line per line,
    // as one text: a failure then prints every line that differs.
    QString changed(const QStringList &before, const AppFixture &fixture)
    {
        return viewTranscriptChanges(before, shown(fixture), {}).join('\n');
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

    // What the tree shows, as one text.
    QString text(const AppFixture &fixture)
    {
        return shown(fixture).join('\n');
    }

    // The line of the row named `name`, without its marks and indentation.
    QString line(const AppFixture &fixture, const QString &name)
    {
        for (const QString &row : shown(fixture)) {
            if (row.contains(name)) {
                return row.mid(2).trimmed();
            }
        }
        return "(no row named " + name + ")";
    }

    // The reply for a tab with `note` set on the item named `name`, as the
    // game sets it when the item is priced there.
    QByteArray withNote(const AppFixture::Tab &tab, const QString &name, const QString &note)
    {
        QJsonObject stash = QJsonDocument::fromJson(tab.reply).object();
        QJsonArray items = stash.value("items").toArray();
        bool found = false;
        for (qsizetype i = 0; i < items.size(); ++i) {
            QJsonObject item = items.at(i).toObject();
            if (item.value("name").toString() == name) {
                item.insert("note", note);
                items.replace(i, item);
                found = true;
            }
        }
        if (!found) {
            qFatal("withNote: tab %s has no item named %s", qPrintable(tab.name), qPrintable(name));
        }
        stash.insert("items", items);
        return QJsonDocument(stash).toJson(QJsonDocument::Compact);
    }

    // The reply for a tab with the item named `name` taken out of it.
    QByteArray without(const AppFixture::Tab &tab, const QString &name)
    {
        QJsonObject stash = QJsonDocument::fromJson(tab.reply).object();
        QJsonArray kept;
        for (const QJsonValue &item : stash.value("items").toArray()) {
            if (item.toObject().value("name").toString() != name) {
                kept.append(item);
            }
        }
        if (kept.size() + 1 != stash.value("items").toArray().size()) {
            qFatal("without: tab %s has no item named %s", qPrintable(tab.name), qPrintable(name));
        }
        stash.insert("items", kept);
        return QJsonDocument(stash).toJson(QJsonDocument::Compact);
    }

} // namespace

void AppSmokeTest::initTestCase()
{
    // The log panel attaches its sink through the hub logging::init installs
    // on the main logger (F42), so the test's logger needs one too.
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    m_main_logger->sinks().push_back(m_sink_hub);
    spdlog::register_logger(m_main_logger);
}

void AppSmokeTest::cleanupTestCase()
{
    spdlog::drop("main");
}

void AppSmokeTest::startsWithCachedItems()
{
    AppFixture fixture;
    QVERIFY(QTest::qWaitFor([&] { return fixture.access().gameDataLoaded(); }, 10000));
    fixture.logIn();
    ScreenshotOnFailure screenshot{fixture.window()};

    // The window is up and usable before any cached item has arrived.
    QVERIFY(fixture.window().isVisible());
    QVERIFY(fixture.window().isEnabled());
    QCOMPARE(fixture.tree().model()->rowCount(), 0);

    QTRY_COMPARE(fixture.tree().model()->rowCount(), 3);
    fixture.tree().expandAll();
    fixture.select("KB/CF Champ");
    saveTestScreenshot(fixture.window(), "loaded");
    // The tabs and items of the last run, a socketed gem as its own row; the
    // tab priced by its name prices its items, and a note prices its item.
    QCOMPARE(shown(fixture),
             QStringList({
                 "*>[-] #16, \"KB/CF Champ\"",
                 "        Viper Suit Loricated Ringmail",
                 "        Iron Will Support",
                 "        Hollow Vision Hypnotic Eye Jewel",
                 "  [-] #22, \"~price 2 chaos\" [price 2 chaos]",
                 "        Sol Buckle Chain Belt | price 2 chaos",
                 "        Hypnotic Finger Moonstone Ring | price 2 chaos",
                 "        Grim Arbiter Searching Eye Jewel | price 2 chaos",
                 "  [-] #24, \"Market\"",
                 "        Dragon Crown Close Helmet | price 3 chaos",
             }));

    // Starting from the cache asks the API for nothing.
    QCOMPARE(fixture.apiGets().join(' '), QString());
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::refreshCheckedKeepsTheView()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};

    // One tab open with an item selected; the other two stay collapsed.
    fixture.tree().expand(fixture.row("~price 2 chaos"));
    fixture.select("Hypnotic Finger");
    const QStringList before = shown(fixture);
    QCOMPARE(before,
             QStringList({
                 "  [+] #16, \"KB/CF Champ\"",
                 "  [-] #22, \"~price 2 chaos\" [price 2 chaos]",
                 "        Sol Buckle Chain Belt | price 2 chaos",
                 "*>      Hypnotic Finger Moonstone Ring | price 2 chaos",
                 "        Grim Arbiter Searching Eye Jewel | price 2 chaos",
                 "  [+] #24, \"Market\"",
             }));

    // Since the last run an item has left the open tab.
    fixture.serveStash(fixture.tabs[1].id, without(fixture.tabs[1], "Sol Buckle"));
    auto *refresh_checked = fixture.window().findChild<QAction *>("actionRefreshCheckedTabs");
    QVERIFY(refresh_checked);
    const auto outcome = fixture.refresh([&] { refresh_checked->trigger(); });
    QVERIFY(AppFixture::isClean(outcome));
    saveTestScreenshot(fixture.window(), "refreshed");

    // Every tab is checked, so every tab was fetched. (No character is
    // known, and a checked refresh does not go looking for one.)
    QCOMPARE(fixture.apiGets().join(' '),
             QString("/stash/Hardcore /stash/Hardcore/%1 /stash/Hardcore/%2 /stash/Hardcore/%3")
                 .arg(fixture.tabs[0].id, fixture.tabs[1].id, fixture.tabs[2].id));
    // The item is gone and nothing else changed: the selection, the current
    // row and the collapsed tabs are as they were.
    QCOMPARE(changed(before, fixture), QString("-         Sol Buckle Chain Belt | price 2 chaos"));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::refreshSelectedKeepsTheView()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};

    // Two tabs open, the first one's header selected.
    fixture.tree().expand(fixture.row("KB/CF Champ"));
    fixture.tree().expand(fixture.row("Market"));
    fixture.select("KB/CF Champ");
    const QStringList before = shown(fixture);

    // Since the last run an item has left the selected tab.
    fixture.serveStash(fixture.tabs[0].id, without(fixture.tabs[0], "Hollow Vision"));
    const auto outcome = fixture.refresh([&] {
        // "Refresh Selected" in the tree's context menu.
        QVERIFY(QMetaObject::invokeMethod(&fixture.window(), "OnRefreshSelected"));
    });
    QVERIFY(AppFixture::isClean(outcome));
    saveTestScreenshot(fixture.window(), "refreshed");

    // Only the selected tab was fetched.
    QCOMPARE(fixture.apiGets().join(' '),
             QString("/stash/Hardcore /stash/Hardcore/%1").arg(fixture.tabs[0].id));
    QCOMPARE(changed(before, fixture), QString("-         Hollow Vision Hypnotic Eye Jewel"));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::typedPriceIsSavedOnEnter()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    const QString viper = fixture.itemId(0, "Viper Suit");

    // Typing alone saves nothing.
    fixture.select("Viper Suit");
    QLineEdit &box = fixture.priceBox();
    QVERIFY(box.isEnabled());
    QTest::keyClicks(&box, "7");
    QCOMPARE(line(fixture, "Viper Suit"), QString("Viper Suit Loricated Ringmail"));
    QCOMPARE(AppFixture::priceText(fixture.storedItemPrices(), viper), QString("(none)"));

    // Enter saves it: the row shows it and it is in the store on disk.
    QTest::keyClick(&box, Qt::Key_Return);
    QCOMPARE(line(fixture, "Viper Suit"), QString("Viper Suit Loricated Ringmail | b/o 7 chaos"));
    QCOMPARE(AppFixture::priceText(fixture.storedItemPrices(), viper), QString("b/o 7 chaos"));

    // Text that is not a price changes nothing, and the status line says so.
    fixture.typePrice("abc");
    QVERIFY2(fixture.statusText().contains("not a price"), qPrintable(fixture.statusText()));
    QCOMPARE(AppFixture::priceText(fixture.storedItemPrices(), viper), QString("b/o 7 chaos"));

    // An item priced in the game is locked.
    fixture.select("Dragon Crown");
    QVERIFY(!fixture.priceBox().isEnabled());

    // A tab's price is one row in the store, not one for each of its items.
    fixture.select("KB/CF Champ");
    fixture.typePrice("3");
    saveTestScreenshot(fixture.window(), "tab-priced");
    QCOMPARE(text(fixture),
             QString("*>[-] #16, \"KB/CF Champ\" [b/o 3 chaos]\n"
                     "        Viper Suit Loricated Ringmail | b/o 7 chaos\n"
                     "        Iron Will Support | b/o 3 chaos\n"
                     "        Hollow Vision Hypnotic Eye Jewel | b/o 3 chaos\n"
                     "  [-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "        Sol Buckle Chain Belt | price 2 chaos\n"
                     "        Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                     "        Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                     "  [-] #24, \"Market\"\n"
                     "        Dragon Crown Close Helmet | price 3 chaos"));
    QCOMPARE(fixture.storedTabPrices().size(), size_t(1));
    QCOMPARE(fixture.storedItemPrices().size(), size_t(1));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::notePriceOverridesAndGivesWay()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    const QString viper = fixture.itemId(0, "Viper Suit");
    fixture.select("Viper Suit");
    fixture.typePrice("7");
    QCOMPARE(line(fixture, "Viper Suit"), QString("Viper Suit Loricated Ringmail | b/o 7 chaos"));
    const auto refresh_selected = [&] {
        QVERIFY(QMetaObject::invokeMethod(&fixture.window(), "OnRefreshSelected"));
    };

    // The item is priced in the game, in a currency that is not one of the
    // original nineteen: the note's price is the one in force.
    fixture.serveStash(fixture.tabs[0].id,
                       withNote(fixture.tabs[0], "Viper Suit", "~b/o 5 jewellers"));
    QVERIFY(AppFixture::isClean(fixture.refresh(refresh_selected)));
    saveTestScreenshot(fixture.window(), "noted");
    QCOMPARE(line(fixture, "Viper Suit"), QString("Viper Suit Loricated Ringmail | b/o 5 jew"));

    // The note goes: the user's own price is back, and was never dropped.
    fixture.serveStash(fixture.tabs[0].id, fixture.tabs[0].reply);
    QVERIFY(AppFixture::isClean(fixture.refresh(refresh_selected)));
    QCOMPARE(line(fixture, "Viper Suit"), QString("Viper Suit Loricated Ringmail | b/o 7 chaos"));
    QCOMPARE(AppFixture::priceText(fixture.storedItemPrices(), viper), QString("b/o 7 chaos"));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::tabNamePriceOverridesAndGivesWay()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    auto *refresh_checked = fixture.window().findChild<QAction *>("actionRefreshCheckedTabs");
    QVERIFY(refresh_checked);
    const auto refresh = [&] { refresh_checked->trigger(); };

    // The user prices a tab; its items take that price.
    fixture.select("KB/CF Champ");
    fixture.typePrice("3");
    QCOMPARE(line(fixture, "#16"), QString("[-] #16, \"KB/CF Champ\" [b/o 3 chaos]"));
    QCOMPARE(line(fixture, "Hollow Vision"),
             QString("Hollow Vision Hypnotic Eye Jewel | b/o 3 chaos"));

    // The tab is renamed in the game to a name that is a price: that price
    // is the one in force.
    fixture.renameTab(0, "~price 9 chaos");
    QVERIFY(AppFixture::isClean(fixture.refresh(refresh)));
    saveTestScreenshot(fixture.window(), "priced-by-name");
    QCOMPARE(line(fixture, "#16"), QString("[-] #16, \"~price 9 chaos\" [price 9 chaos]"));
    QCOMPARE(line(fixture, "Hollow Vision"),
             QString("Hollow Vision Hypnotic Eye Jewel | price 9 chaos"));

    // Renamed back, the user's price for the tab returns; it was never
    // dropped from the store.
    fixture.renameTab(0, "KB/CF Champ");
    QVERIFY(AppFixture::isClean(fixture.refresh(refresh)));
    QCOMPARE(line(fixture, "#16"), QString("[-] #16, \"KB/CF Champ\" [b/o 3 chaos]"));
    QCOMPARE(line(fixture, "Hollow Vision"),
             QString("Hollow Vision Hypnotic Eye Jewel | b/o 3 chaos"));
    QCOMPARE(AppFixture::priceText(fixture.storedTabPrices(), fixture.tabs[0].id),
             QString("b/o 3 chaos"));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::pricedFilterFollowsPrices()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    fixture.select("Viper Suit");
    fixture.typePrice("7");
    fixture.select("Hollow Vision");
    fixture.typePrice("8");

    // Only what has a price is shown.
    fixture.pricedBox().click();
    fixture.tree().expandAll();
    fixture.tree().collapse(fixture.row("Market"));
    fixture.select("Hollow Vision");
    QCOMPARE(text(fixture),
             QString("  [-] #16, \"KB/CF Champ\"\n"
                     "        Viper Suit Loricated Ringmail | b/o 7 chaos\n"
                     "*>      Hollow Vision Hypnotic Eye Jewel | b/o 8 chaos\n"
                     "  [-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "        Sol Buckle Chain Belt | price 2 chaos\n"
                     "        Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                     "        Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                     "  [+] #24, \"Market\""));

    // Its price cleared, the row leaves.
    fixture.priceBox(Buyout::BUYOUT_TYPE_INHERIT);
    QTRY_COMPARE(line(fixture, "Hollow Vision"), QString("(no row named Hollow Vision)"));

    // The tab priced, its other items join; the header stays selected and
    // the collapsed tab stays collapsed.
    fixture.select("KB/CF Champ");
    fixture.typePrice("3");
    saveTestScreenshot(fixture.window(), "tab-priced");
    QTRY_COMPARE(text(fixture),
                 QString("*>[-] #16, \"KB/CF Champ\" [b/o 3 chaos]\n"
                         "        Viper Suit Loricated Ringmail | b/o 7 chaos\n"
                         "        Iron Will Support | b/o 3 chaos\n"
                         "        Hollow Vision Hypnotic Eye Jewel | b/o 3 chaos\n"
                         "  [-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                         "        Sol Buckle Chain Belt | price 2 chaos\n"
                         "        Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                         "        Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                         "  [+] #24, \"Market\""));
    QCOMPARE(surprises(fixture), QString());
}

void AppSmokeTest::numberFieldsTakeACommaAndRefuseAPercent()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    // Every row, with every tab opened; without the selection marks, which
    // this scenario is not about.
    const auto rows = [&fixture] {
        fixture.tree().expandAll();
        QStringList lines;
        for (const QString &row : shown(fixture)) {
            lines << row.mid(2);
        }
        return lines.join('\n');
    };
    const QString everything = rows();
    QLineEdit *minimum = fixture.numberBoxes("ilvl").first();
    const auto marked = [](const QLineEdit *box) {
        return !box->toolTip().isEmpty() && !box->styleSheet().isEmpty();
    };

    // A decimal comma is a number: only the items at or above it are shown.
    QTest::keyClicks(minimum, "75,5");
    QTRY_COMPARE(rows(),
                 QString("[-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                         "      Sol Buckle Chain Belt | price 2 chaos\n"
                         "      Hypnotic Finger Moonstone Ring | price 2 chaos"));
    QVERIFY(!marked(minimum));

    // A percent sign is not: the box is marked and nothing is filtered out.
    minimum->selectAll();
    QTest::keyClicks(minimum, "5%");
    saveTestScreenshot(fixture.window(), "percent");
    QTRY_VERIFY(marked(minimum));
    QTRY_COMPARE(rows(), everything);
    QCOMPARE(surprises(fixture), QString());
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppSmokeTest)
#include "tst_appsmoke.moc"
