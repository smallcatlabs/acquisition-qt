// SPDX-License-Identifier: GPL-3.0-or-later

#include <QSettings>
#include <QSqlQuery>
#include <QtTest/QtTest>

#include "applybuyout.h"
#include "itemsmanager.h"
#include "testfixtures.h"

class BuyoutManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void stringToBuyout();
    // #293 step 2: a tab's price is not copied onto its items.
    void tabPriceWritesOneRow();
    void inheritedRowIsIgnoredOnLoad();
    void inheritedDateIsTheTabPricesDate();
    void itemBuyoutRoundTrip();
    void tabBuyoutRoundTrip();
    void itemShowsItsTabsPrice();
    void clearingItemBuyoutRemovesInMemoryValue();
    void clearingTabBuyoutRemovesInMemoryValue();
    void clearingItemBuyoutRemovesRepoRow();
    void clearingTabBuyoutRemovesRepoRow();
    void clearingAbsentBuyoutSkipsRepo();
    void clearingAbsentTabBuyoutSkipsRepo();
    void failedDeleteKeepsEntryForRetry();
    void failedSaveLeavesEntryUnchanged();

    // Items-pipeline M3 S2, review round 1: choke-point self-consistency
    // for mutations outside any caller batch — one boundary per operation,
    // emitted only after the mutation completes.
    void unbatchedTabCompressionEmitsOnceAfterErase();

    // #251: a price read from an in-game note or tab name is cleared when
    // the note or name stops yielding one, on the snapshot and delta paths.
    void removedNoteClearsGamePriceOnSnapshot();
    void removedNoteClearsGamePriceOnDelta();
    void renamedTabClearsGameTabPrice();
    void unknownCurrencyClearsGamePrice();
    void manualPriceWithEmptyNoteIsUntouched();
    void inheritedGameTabPriceIsNotRewritten();

    // #290: a snapshot no longer stamps db_version into the per-league
    // file, which the legacy importer reads.
    void snapshotWritesNoDbVersion();

    // #257: when the buyouts did not load, the manager writes nothing —
    // a refresh must not overwrite a price it could not read.
    void failedItemReadKeepsManualRowThroughRefresh();

    // #148: a tab whose own price requires a refresh is locked from the tab
    // list, whether or not any of its items are loaded.
    void gamePricedEmptyTabIsLocked();
    void manuallyPricedEmptyTabIsLocked();
    void removeOnlyPricedTabStaysUnlocked();
    void priceNamedFolderStaysUnlocked();
};

namespace {

    // An ItemsManager over the buyout fixture, fed snapshots and deltas the
    // way the worker publishes them.
    struct PricingFixture
    {
        PricingFixture()
            : settings(buyouts.tempDir.filePath("settings.ini"), QSettings::IniFormat)
            , items(settings, *buyouts.manager)
        {}

        BuyoutManagerFixture buyouts;
        QSettings settings;
        ItemsManager items;
    };

