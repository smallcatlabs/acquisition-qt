// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QSettings>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QtTest/QtTest>

#include "applybuyout.h"
#include "itemsmanager.h"
#include "testfixtures.h"

// The price-edit command without a window (#298): the selection policy is
// the caller's; what is locked, what is saved and how the edit reaches the
// model are pinned here.
class ApplyBuyoutTest : public QObject
{
    Q_OBJECT

private slots:
    void appliesToTabsAndItems();
    void leavesGamePricedTargetsAlone();
    void countsWhatWasNotSaved();
    void isOneBatch();
    void noticeNamesTheCause();
    void targetsCompareAsSets();
    void resolvesTargetsThatAreStillThere();
};

namespace {

    struct Fixture
    {
        Fixture()
            : settings(buyouts.tempDir.filePath("settings.ini"), QSettings::IniFormat)
            , items(settings, *buyouts.manager)
        {}

        BuyoutManagerFixture buyouts;
        QSettings settings;
        ItemsManager items;
    };

    // A stash item as the API sends it; "note" only when the player wrote one.
    std::shared_ptr<Item> makeItem(const QString &id,
                                   const ItemLocation &location,
                                   const QString &note = QString())
    {
        const QString note_field = note.isEmpty() ? QString()
                                                  : QString(R"("note": "%1",)").arg(note);
        const QByteArray json = QString(R"({
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "%1",
            "name": "",
            "typeLine": "Iron Ring",
            "baseType": "Iron Ring",
            "identified": true,
            "ilvl": 1,
            %2
            "x": 0,
            "y": 0
        })")
                                    .arg(id, note_field)
                                    .toUtf8();
        return std::make_shared<Item>(makeTestItem(json.constData(), location));
    }

} // namespace

void ApplyBuyoutTest::appliesToTabsAndItems()
{
    Fixture f;
    const ItemLocation tab = makeTestStashLocation("stash-a", "Dump", 0);
    const auto own = makeItem("item-own", tab);
    const auto other = makeItem("item-other", tab);
    f.items.OnItemsRefreshed({own, other}, {tab}, false);

    BuyoutTargets targets;
    targets.tabs.push_back(tab);
    targets.items.push_back(own.get());
    const ApplyBuyoutResult result = ApplyBuyout(*f.buyouts.manager,
                                                 f.items,
                                                 targets,
                                                 makeChaosBuyout(7.0));

    QCOMPARE(result.applied, 2);
    QCOMPARE(result.locked, 0);
    QCOMPARE(result.not_saved, 0);
    QCOMPARE(f.buyouts.manager->GetTab(tab).value, 7.0);
    QCOMPARE(f.buyouts.manager->Get(*own).value, 7.0);
    QVERIFY(!f.buyouts.manager->Get(*own).IsInherited());
    // The tab price reaches the item that inherits it in the same command.
    QCOMPARE(f.buyouts.manager->Get(*other).value, 7.0);
    QVERIFY(f.buyouts.manager->Get(*other).IsInherited());
}

void ApplyBuyoutTest::leavesGamePricedTargetsAlone()
{
    Fixture f;
    const ItemLocation dump = makeTestStashLocation("stash-dump", "Dump", 0);
    const ItemLocation priced = makeTestStashLocation("stash-priced", "~price 2 chaos", 1);
    const auto noted = makeItem("item-noted", dump, "~b/o 5 chaos");
    const auto inside = makeItem("item-inside", priced);
    f.items.OnItemsRefreshed({noted, inside}, {dump, priced}, false);

    BuyoutTargets targets;
    targets.tabs.push_back(priced);
    targets.items = {noted.get(), inside.get()};
    const ApplyBuyoutResult result = ApplyBuyout(*f.buyouts.manager,
                                                 f.items,
                                                 targets,
                                                 makeChaosBuyout(7.0));

    QCOMPARE(result.applied, 0);
    QCOMPARE(result.locked, 3);
    QCOMPARE(result.not_saved, 0);
    QCOMPARE(f.buyouts.manager->Get(*noted).value, 5.0);
    QCOMPARE(f.buyouts.manager->GetTab(priced).value, 2.0);
    QCOMPARE(f.buyouts.manager->Get(*inside).value, 2.0);
}

