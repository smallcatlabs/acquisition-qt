// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// The release smoke list as scenario tests (#377; the "first sitting" steps
// on #320), on the real Application (appfixture.h). One scenario per process.

#include <QtTest/QtTest>

#include <QAction>
#include <QClipboard>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QUrlQuery>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "appfixture.h"
#include "shop.h"
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
    // Step 9: the forum shop, against the captured forum pages. Whether the
    // real forum accepts the post stays a check by hand.
    void manualShopUpdatePostsTheUsersPrices();
    void automaticShopUpdateWaitsForACleanRefresh();

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
    // The application's own switch for a slow load: 200 ms for each cached
    // tab, so there is a while in which to be usable.
    qputenv("ACQ_PARSE_DELAY_MS", "200");
    fixture.logIn();
    qunsetenv("ACQ_PARSE_DELAY_MS");
    ScreenshotOnFailure screenshot{fixture.window()};
    QVERIFY(fixture.window().isVisible());
    QVERIFY(fixture.window().isEnabled());

    // Usable while the items load: the event loop keeps turning with the
    // tree still empty. A load that held the main thread would let the timer
    // fire only once the items were already there.
    int turns_while_empty = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, &heartbeat, [&] {
        if (fixture.tree().model()->rowCount() == 0) {
            ++turns_while_empty;
        }
    });
    heartbeat.start(10);
    QTRY_COMPARE(fixture.tree().model()->rowCount(), 3);
    heartbeat.stop();
    QVERIFY2(turns_while_empty >= 10, qPrintable(QString::number(turns_while_empty)));
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

    // Starting from the cache asks the API for nothing, not even a probe:
    // waited for, since a request would leave only after the limiter's
    // spacing between sends.
    QTest::qWait(1000);
    QCOMPARE(fixture.apiRequests().join(' '), QString());
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
    // "Refresh Selected" in the tree's context menu.
    const auto outcome = fixture.refresh([&] { fixture.menuAction("Refresh Selected").trigger(); });
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
    const auto refresh_selected = [&] { fixture.menuAction("Refresh Selected").trigger(); };

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
    // (Item levels are whole, so this shows the comma is taken, not what the
    // fraction is worth; tst_filters pins the value.)
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

namespace {

    // Not a real session id: the shape of one, 32 hexadecimal digits.
    constexpr const char *kTestSessionId = "00112233445566778899aabbccddeeff";

    // The scrubbed security token in tests/fixtures/forum/edit-thread.html.
    constexpr const char *kEditPageHash
        = "0123456789abcdef0123456789abcdef-fedcba9876543210fedcba9876543210";

    // The shop with the scene's Viper Suit at b/o 7 chaos and nothing else:
    // the website lists the item's tab as number 15, and it lies at 5, 2.
    // The spaces in the price and the one closing tag for two spoilers are
    // what the application posts today (#359).
    constexpr const char *kShopWithViperSuit
        = R"([spoiler][spoiler=" ~b/o  7 chaos"])"
          R"([linkItem location="Stash16" league="Hardcore" x="5" y="2" realm="pc"])"
          R"([/spoiler])";

    // What a shop scenario must end without: a request the script did not
    // answer, or a dialog other than the ones it expected.
    QString surprisesBeyond(const AppFixture &fixture, qsizetype expected_dialogs)
    {
        QStringList found;
        for (const QString &url : fixture.network->unanswered()) {
            found << "unanswered request: " + url;
        }
        for (const QString &dialog : fixture.dialogs.mid(expected_dialogs)) {
            found << "dialog: " + dialog;
        }
        return found.join('\n');
    }

    // The session id the network would send to the website.
    QString sessionCookie(const AppFixture &fixture)
    {
        const QUrl forum(AppFixture::forumEditUrl());
        for (const QNetworkCookie &cookie : fixture.network->cookieJar()->cookiesForUrl(forum)) {
            if (cookie.name() == "POESESSID") {
                return QString::fromUtf8(cookie.value());
            }
        }
        return "(no POESESSID cookie)";
    }

} // namespace