    // A stash item as the API sends it; the "note" key is present only when
    // the player has written one.
    std::shared_ptr<Item> makeNotedItem(const QString &id,
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

void BuyoutManagerTest::stringToBuyout()
{
    BuyoutManagerFixture fixture;

    const Buyout chaos = fixture.manager->StringToBuyout("~b/o 5 chaos");
    QCOMPARE(chaos.type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(chaos.value, 5.0);
    QCOMPARE(chaos.currency, Currency::CURRENCY_CHAOS_ORB);
    QCOMPARE(chaos.source, Buyout::BUYOUT_SOURCE_GAME);
    QVERIFY(chaos.IsActive());

    const Buyout exalted = fixture.manager->StringToBuyout("~price 1.5 exalted");
    QCOMPARE(exalted.type, Buyout::BUYOUT_TYPE_FIXED);
    QCOMPARE(exalted.value, 1.5);
    QCOMPARE(exalted.currency, Currency::CURRENCY_EXALTED_ORB);

    const Buyout fusing = fixture.manager->StringToBuyout("~gb/o 20 fusing");
    QCOMPARE(fusing.type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(fusing.value, 20.0);
    QCOMPARE(fusing.currency, Currency::CURRENCY_ORB_OF_FUSING);

    const Buyout currentOffer = fixture.manager->StringToBuyout("~c/o 3 chaos");
    QCOMPARE(currentOffer.type, Buyout::BUYOUT_TYPE_CURRENT_OFFER);
    QCOMPARE(currentOffer.value, 3.0);
    QCOMPARE(currentOffer.currency, Currency::CURRENCY_CHAOS_ORB);

    QVERIFY(fixture.manager->StringToBuyout("not a buyout").IsNull());

    const Buyout tolerant = fixture.manager->StringToBuyout("prefix ~b/o 7 chaos suffix");
    QCOMPARE(tolerant.type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(tolerant.value, 7.0);
    QCOMPARE(tolerant.currency, Currency::CURRENCY_CHAOS_ORB);
}

// A price set on a tab of three items writes one location row and no item
// rows (it wrote one inherited row per item); the items show it.
void BuyoutManagerTest::tabPriceWritesOneRow()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-one-row", "Dump", 0);
    const auto a = makeNotedItem("one-row-a", tab);
    const auto b = makeNotedItem("one-row-b", tab);
    const auto c = makeNotedItem("one-row-c", tab);
    fixture.items.OnItemsRefreshed({a, b, c}, {tab}, false);

    BuyoutTargets targets;
    targets.tabs = {tab};
    const ApplyBuyoutResult result = ApplyBuyout(*fixture.buyouts.manager,
                                                 fixture.items,
                                                 targets,
                                                 makeChaosBuyout(4.0));
    QCOMPARE(result.applied, 1);

    QCOMPARE(fixture.buyouts.repo->loadItemBuyouts().value().size(), size_t(0));
    QCOMPARE(fixture.buyouts.repo->loadLocationBuyouts().value().size(), size_t(1));
    for (const auto &item : {a, b, c}) {
        const EffectivePrice price = fixture.buyouts.manager->Effective(*item);
        QCOMPARE(price.source, PriceSource::ManualTab);
        QCOMPARE(price.buyout.value, 4.0);
    }
}

// An inherited row an older build wrote is ignored on load: it is a copy of
// a tab's price, not a price the user set on the item, and it is never read
// as one.
void BuyoutManagerTest::inheritedRowIsIgnoredOnLoad()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("inherited-row");
    Buyout copy = makeChaosBuyout(3.0);
    copy.inherited = true;
    QCOMPARE(fixture.repo->saveItemBuyout(copy,
                                          item.id(),
                                          item.location().id(),
                                          item.location().type(),
                                          true),
             BuyoutSaveResult::Saved);
    fixture.manager->ReloadBuyouts();
    QVERIFY(fixture.manager->BuyoutsLoaded());

    QVERIFY(fixture.manager->Get(item).IsNull());
    QCOMPARE(fixture.manager->Effective(item).source, PriceSource::None);
}

// The Date column for an item that shows its tab's price shows the tab
// price's own date. It showed when the price was first copied onto the
// item, which changed with every copy.
void BuyoutManagerTest::inheritedDateIsTheTabPricesDate()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-date", "Dump", 0);
    const auto item = makeNotedItem("date-item", tab);
    const QDateTime set_at = QDateTime::fromSecsSinceEpoch(1000);
    QVERIFY(fixture.buyouts.manager->SetTab(tab,
                                            Buyout(4.0,
                                                   Buyout::BUYOUT_TYPE_BUYOUT,
                                                   Currency::CURRENCY_CHAOS_ORB,
                                                   set_at)));
    fixture.items.OnItemsRefreshed({item}, {tab}, false);

    QCOMPARE(fixture.buyouts.manager->Effective(*item).buyout.last_update, set_at);
}

void BuyoutManagerTest::itemBuyoutRoundTrip()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("item-round-trip");
    const Buyout buyout = makeChaosBuyout(9.0);

    fixture.manager->Set(item, buyout);

    QCOMPARE(fixture.manager->Get(item), buyout);
}

void BuyoutManagerTest::tabBuyoutRoundTrip()
{
    BuyoutManagerFixture fixture;
    const ItemLocation location = makeTestStashLocation();
    const Buyout buyout = makeChaosBuyout(11.0);

    fixture.manager->SetTab(location, buyout);

    QCOMPARE(fixture.manager->GetTab(location), buyout);
}

// An item without its own price shows its tab's, read when asked, marked
// inherited; an item whose tab has none shows none. Nothing is stored on
// the item.
void BuyoutManagerTest::itemShowsItsTabsPrice()
{
    BuyoutManagerFixture activeFixture;
    const Item activeItem = makeTestItem("propagated-item");
    activeFixture.manager->SetTab(activeItem.location(), makeChaosBuyout(12.0));

    const Buyout shown = activeFixture.manager->Effective(activeItem).buyout;
    QVERIFY(shown.IsActive());
    QVERIFY(shown.IsInherited());
    QCOMPARE(shown.value, 12.0);
    QCOMPARE(shown.currency, Currency::CURRENCY_CHAOS_ORB);
    QVERIFY(activeFixture.manager->Get(activeItem).IsNull());

    BuyoutManagerFixture inactiveFixture;
    const Item inactiveItem = makeTestItem("cleared-inherited-item");
    QCOMPARE(inactiveFixture.manager->Effective(inactiveItem).source, PriceSource::None);
}

void BuyoutManagerTest::clearingItemBuyoutRemovesInMemoryValue()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("f14-item");

