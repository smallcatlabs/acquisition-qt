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
    void resolvesToTheCopyWhereItWasShown();
    void noticeStatesEveryReason();
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
    // The item without its own price shows the tab's.
    QCOMPARE(f.buyouts.manager->Effective(*other).buyout.value, 7.0);
    QVERIFY(f.buyouts.manager->Effective(*other).buyout.IsInherited());
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
    QCOMPARE(f.buyouts.manager->Effective(*inside).buyout.value, 2.0);
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
    QVERIFY(
        ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 2, .locked = 0, .not_saved = 0, .missing = 0},
                          true)
            .isEmpty());
    // Rows priced in the game are left alone, and the user is told why.
    const QString locked = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 2,
                                                               .locked = 1,
                                                               .not_saved = 0,
                                                               .missing = 0},
                                             true);
    QVERIFY2(locked.contains("in the game"), qPrintable(locked));
    const QString rejected = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 0,
                                                                 .locked = 0,
                                                                 .not_saved = 1,
                                                                 .missing = 0},
                                               true);
    QVERIFY(rejected.contains("log file"));
    const QString unloaded = ApplyBuyoutNotice(ApplyBuyoutResult{.applied = 0,
                                                                 .locked = 0,
                                                                 .not_saved = 1,
                                                                 .missing = 0},
                                               false);
    QVERIFY(unloaded.contains("cannot be saved in this session"));
}

// The same rows in another order are the same targets; an item is
// compared by id (a refresh's replacement is the same target) and a tab by
// its type and id, so a renamed tab is the same target.
void ApplyBuyoutTest::targetsCompareAsSets()
{
    const ItemLocation tab = makeTestStashLocation("stash-x", "Before", 0);
    const auto a = makeItem("a", tab);
    const auto b = makeItem("b", tab);
    PriceTargets first;
    first.rows = {PriceTarget::ForItem(a), PriceTarget::ForItem(b), PriceTarget::ForLocation(tab)};
    PriceTargets second;
    second.rows = {PriceTarget::ForLocation(makeTestStashLocation("stash-x", "After", 0)),
                   PriceTarget::ForItem(makeItem("b", tab)),
                   PriceTarget::ForItem(a)};
    QVERIFY(first == second);

    second.rows.pop_back();
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
    const auto gone = makeItem("item-gone", tab);
    f.items.OnItemsRefreshed({kept, gone}, {tab, gone_tab}, false);
    const auto replacement = makeItem("item-kept", tab);
    f.items.OnItemsRefreshed({replacement}, {tab}, false);

    PriceTargets targets;
    targets.rows = {PriceTarget::ForItem(kept),
                    PriceTarget::ForItem(gone),
                    PriceTarget::ForLocation(tab),
                    PriceTarget::ForLocation(gone_tab)};
    const ResolvedTargets resolved = ResolvePriceTargets(targets,
                                                         f.items.items(),
                                                         f.items.locationInventory());

    QCOMPARE(resolved.missing, 2);
    QCOMPARE(resolved.targets.items.size(), size_t(1));
    QCOMPARE(resolved.targets.items.front(), replacement.get());
    QCOMPARE(resolved.targets.tabs.size(), size_t(1));
    QCOMPARE(resolved.targets.tabs.front().id(), tab.id());
}

// Mid-refresh an id can sit in two tabs. The target resolves to the copy in
// the tab it was shown in, so the lock check sees that copy's tab: shown in
// an ordinary tab, it is not refused because another copy sits in a tab
// priced in the game.
void ApplyBuyoutTest::resolvesToTheCopyWhereItWasShown()
{
    Fixture f;
    const ItemLocation priced = makeTestStashLocation("stash-priced", "~price 2 chaos", 0);
    const ItemLocation plain = makeTestStashLocation("stash-plain", "Dump", 1);
    const auto in_priced = makeItem("item-twice", priced);
    const auto in_plain = makeItem("item-twice", plain);
    f.items.OnItemsRefreshed({in_priced, in_plain}, {priced, plain}, false);

    PriceTargets shown_plain;
    shown_plain.rows = {PriceTarget::ForItem(in_plain)};
    const ResolvedTargets resolved = ResolvePriceTargets(shown_plain,
                                                         f.items.items(),
                                                         f.items.locationInventory());
    QCOMPARE(resolved.targets.items.size(), size_t(1));
    QCOMPARE(resolved.targets.items.front(), in_plain.get());

    PriceTargets shown_priced;
    shown_priced.rows = {PriceTarget::ForItem(in_priced)};
    QCOMPARE(ResolvePriceTargets(shown_priced, f.items.items(), f.items.locationInventory())
                 .targets.items.front(),
             in_priced.get());
}

// Every reason that applies is stated, with its count, not just the first.
void ApplyBuyoutTest::noticeStatesEveryReason()
{
    ApplyBuyoutResult result;
    result.applied = 1;
    result.locked = 2;
    result.not_saved = 3;
    result.missing = 4;
    const QString notice = ApplyBuyoutNotice(result, true);
    QVERIFY2(notice.contains("saved for 1 row"), qPrintable(notice));
    QVERIFY2(notice.contains("2 row(s) are priced in the game"), qPrintable(notice));
    QVERIFY2(notice.contains("not saved for 3 row"), qPrintable(notice));
    QVERIFY2(notice.contains("4 row(s)"), qPrintable(notice));
}

QTEST_GUILESS_MAIN(ApplyBuyoutTest)

#include "tst_applybuyout.moc"