void AppSmokeTest::manualShopUpdatePostsTheUsersPrices()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    fixture.serveForum();

    // One price of the user's own, beside a tab priced by its name and an
    // item priced by a note: four items the game priced. The gem sits in
    // the Viper Suit and has no price.
    fixture.select("Viper Suit");
    fixture.typePrice("7");
    QCOMPARE(text(fixture),
             QString("  [-] #16, \"KB/CF Champ\"\n"
                     "*>      Viper Suit Loricated Ringmail | b/o 7 chaos\n"
                     "        Iron Will Support\n"
                     "        Hollow Vision Hypnotic Eye Jewel\n"
                     "  [-] #22, \"~price 2 chaos\" [price 2 chaos]\n"
                     "        Sol Buckle Chain Belt | price 2 chaos\n"
                     "        Hypnotic Finger Moonstone Ring | price 2 chaos\n"
                     "        Grim Arbiter Searching Eye Jewel | price 2 chaos\n"
                     "  [-] #24, \"Market\"\n"
                     "        Dragon Crown Close Helmet | price 3 chaos"));

    // With no thread and no session id set, an update says what is missing
    // and asks the website for nothing.
    QAction &update = fixture.menuAction("Update forum shop(s)");
    update.trigger();
    QTRY_COMPARE(fixture.dialogs.size(), 1);
    QVERIFY2(fixture.dialogs.last().contains("No forum threads have been set"),
             qPrintable(fixture.dialogs.last()));

    QVERIFY(fixture.answerMenuDialog("Forum shop thread...", AppFixture::kForumThread));
    QCOMPARE(fixture.window().findChild<QAction *>("actionSetShopThreads")->text(),
             QString("Forum shop thread... [3445874]"));
    update.trigger();
    QTRY_COMPARE(fixture.dialogs.size(), 2);
    QVERIFY2(fixture.dialogs.last().contains("POESESSID has not been set"),
             qPrintable(fixture.dialogs.last()));
    QCOMPARE(fixture.websiteRequests(), QStringList());

    QCOMPARE(sessionCookie(fixture), QString("(no POESESSID cookie)"));
    QVERIFY(fixture.answerMenuDialog("Update shop POESESSID", kTestSessionId));
    QCOMPARE(sessionCookie(fixture), QString(kTestSessionId));

    // The update: the website's list of tabs, the thread's edit page, and
    // half a second later the post.
    update.trigger();
    QVERIFY2(QTest::qWaitFor([&] { return fixture.statusText().contains("Shop threads updated"); },
                             15000),
             qPrintable(fixture.statusText() + '\n' + fixture.websiteRequests().join('\n')));
    QCOMPARE(fixture.websiteRequests(),
             QStringList({
                 "HEAD /character-window/get-stash-items",
                 "GET /character-window/get-stash-items",
                 "GET /forum/edit-thread/3445874",
                 "POST /forum/edit-thread/3445874",
             }));

    // The list of tabs is asked for by account, realm and league.
    const QString index_url
        = fixture.network->requested(QNetworkAccessManager::GetOperation, "get-stash-items").first();
    QCOMPARE(QUrlQuery(QUrl(index_url)).queryItemValue("accountName", QUrl::FullyDecoded),
             QString(AppFixture::kAccount));
    QCOMPARE(QUrlQuery(QUrl(index_url)).queryItemValue("realm"), QString(AppFixture::kRealm));
    QCOMPARE(QUrlQuery(QUrl(index_url)).queryItemValue("league"), QString(AppFixture::kLeague));

    // The form: the title and the security token of the edit page, and the
    // shop. Nothing in it is left unencoded.
    QCOMPARE(fixture.forumPosts().size(), 1);
    const QByteArray form = fixture.forumPosts().first();
    QCOMPARE(AppFixture::formField(form, "title"), QString("GERWARIC's Standard Shop"));
    QCOMPARE(AppFixture::formField(form, "hash"), QString(kEditPageHash));
    QCOMPARE(AppFixture::formField(form, "notify_owner"), QString("0"));
    QCOMPARE(AppFixture::formField(form, "submit"), QString("Submit"));
    for (const char unencoded : {'[', ']', '"', ' ', '+', '\''}) {
        QVERIFY2(!form.contains(unencoded), form.constData());
    }
    QVERIFY2(form.contains("content=%5Bspoiler%5D"), form.constData());

    // The shop lists the item the user priced and none the game priced.
    const QString content = AppFixture::formField(form, "content");
    QCOMPARE(content, QString(kShopWithViperSuit));

    // "Copy shop data to clipboard" gives the same text.
    QApplication::clipboard()->clear();
    fixture.menuAction("Copy shop data to clipboard").trigger();
    QCOMPARE(QApplication::clipboard()->text(), content);

    QCOMPARE(surprisesBeyond(fixture, 2), QString());
}