    fixture.manager->Set(item, makeChaosBuyout(14.0));
    QVERIFY(fixture.manager->Get(item).IsActive());

    fixture.manager->Set(item, Buyout());

    QVERIFY(fixture.manager->Get(item).IsNull());
}

void BuyoutManagerTest::clearingTabBuyoutRemovesInMemoryValue()
{
    BuyoutManagerFixture fixture;
    const ItemLocation location = makeTestStashLocation("f14-tab");

    fixture.manager->SetTab(location, makeChaosBuyout(14.0));
    QVERIFY(fixture.manager->GetTab(location).IsActive());

    fixture.manager->SetTab(location, Buyout());

    QVERIFY(fixture.manager->GetTab(location).IsNull());
}

// The F52 guard must not weaken the F14 behavior: clearing a buyout that
// exists still removes the database row, not just the in-memory entry.
void BuyoutManagerTest::clearingItemBuyoutRemovesRepoRow()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("f52-item");

    fixture.manager->Set(item, makeChaosBuyout(52.0));
    QVERIFY(fixture.repo->loadItemBuyouts().value().contains(item.id()));

    fixture.manager->Set(item, Buyout());

    QVERIFY(!fixture.repo->loadItemBuyouts().value().contains(item.id()));
}

void BuyoutManagerTest::clearingTabBuyoutRemovesRepoRow()
{
    BuyoutManagerFixture fixture;
    const ItemLocation location = makeTestStashLocation("f52-tab");

    fixture.manager->SetTab(location, makeChaosBuyout(52.0));
    QVERIFY(fixture.repo->loadLocationBuyouts().value().contains(location.id()));

    fixture.manager->SetTab(location, Buyout());

    QVERIFY(!fixture.repo->loadLocationBuyouts().value().contains(location.id()));
}

// Clearing a buyout the manager does not hold must not touch the repo (F52:
// PropagateTabBuyouts clears nearly every item on every refresh, which used
// to issue one no-op DELETE per item). Pinned by seeding a row behind the
// manager's back: with the guard, the row survives the clear. This is the
// accepted tradeoff — a desynced row lasts until the next Load() picks it
// up — in exchange for not running ~item-count DELETEs per refresh.
void BuyoutManagerTest::clearingAbsentBuyoutSkipsRepo()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("f52-desynced-item");

    fixture.repo->saveItemBuyout(makeChaosBuyout(52.0), item);
    QVERIFY(fixture.manager->Get(item).IsNull());

    fixture.manager->Set(item, Buyout());

    QVERIFY(fixture.repo->loadItemBuyouts().value().contains(item.id()));
}

// SetTab carries the same F52 guard as Set; pin the tab side too.
void BuyoutManagerTest::clearingAbsentTabBuyoutSkipsRepo()
{
    BuyoutManagerFixture fixture;
    const ItemLocation location = makeTestStashLocation("f52-desynced-tab");

    fixture.repo->saveLocationBuyout(makeChaosBuyout(52.0), location);
    QVERIFY(fixture.manager->GetTab(location).IsNull());

    fixture.manager->SetTab(location, Buyout());

    QVERIFY(fixture.repo->loadLocationBuyouts().value().contains(location.id()));
}