void ApplyBuyoutTest::countsWhatWasNotSaved()
{
    Fixture f;
    const ItemLocation tab = makeTestStashLocation("stash-b", "Dump", 0);
    const auto item = makeItem("item-b", tab);
    f.items.OnItemsRefreshed({item}, {tab}, false);

    QSqlQuery q(*f.buyouts.db);
    QVERIFY(q.exec("CREATE TRIGGER block_item_insert BEFORE INSERT ON item_buyouts "
                   "BEGIN SELECT RAISE(FAIL, 'test: insert blocked'); END"));
    BuyoutTargets targets;
    targets.tabs.push_back(tab);
    targets.items.push_back(item.get());
    const ApplyBuyoutResult result = ApplyBuyout(*f.buyouts.manager,
                                                 f.items,
                                                 targets,
                                                 makeChaosBuyout(7.0));

    QCOMPARE(result.applied, 1);
    QCOMPARE(result.not_saved, 1);
    QVERIFY(f.buyouts.manager->Get(*item).IsInherited());
}

// One command is one model update (M3 R2-5), the propagation pass included.
void ApplyBuyoutTest::isOneBatch()
{
    Fixture f;
    const ItemLocation tab = makeTestStashLocation("stash-c", "Dump", 0);
    const auto first = makeItem("item-c1", tab);
    const auto second = makeItem("item-c2", tab);
    f.items.OnItemsRefreshed({first, second}, {tab}, false);

    QSignalSpy changes(f.buyouts.manager.get(), &BuyoutManager::BuyoutsChanged);
    BuyoutTargets targets;
    targets.tabs.push_back(tab);
    targets.items.push_back(first.get());
    ApplyBuyout(*f.buyouts.manager, f.items, targets, makeChaosBuyout(7.0));

    QCOMPARE(changes.count(), 1);
}

void ApplyBuyoutTest::noticeNamesTheCause()
{
    // Rows the price was typed for that went away are counted.
    ApplyBuyoutResult gone;
    gone.applied = 1;
    gone.missing = 2;
    const QString missing = ApplyBuyoutNotice(gone, true);
    QVERIFY2(missing.contains("2 row(s)") && missing.contains("no longer there"),
             qPrintable(missing));
    QVERIFY(ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 2, .locked = 0, .not_saved = 0}, true)
                .isEmpty());
    // Rows priced in the game are left alone, and the user is told why.
    const QString locked
        = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 2, .locked = 1, .not_saved = 0}, true);
    QVERIFY2(locked.contains("in the game"), qPrintable(locked));
    const QString rejected
        = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 0, .locked = 0, .not_saved = 1}, true);
    QVERIFY(rejected.contains("log file"));
    const QString unloaded
        = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 0, .locked = 0, .not_saved = 1}, false);
    QVERIFY(unloaded.contains("cannot be saved in this session"));
}

// The same rows in another order are the same targets; a tab is compared
// by its type and id, so a renamed tab is the same target.
void ApplyBuyoutTest::targetsCompareAsSets()
{
    PriceTargets first;
    first.item_ids = {"a", "b"};
    first.tabs = {makeTestStashLocation("stash-x", "Before", 0)};
    PriceTargets second;
    second.item_ids = {"b", "a"};
    second.tabs = {makeTestStashLocation("stash-x", "After", 0)};
    QVERIFY(first == second);

    second.item_ids = {"a"};
    QVERIFY(!(first == second));
    QVERIFY(PriceTargets() == PriceTargets());
}

// An item a refresh replaced with the same id resolves to the new object;
// an item or tab that is gone is counted, not resolved.
void ApplyBuyoutTest::resolvesTargetsThatAreStillThere()
{
    Fixture f;
    const ItemLocation tab = makeTestStashLocation("stash-r", "Dump", 0);
    const ItemLocation gone_tab = makeTestStashLocation("stash-gone", "Gone", 1);
    const auto kept = makeItem("item-kept", tab);
    f.items.OnItemsRefreshed({kept, makeItem("item-gone", tab)}, {tab, gone_tab}, false);
    const auto replacement = makeItem("item-kept", tab);
    f.items.OnItemsRefreshed({replacement}, {tab}, false);

    PriceTargets targets;
    targets.item_ids = {"item-kept", "item-gone"};
    targets.tabs = {tab, gone_tab};
    const ResolvedTargets resolved = ResolvePriceTargets(targets,
                                                         f.items.items(),
                                                         f.items.locationInventory());

    QCOMPARE(resolved.missing, 2);
    QCOMPARE(resolved.targets.items.size(), size_t(1));
    QCOMPARE(resolved.targets.items.front(), replacement.get());
    QCOMPARE(resolved.targets.tabs.size(), size_t(1));
    QCOMPARE(resolved.targets.tabs.front().id(), tab.id());
}

QTEST_GUILESS_MAIN(ApplyBuyoutTest)

#include "tst_applybuyout.moc"