void AppSmokeTest::automaticShopUpdateWaitsForACleanRefresh()
{
    AppFixture fixture;
    QVERIFY(fixture.logInAndLoad());
    ScreenshotOnFailure screenshot{fixture.window()};
    fixture.tree().expandAll();
    fixture.serveForum();
    QVERIFY(fixture.answerMenuDialog("Forum shop thread...", AppFixture::kForumThread));
    QVERIFY(fixture.answerMenuDialog("Update shop POESESSID", kTestSessionId));
    fixture.select("Viper Suit");
    fixture.typePrice("7");

    // Turning automatic updates on posts nothing by itself.
    QAction &automatic = fixture.menuAction("Automatically update shop");
    QVERIFY(!automatic.isChecked());
    automatic.trigger();
    QVERIFY(automatic.isChecked());
    QVERIFY(fixture.access().shop().auto_update());

    auto *refresh_checked = fixture.window().findChild<QAction *>("actionRefreshCheckedTabs");
    QVERIFY(refresh_checked);

    // A refresh in which one tab's reply could not be read completes with
    // that tab skipped, and the shop is not posted: an automatic update
    // would ask the website for its list of tabs at once.
    fixture.serveStash(fixture.tabs[1].id, "\"not a stash\"");
    const auto with_a_skip = fixture.refresh([&] { refresh_checked->trigger(); });
    QVERIFY(with_a_skip.has_value());
    const auto *completed = std::get_if<CompletedRefresh>(&*with_a_skip);
    QVERIFY(completed);
    QCOMPARE(completed->skipped.size(), size_t(1));
    QTest::qWait(1000);
    QCOMPARE(fixture.websiteRequests(), QStringList());

    // A clean refresh: the shop is posted, with the user's price in it.
    fixture.serveStash(fixture.tabs[1].id, fixture.tabs[1].reply);
    const auto clean = fixture.refresh([&] { refresh_checked->trigger(); });
    QVERIFY(AppFixture::isClean(clean));
    QVERIFY2(QTest::qWaitFor([&] { return fixture.statusText().contains("Shop threads updated"); },
                             15000),
             qPrintable(fixture.statusText() + '\n' + fixture.websiteRequests().join('\n')));
    QCOMPARE(fixture.forumPosts().size(), 1);
    QCOMPARE(AppFixture::formField(fixture.forumPosts().first(), "content"),
             QString(kShopWithViperSuit));

    // Another clean refresh with nothing changed: the website's list of
    // tabs is read again to see that, and nothing is posted.
    const qsizetype asked_before = fixture.websiteRequests().size();
    const auto unchanged = fixture.refresh([&] { refresh_checked->trigger(); });
    QVERIFY(AppFixture::isClean(unchanged));
    QVERIFY(QTest::qWaitFor([&] { return fixture.websiteRequests().size() > asked_before; }, 15000));
    QTest::qWait(1500);
    QCOMPARE(fixture.websiteRequests().mid(asked_before),
             QStringList({"GET /character-window/get-stash-items"}));
    QCOMPARE(fixture.forumPosts().size(), 1);

    QCOMPARE(surprisesBeyond(fixture, 0), QString());
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppSmokeTest)
#include "tst_appsmoke.moc"