// The map entry must survive a failed repo delete so a later clear retries
// it — otherwise the F52 guard would skip every retry and the undeleted row
// would resurrect at the next Load(). A BEFORE DELETE trigger forces the
// failure without disturbing the row, so both states are observable after
// the failed clear and the retry deletes both for real.
void BuyoutManagerTest::failedDeleteKeepsEntryForRetry()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("f52-failed-delete-item");

    fixture.manager->Set(item, makeChaosBuyout(52.0));

    QSqlQuery q(*fixture.db);
    QVERIFY(q.exec("CREATE TRIGGER f52_block_delete BEFORE DELETE ON item_buyouts "
                   "BEGIN SELECT RAISE(FAIL, 'f52 test: delete blocked'); END"));

    fixture.manager->Set(item, Buyout());
    QVERIFY(fixture.manager->Get(item).IsActive());
    QVERIFY(fixture.repo->loadItemBuyouts().value().contains(item.id()));

    QVERIFY(q.exec("DROP TRIGGER f52_block_delete"));

    fixture.manager->Set(item, Buyout());
    QVERIFY(fixture.manager->Get(item).IsNull());
    QVERIFY(!fixture.repo->loadItemBuyouts().value().contains(item.id()));
}

// #257: the map follows the database. A save the repo rejects leaves the
// previous price in place and reports no change to the model.
void BuyoutManagerTest::failedSaveLeavesEntryUnchanged()
{
    BuyoutManagerFixture fixture;
    const Item item = makeTestItem("failed-save-item");
    const ItemLocation location = makeTestStashLocation("failed-save-tab");
    fixture.manager->Set(item, makeChaosBuyout(1.0));
    fixture.manager->SetTab(location, makeChaosBuyout(2.0));

    QSqlQuery q(*fixture.db);
    QVERIFY(q.exec("CREATE TRIGGER block_item_update BEFORE UPDATE ON item_buyouts "
                   "BEGIN SELECT RAISE(FAIL, 'test: update blocked'); END"));
    QVERIFY(q.exec("CREATE TRIGGER block_location_update BEFORE UPDATE ON location_buyouts "
                   "BEGIN SELECT RAISE(FAIL, 'test: update blocked'); END"));

    int changes = 0;
    QObject::connect(fixture.manager.get(),
                     &BuyoutManager::BuyoutsChanged,
                     [&changes](const BuyoutChangeSet &) { ++changes; });
    fixture.manager->Set(item, makeChaosBuyout(5.0));
    fixture.manager->SetTab(location, makeChaosBuyout(6.0));

    QCOMPARE(fixture.manager->Get(item).value, 1.0);
    QCOMPARE(fixture.manager->GetTab(location).value, 2.0);
    QCOMPARE(changes, 0);
}

// An unbatched compression coalesces its erasures into one boundary, and a
// consumer reached by that boundary must observe post-erase lookup state —
// never the entries the compression is about to remove.
void BuyoutManagerTest::unbatchedTabCompressionEmitsOnceAfterErase()
{
    BuyoutManagerFixture fixture;
    const ItemLocation keep = makeTestStashLocation("s2-keep-tab", "Keep", 0);
    const ItemLocation goneOne = makeTestStashLocation("s2-gone-one", "GoneOne", 1);
    const ItemLocation goneTwo = makeTestStashLocation("s2-gone-two", "GoneTwo", 2);
    fixture.manager->SetTab(keep, makeChaosBuyout(1.0));
    fixture.manager->SetTab(goneOne, makeChaosBuyout(2.0));
    fixture.manager->SetTab(goneTwo, makeChaosBuyout(3.0));
    fixture.manager->SetStashTabLocations({keep});

    int emissions = 0;
    bool observedPostEraseState = false;
    std::set<QString> reportedTabs;
    QObject::connect(fixture.manager.get(),
                     &BuyoutManager::BuyoutsChanged,
                     [&](const BuyoutChangeSet &changes) {
                         ++emissions;
                         reportedTabs = changes.tab_ids;
                         observedPostEraseState = fixture.manager->GetTab(goneOne).IsNull()
                                                  && fixture.manager->GetTab(goneTwo).IsNull();
                     });
    fixture.manager->CompressTabBuyouts();

    QCOMPARE(emissions, 1);
    QVERIFY(observedPostEraseState);
    const std::set<QString> expected{"s2-gone-one", "s2-gone-two"};
    QCOMPARE(reportedTabs, expected);
    QVERIFY(fixture.manager->GetTab(keep).IsActive());
}

