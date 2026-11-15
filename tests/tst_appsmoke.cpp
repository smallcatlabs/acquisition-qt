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
    QCOMPARE(fixture.apiGets(), QStringList());
    QCOMPARE(fixture.network->unanswered(), QStringList());
    QCOMPARE(fixture.dialogs, QStringList());
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
    QCOMPARE(fixture.apiGets(),
             QStringList({
                 "/stash/Hardcore",
                 "/stash/Hardcore/" + fixture.tabs[0].id,
                 "/stash/Hardcore/" + fixture.tabs[1].id,
                 "/stash/Hardcore/" + fixture.tabs[2].id,
             }));
    // The item is gone and nothing else changed: the selection, the current
    // row and the collapsed tabs are as they were.
    QCOMPARE(changed(before, fixture), QString("-         Sol Buckle Chain Belt | price 2 chaos"));
    QCOMPARE(fixture.network->unanswered(), QStringList());
    QCOMPARE(fixture.dialogs, QStringList());
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
    QCOMPARE(fixture.apiGets(),
             QStringList({"/stash/Hardcore", "/stash/Hardcore/" + fixture.tabs[0].id}));
    QCOMPARE(changed(before, fixture), QString("-         Hollow Vision Hypnotic Eye Jewel"));
    QCOMPARE(fixture.network->unanswered(), QStringList());
    QCOMPARE(fixture.dialogs, QStringList());
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppSmokeTest)
#include "tst_appsmoke.moc"