// #251: deleting an item's in-game note clears the price it set — the
// snapshot passes must not leave a locked, game-set price behind.
void BuyoutManagerTest::removedNoteClearsGamePriceOnSnapshot()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-251a", "Dump", 0);

    const auto priced = makeNotedItem("item-251a", tab, "~b/o 5 chaos");
    fixture.items.OnItemsRefreshed({priced}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Get(*priced).IsGameSet());
    QCOMPARE(fixture.buyouts.manager->Get(*priced).value, 5.0);

    const auto unnoted = makeNotedItem("item-251a", tab);
    fixture.items.OnItemsRefreshed({unnoted}, {tab}, false);

    QVERIFY(fixture.buyouts.manager->Get(*unnoted).IsNull());
    QVERIFY(!fixture.buyouts.repo->loadItemBuyouts().value().contains("item-251a"));
}

// #251: the per-tab delta path applies the same rule as the snapshot.
void BuyoutManagerTest::removedNoteClearsGamePriceOnDelta()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-251b", "Dump", 0);

    const auto priced = makeNotedItem("item-251b", tab, "~b/o 5 chaos");
    fixture.items.OnItemsRefreshed({priced}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Get(*priced).IsGameSet());

    const auto unnoted = makeNotedItem("item-251b", tab);
    fixture.items.OnTabRefreshed(tab, {unnoted});

    QVERIFY(fixture.buyouts.manager->Get(*unnoted).IsNull());
    QVERIFY(!fixture.buyouts.repo->loadItemBuyouts().value().contains("item-251b"));
}

// #251: renaming a tab away from a price clears the tab's game-set price,
// and its items go back to inherit (nothing to inherit from).
void BuyoutManagerTest::renamedTabClearsGameTabPrice()
{
    PricingFixture fixture;
    const ItemLocation priced_tab = makeTestStashLocation("stash-251c", "~price 5 chaos", 0);
    const auto item = makeNotedItem("item-251c", priced_tab);
    fixture.items.OnItemsRefreshed({item}, {priced_tab}, false);
    QVERIFY(fixture.buyouts.manager->GetTab(priced_tab).IsGameSet());
    QVERIFY(fixture.buyouts.manager->Effective(*item).buyout.IsActive());

    const ItemLocation renamed_tab = makeTestStashLocation("stash-251c", "Dump", 0);
    const auto moved = makeNotedItem("item-251c", renamed_tab);
    fixture.items.OnItemsRefreshed({moved}, {renamed_tab}, false);

    QVERIFY(fixture.buyouts.manager->GetTab(renamed_tab).IsNull());
    QVERIFY(fixture.buyouts.manager->Get(*moved).IsNull());
    QVERIFY(!fixture.buyouts.repo->loadLocationBuyouts().value().contains("stash-251c"));
    QVERIFY(!fixture.buyouts.repo->loadItemBuyouts().value().contains("item-251c"));
}

// #251/#273: a note that names a currency the parser does not know yields
// no active price, so it clears an existing game price instead of storing
// an invalid, locked one — on both paths.
void BuyoutManagerTest::unknownCurrencyClearsGamePrice()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-251d", "Dump", 0);

    const auto first = makeNotedItem("item-251d", tab, "~b/o 5 chaos");
    const auto second = makeNotedItem("item-251e", tab, "~b/o 3 chaos");
    fixture.items.OnItemsRefreshed({first, second}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Get(*first).IsGameSet());
    QVERIFY(fixture.buyouts.manager->Get(*second).IsGameSet());

    const auto first_unknown = makeNotedItem("item-251d", tab, "~b/o 5 jewellers");
    const auto second_unknown = makeNotedItem("item-251e", tab, "~b/o 3 jewellers");

    // Delta path first, for the first item only.
    fixture.items.OnTabRefreshed(tab, {first_unknown, second});
    QVERIFY(fixture.buyouts.manager->Get(*first_unknown).IsNull());
    QVERIFY(fixture.buyouts.manager->Get(*second).IsGameSet());

    // Then the snapshot path for the second.
    fixture.items.OnItemsRefreshed({first_unknown, second_unknown}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Get(*second_unknown).IsNull());
    QVERIFY(fixture.buyouts.repo->loadItemBuyouts().value().empty());
}

// The rule never touches a price the user set by hand.
void BuyoutManagerTest::manualPriceWithEmptyNoteIsUntouched()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-251f", "~price 2 chaos", 0);
    const auto item = makeNotedItem("item-251f", tab);
    fixture.items.OnItemsRefreshed({item}, {tab}, false);

    const Buyout manual = makeChaosBuyout(9.0);
    fixture.buyouts.manager->Set(*item, manual);

    fixture.items.OnItemsRefreshed({item}, {tab}, false);
    fixture.items.OnTabRefreshed(tab, {item});
    const ItemLocation renamed = makeTestStashLocation("stash-251f", "Dump", 0);
    const auto moved = makeNotedItem("item-251f", renamed);
    fixture.items.OnItemsRefreshed({moved}, {renamed}, false);

    QCOMPARE(fixture.buyouts.manager->Get(*moved), manual);
    QCOMPARE(fixture.buyouts.manager->Get(*moved).source, Buyout::BUYOUT_SOURCE_MANUAL);
    QVERIFY(fixture.buyouts.repo->loadItemBuyouts().value().contains("item-251f"));
}

// An item showing a game-set tab price carries the game source too, and
// nothing is written for it on a refresh (the F52 class of churn): since
// #293 no row is stored for it at all.
void BuyoutManagerTest::inheritedGameTabPriceIsNotRewritten()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-251g", "~price 5 chaos", 0);
    const auto item = makeNotedItem("item-251g", tab);
    fixture.items.OnItemsRefreshed({item}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Effective(*item).buyout.inherited);

    // Rows written through the fixture's connection, counted by SQLite.
    const auto total_changes = [&fixture] {
        QSqlQuery q(*fixture.buyouts.db);
        return q.exec("SELECT total_changes()") && q.next() ? q.value(0).toLongLong() : -1;
    };
    const qint64 changes_before = total_changes();
    int changes = 0;
    QObject::connect(fixture.buyouts.manager.get(),
                     &BuyoutManager::BuyoutsChanged,
                     [&changes](const BuyoutChangeSet &) { ++changes; });
    fixture.items.OnItemsRefreshed({item}, {tab}, false);
    fixture.items.OnTabRefreshed(tab, {item});

    QCOMPARE(total_changes(), changes_before);
    QCOMPARE(changes, 0);
    QVERIFY(fixture.buyouts.manager->Effective(*item).buyout.IsActive());
}

// The per-league data file is the one the legacy importer reads, and the
// importer accepts only db_version 4 or 5. Pricing a snapshot must leave
// that key alone (#290).
void BuyoutManagerTest::snapshotWritesNoDbVersion()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-290", "Dump", 0);
    const auto item
        = makeNotedItem("4d3a9f0c2b8e7d6a5f4e3d2c1b0a99887766554433221100ffeeddccbbaa9988",
                        tab,
                        "~b/o 5 chaos");
    QVERIFY(fixture.buyouts.data->Get("db_version").isEmpty());

    fixture.items.OnItemsRefreshed({item}, {tab}, false);

    QCOMPARE(fixture.buyouts.manager->Get(*item).value, 5.0);
    QCOMPARE(fixture.buyouts.data->Get("db_version"), QString());
}

// #257: a failed item read leaves the manager without the user's item
// prices, so every item looks like "inherit". A refresh must not then
// propagate the tab's price over the manual row it could not see. The
// rows are seeded behind the manager's back and the reload runs with the
// item table hidden, which is the state a failed read at start-up leaves.
// No note on the item and no price in the tab's name, so only the
// propagation pass could touch the row.
void BuyoutManagerTest::failedItemReadKeepsManualRowThroughRefresh()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-257", "Dump", 0);
    const auto item = makeNotedItem("item-257", tab);

    const Buyout manual = makeChaosBuyout(9.0);
    QCOMPARE(fixture.buyouts.repo->saveItemBuyout(manual, item->id(), tab.id(), tab.type(), true),
             BuyoutSaveResult::Saved);
    QCOMPARE(fixture.buyouts.repo->saveLocationBuyout(makeChaosBuyout(4.0),
                                                      tab.id(),
                                                      tab.type(),
                                                      true),
             BuyoutSaveResult::Saved);

    QSqlQuery q(*fixture.buyouts.db);
    QVERIFY(q.exec("ALTER TABLE item_buyouts RENAME TO item_buyouts_hidden"));
    fixture.buyouts.manager->ReloadBuyouts();
    QVERIFY(q.exec("ALTER TABLE item_buyouts_hidden RENAME TO item_buyouts"));
    QVERIFY(!fixture.buyouts.manager->BuyoutsLoaded());
    QVERIFY(fixture.buyouts.manager->GetTab(tab).IsActive());

    fixture.items.OnItemsRefreshed({item}, {tab}, false);

    const auto rows = fixture.buyouts.repo->loadItemBuyouts();
    QVERIFY(rows);
    QVERIFY(rows->contains("item-257"));
    QCOMPARE(rows->at("item-257").value, 9.0);
    QVERIFY(!rows->at("item-257").inherited);
}

// #148: a tab named with a price but holding no loaded items (empty, or not
// fetched yet) is still locked, so the refresh keeps its price current.
void BuyoutManagerTest::gamePricedEmptyTabIsLocked()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-148a", "~price 5 chaos", 0);

    fixture.items.OnItemsRefreshed({}, {tab}, false);

    QVERIFY(fixture.buyouts.manager->GetTab(tab).IsGameSet());
    QVERIFY(fixture.buyouts.manager->GetRefreshLocked(tab));
    QVERIFY(fixture.buyouts.manager->GetRefreshChecked(tab));
}

// #148: the same for a price set on the tab by hand.
void BuyoutManagerTest::manuallyPricedEmptyTabIsLocked()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-148b", "Dump", 0);
    QVERIFY(fixture.buyouts.manager->SetTab(tab, makeChaosBuyout(5.0)));
    fixture.buyouts.manager->SetRefreshChecked(tab, false);

    fixture.items.OnItemsRefreshed({}, {tab}, false);

    QVERIFY(fixture.buyouts.manager->GetRefreshLocked(tab));
    QVERIFY(fixture.buyouts.manager->GetRefreshChecked(tab));
}

// Remove-only tabs are skipped, with or without items, as before #148:
// whether they should lock is a separate question.
void BuyoutManagerTest::removeOnlyPricedTabStaysUnlocked()
{
    PricingFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-148c", "~price 20 chaos (Remove-only)", 0);
    QVERIFY(tab.removeonly());
    const auto item = makeNotedItem("item-148c", tab);

    fixture.items.OnItemsRefreshed({}, {tab}, false);
    QVERIFY(!fixture.buyouts.manager->GetRefreshLocked(tab));

    fixture.items.OnItemsRefreshed({item}, {tab}, false);
    QVERIFY(fixture.buyouts.manager->Effective(*item).buyout.IsActive());
    QVERIFY(!fixture.buyouts.manager->GetRefreshLocked(tab));
}

// A folder can be named with a price in game, which lists nothing at that
// price (trade-ground-truth T30), and holds no items to refresh.
void BuyoutManagerTest::priceNamedFolderStaysUnlocked()
{
    PricingFixture fixture;
    poe::StashTab stash;
    stash.id = "folder-148d";
    stash.name = "~price 3333 chaos";
    stash.type = "Folder";
    stash.index = 0;
    const ItemLocation folder(stash);
    QVERIFY(folder.IsFolder());

    fixture.items.OnItemsRefreshed({}, {folder}, false);

    QVERIFY(!fixture.buyouts.manager->GetRefreshLocked(folder));
}

QTEST_GUILESS_MAIN(BuyoutManagerTest)

#include "tst_buyoutmanager.moc"
