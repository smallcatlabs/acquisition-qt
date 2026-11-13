// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include <algorithm>
#include <memory>
#include <variant>

#include "bucket.h"
#include "buyout.h"
#include "currency.h"
#include "filters/filterspec.h"
#include "locationinventory.h"
#include "modelprobes.h"
#include "search.h"
#include "testfixtures.h"

class SearchTest : public QObject
{
    Q_OBJECT

private slots:
    void bucketConstruction();
    void nameFilterMembership();
    void backgroundRefilterUsesOwnState();
    void backgroundBooleanRefilterUsesOwnState();
    void backgroundMinMaxRefilterUsesOwnState();
    // #268: the filters whose decision follows the prices are marked, as
    // columns are; a search reports one while it is active.
    void priceFiltersAreMarked();
    void tabChangeSkipsRefilterWhenStateIsUnchanged();
    void tabChangeRefiltersAfterStateChange();
    void probeCountersTrackRefilterAndSort();
    void keyedOrderMatchesComparatorOrder();
    void intendedTieBreakRestored();
    // #269: the Inf column sorts by influence, through both the keyed sort
    // and the comparator, and still displays icons only.
    void influenceColumnSortsByInfluence();
    // M3 S6 (R1-2/R1-7): the final reconciliation is authoritative at the
    // row grain, which is what licenses clearing the fail-safe dirty flag
    // a skipped delta left behind — the
    // `appliedDeltasLeaveActiveSearchClean` clause the MainWindow fixture
    // cannot reach (no reachable skip path exists there since S5).
    void reconciliationDischargesFailSafeDirtiness();
    // S6 review round 1: the reconciliation's diff is per stable key, not
    // global pointer membership. ApplyTabDelta can park an item under the
    // delta anchor's bucket even when the item's own location keys
    // elsewhere (insertions target the anchor); the reconciliation must
    // move it home, never retain-and-duplicate.
    void reconciliationRehomesWrongBucketRow();
    // #271: a mods row with no mod chosen leaves the search unfiltered.
    void blankModsRowLeavesSearchUnfiltered();
};

template<typename Payload>
static qsizetype findFilterIndex(const FilterCatalog &catalog, const QString &caption)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto &spec = catalog[index];
        if ((spec.caption == caption) && std::holds_alternative<Payload>(spec.payload)) {
            return index;
        }
    }
    return -1;
}

static std::shared_ptr<Item> makeSearchItem(const QString &id,
                                            const QString &name,
                                            const QString &typeLine,
                                            const ItemLocation &location,
                                            const QString &extraJson = {})
{
    const QByteArray json = QString(R"json({
        "baseType": "%3",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "%1",
        "identified": true,
        "ilvl": 1,
        "name": "%2",
        "typeLine": "%3",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0%4
    })json")
                                .arg(id, name, typeLine, extraJson)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), location));
}

void SearchTest::priceFiltersAreMarked()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    // Only a filter built from the BuyoutManager can read a price, and
    // those are built marked (filterspec.cpp); Priced is the one today.
    const qsizetype pricedIndex = findFilterIndex<BoolPayload>(catalog, "Priced");
    const qsizetype corruptedIndex = findFilterIndex<BoolPayload>(catalog, "Corrupted");
    QVERIFY(pricedIndex >= 0);
    QVERIFY(corruptedIndex >= 0);
    QCOMPARE(catalog.buyoutDependentIndices(), std::vector<qsizetype>{pricedIndex});

    Search search(*buyoutFixture.manager, "Marked", catalog);
    QVERIFY(!search.UsesBuyoutDependentFilter());
    search.setFilterState(corruptedIndex, BoolState{true});
    QVERIFY(!search.UsesBuyoutDependentFilter());
    search.setFilterState(pricedIndex, BoolState{true});
    QVERIFY(search.UsesBuyoutDependentFilter());
    search.setFilterState(pricedIndex, BoolState{false});
    QVERIFY(!search.UsesBuyoutDependentFilter());
}

void SearchTest::bucketConstruction()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    const ItemLocation emptyTab = makeTestStashLocation("stash-empty", "Empty Tab", 2);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab, emptyTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "All", catalog);
    search.FilterItems(items);

    QCOMPARE(search.GetCaption(), "All [2]");
    QCOMPARE(search.buckets().size(), 3);
    QCOMPARE(search.buckets()[0].location().id(), firstTab.id());
    QCOMPARE(search.buckets()[0].items().size(), 1);
    QCOMPARE(search.buckets()[1].location().id(), secondTab.id());
    QCOMPARE(search.buckets()[1].items().size(), 1);
    QCOMPARE(search.buckets()[2].location().id(), emptyTab.id());
    QCOMPARE(search.buckets()[2].items().size(), 0);

    search.SetViewMode(Search::ViewMode::ByItem);
    QCOMPARE(search.buckets().size(), 1);
    QCOMPARE(search.buckets()[0].items().size(), 2);
}

void SearchTest::nameFilterMembership()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    const ItemLocation emptyTab = makeTestStashLocation("stash-empty", "Empty Tab", 2);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab, emptyTab});

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype nameIndex = findFilterIndex<TextPayload>(catalog, "Name");
    QVERIFY(nameIndex >= 0);
    Search search(*buyoutFixture.manager, "Filtered", catalog);
    search.setFilterState(nameIndex, TextState{"alpha"});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    search.FilterItems(items);

    QCOMPARE(search.GetCaption(), "Filtered [1]");
    QCOMPARE(search.buckets().size(), 1);
    QCOMPARE(search.buckets()[0].location().id(), firstTab.id());
    QCOMPARE(search.buckets()[0].items().size(), 1);
    QCOMPARE(search.buckets()[0].items()[0]->id(), "alpha-item");

    search.SetViewMode(Search::ViewMode::ByItem);
    QCOMPARE(search.buckets().size(), 1);
    QCOMPARE(search.buckets()[0].items().size(), 1);
    QCOMPARE(search.buckets()[0].items()[0]->id(), "alpha-item");
}

void SearchTest::backgroundRefilterUsesOwnState()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype nameIndex = findFilterIndex<TextPayload>(catalog, "Name");
    QVERIFY(nameIndex >= 0);
    Search background(*buyoutFixture.manager, "Background", catalog);
    background.setFilterState(nameIndex, TextState{"alpha"});
    // The current search leaves its name empty: under the old shared-activity
    // design that made the name filter inactive for every search, so the
    // background search's own query was skipped and it kept both items.
    Search current(*buyoutFixture.manager, "Current", catalog);
    background.FilterItems(items);

    // F33: a background search uses its own saved activity and query.
    QCOMPARE(background.GetCaption(), "Background [1]");
    QCOMPARE(background.items().size(), 1);
    QCOMPARE(background.items().front()->id(), "alpha-item");
}

void SearchTest::backgroundBooleanRefilterUsesOwnState()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("corrupted-item",
                                   "Corrupted Bite",
                                   "Vaal Axe",
                                   firstTab,
                                   R"json(, "corrupted": true)json"));
    items.push_back(makeSearchItem("ordinary-item", "Ordinary Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype corruptedIndex = findFilterIndex<BoolPayload>(catalog, "Corrupted");
    QVERIFY(corruptedIndex >= 0);
    Search background(*buyoutFixture.manager, "Background", catalog);
    background.setFilterState(corruptedIndex, BoolState{true});
    Search current(*buyoutFixture.manager, "Current", catalog);
    current.setFilterState(corruptedIndex, BoolState{false});
    background.FilterItems(items);

    QCOMPARE(background.GetCaption(), "Background [1]");
    QCOMPARE(background.items().size(), 1);
    QCOMPARE(background.items().front()->id(), "corrupted-item");
}

void SearchTest::backgroundMinMaxRefilterUsesOwnState()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("critical-item",
                                   "Critical Bite",
                                   "Vaal Axe",
                                   firstTab,
                                   R"json(,
        "properties": [
            {
                "displayMode": 0,
                "name": "Critical Strike Chance",
                "type": 6,
                "values": [["6", 1]]
            }
        ])json"));
    items.push_back(makeSearchItem("ordinary-item", "Ordinary Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype critIndex = findFilterIndex<MinMaxPayload>(catalog, "Crit.");
    QVERIFY(critIndex >= 0);
    Search background(*buyoutFixture.manager, "Background", catalog);
    background.setFilterState(critIndex, MinMaxState{5.0, std::nullopt});
    // As above: the current search leaves both bounds empty, which is what made
    // the old shared activity flag drop the background search's own bounds.
    Search current(*buyoutFixture.manager, "Current", catalog);
    background.FilterItems(items);

    QCOMPARE(background.GetCaption(), "Background [1]");
    QCOMPARE(background.items().size(), 1);
    QCOMPARE(background.items().front()->id(), "critical-item");
}

// The TabChanged short-circuit is an optimization: switching tabs alone cannot
// change what a search matches, so it keeps its buckets.
void SearchTest::tabChangeSkipsRefilterWhenStateIsUnchanged()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Search", catalog);
    search.FilterItems(items);
    QCOMPARE(search.items().size(), 2);

    // Nothing changed, so a tab change must not re-run the filters: the new
    // item list is ignored entirely.
    Items moreItems = items;
    moreItems.push_back(makeSearchItem("gamma-item", "Gamma Blade", "Vaal Axe", firstTab));
    search.SetRefreshReason(RefreshReason::TabChanged);
    search.FilterItems(moreItems);
    QCOMPARE(search.items().size(), 2);
}

// ...but a filter state edited while this search was in the background (the
// mods form writes through to the bound search) must force the refilter that
// the debounced refresh gave to whichever search was current when it fired.
void SearchTest::tabChangeRefiltersAfterStateChange()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype nameIndex = findFilterIndex<TextPayload>(catalog, "Name");
    QVERIFY(nameIndex >= 0);
    Search search(*buyoutFixture.manager, "Search", catalog);
    search.FilterItems(items);
    QCOMPARE(search.items().size(), 2);

    search.setFilterState(nameIndex, TextState{"alpha"});
    search.SetRefreshReason(RefreshReason::TabChanged);
    search.FilterItems(items);

    QCOMPARE(search.GetCaption(), "Search [1]");
    QCOMPARE(search.items().size(), 1);
    QCOMPARE(search.items().front()->id(), "alpha-item");

    // The refilter clears the dirty flag: the short-circuit is back in force.
    Items moreItems = items;
    moreItems.push_back(makeSearchItem("gamma-item", "Alpha Blade", "Vaal Axe", firstTab));
    search.SetRefreshReason(RefreshReason::TabChanged);
    search.FilterItems(moreItems);
    QCOMPARE(search.items().size(), 1);
}

// The S0 probe surface (items-pipeline M3): the counters the M3 pins
// read. This pins the sites that exist today — refilter, index rebuild,
// model reset (with per-model attribution), bucket sort (with stable
// (type, id) attribution), all three comparator implementations — that
// probes count nothing while disabled, and that the later-stage fields
// stay untouched until their machinery lands.
void SearchTest::probeCountersTrackRefilterAndSort()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));
    items.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", secondTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Probed", catalog);

    auto &probes = ModelProbes::instance();

    // Disabled (the production default): sites count nothing. The gauge is
    // the exception (S3): it tracks live key ownership unconditionally, so
    // this test asserts its deltas against the entry baseline.
    probes.reset();
    QVERIFY(!probes.enabled);
    const std::int64_t baseline_key_bytes = probes.live_key_bytes;
    search.FilterItems(items);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.model_resets, 0);

    // Enabled: the same refilter counts (the gate only short-circuits
    // TabChanged, so this run does full work again).
    probes.enabled = true;
    search.FilterItems(items);
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.index_rebuilds, 1);
    QCOMPARE(probes.model_resets, 1);
    QCOMPARE(probes.model_resets_by_model[&search.model()], 1);
    QCOMPARE(probes.bucket_sorts, 0); // FilterItems never sorts

    // SetViewMode sorts the active buckets — here the By-Item flat
    // bucket, which reports the null location's stable key. Since S1 the
    // sort is keyed (D1): one key-vector build per bucket, tuple compares
    // instead of comparator calls — sorting never calls Column::lt.
    search.SetViewMode(Search::ViewMode::ByItem);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(ItemLocation())], 1);
    QCOMPARE(probes.key_builds, 1);
    QCOMPARE(probes.key_builds_by_location[LocationInventory::KeyFor(ItemLocation())], 1);
    QVERIFY(probes.keyed_compares > 0);
    QCOMPARE(probes.comparator_calls, 0);
    QCOMPARE(probes.model_resets, 2);

    // All three comparator implementations are instrumented: the
    // Column::lt base and the PriceColumn/DateColumn overrides
    // (search columns 0/1/2 — search.cpp's column table).
    const auto &columns = search.columns();
    for (const size_t column_index : {size_t(0), size_t(1), size_t(2)}) {
        const std::int64_t before = probes.comparator_calls;
        columns[column_index]->lt(items[0].get(), items[1].get());
        QCOMPARE(probes.comparator_calls, before + 1);
    }

    // The TabChanged gate skips clean searches: no refilter, no reset.
    search.SetRefreshReason(RefreshReason::TabChanged);
    search.FilterItems(items);
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.index_rebuilds, 1);
    QCOMPARE(probes.model_resets, 2);

    // The batched-model-update site (S2) lives in MainWindow's batch
    // response, which this direct harness never reaches.
    QCOMPARE(probes.model_updates, 0);

    // Since S3 the SetViewMode sort left the flat bucket's keys resident
    // (D1): the gauge rose above the baseline, and evicting returns it
    // exactly — the store owns its share of the gauge.
    QVERIFY(probes.live_key_bytes > baseline_key_bytes);
    search.EvictResidentKeys();
    QCOMPARE(probes.live_key_bytes, baseline_key_bytes);

    probes.enabled = false;
}

// Mixed-path item maker for the S1 sort pins. An empty id yields an
// id-less item (m_uid stays empty — the D5 hash tie-break's subject);
// extraJson supplies the properties that steer Column::multivalue down
// its double / dash-range / slash-range / string paths, and distinct
// property values also give otherwise-identical items distinct hashes.
static std::shared_ptr<Item> makeKeyedItem(const QString &id,
                                           const QString &name,
                                           const QString &typeLine,
                                           const ItemLocation &location,
                                           const QString &extraJson = {})
{
    const QString idJson = id.isEmpty() ? QString() : QString(R"json("id": "%1",)json").arg(id);
    const QByteArray json = QString(R"json({
        "baseType": "%3",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        %1
        "identified": true,
        "ilvl": 1,
        "name": "%2",
        "typeLine": "%3",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0%4
    })json")
                                .arg(idJson, name, typeLine, extraJson)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), location));
}

static QString propertyJson(const QString &name, const QString &value)
{
    return QString(R"json(,
        "properties": [
            {"displayMode": 0, "name": "%1", "type": 6, "values": [["%2", 0]]}
        ])json")
        .arg(name, value);
}

// The mixed dataset the S1 pins sort: double path, dash range, slash
// range, string path, heavy ties on both column values and names, and
// id-less items. Every item is distinct in (uid, hash), so each
// comparator induces a total order and the expected sequence is unique.
static Items makeMixedSortItems(const ItemLocation &tab)
{
    Items items;
    // Double path (Quality is stored stripped: "+20%" -> "20").
    items.push_back(
        makeKeyedItem("q20", "Gloom Bite", "Vaal Axe", tab, propertyJson("Quality", "+20%")));
    items.push_back(
        makeKeyedItem("q7", "Iron Song", "Copper Shield", tab, propertyJson("Quality", "+7%")));
    // Heavy tie on the Quality column value (suffix decides).
    items.push_back(
        makeKeyedItem("q20b", "Storm Mark", "Vaal Axe", tab, propertyJson("Quality", "+20%")));
    // Dash range: mean of the bounds.
    items.push_back(
        makeKeyedItem("pd1", "Dread Roar", "Sword", tab, propertyJson("Physical Damage", "12-24")));
    items.push_back(
        makeKeyedItem("pd2", "Sacred Whorl", "Sword", tab, propertyJson("Physical Damage", "30-50")));
    // Slash range: (PrettyName, first value).
    items.push_back(
        makeKeyedItem("st1", "Whisper Card", "Card", tab, propertyJson("Stack Size", "10/20")));
    items.push_back(
        makeKeyedItem("st2", "Ancient Card", "Card", tab, propertyJson("Stack Size", "5/40")));
    // String path: a property value neither regex matches.
    items.push_back(
        makeKeyedItem("txt", "Hollow Grasp", "Wand", tab, propertyJson("Armour", "N/A")));
    // Plain items (every property column empty -> string path on "").
    items.push_back(makeKeyedItem("plain1", "Frozen Pledge", "Amulet", tab));
    items.push_back(makeKeyedItem("plain2", "", "Belt", tab));
    // Name ties: identical PrettyName, distinct uids.
    items.push_back(makeKeyedItem("twin-a", "Twin Ward", "Dagger", tab));
    items.push_back(makeKeyedItem("twin-b", "Twin Ward", "Dagger", tab));
    items.push_back(makeKeyedItem("twin-c", "Twin Ward", "Dagger", tab));
    // Id-less items tying on PrettyName: only the hash separates them.
    items.push_back(
        makeKeyedItem("", "Nameless Twin", "Claw", tab, propertyJson("Evasion Rating", "31")));
    items.push_back(
        makeKeyedItem("", "Nameless Twin", "Claw", tab, propertyJson("Evasion Rating", "77")));
    return items;
}

static QStringList itemOrderSignature(const Items &items)
{
    QStringList result;
    result.reserve(static_cast<qsizetype>(items.size()));
    for (const auto &item : items) {
        result.push_back(item->id() + "/" + item->hash_v4());
    }
    return result;
}

// S1 pin (items-pipeline-m3.md, sort-correctness): for every column, the
// keyed Bucket::Sort of the mixed dataset is identical to a direct
// Column::lt sort with the D5-fixed comparator, both directions.
void SearchTest::keyedOrderMatchesComparatorOrder()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation tab = makeTestStashLocation("stash-keys", "Keyed Tab", 0);
    buyoutFixture.manager->SetStashTabLocations({tab});

    const Items items = makeMixedSortItems(tab);

    // Buyout variety for the Price and Date columns: distinct currencies,
    // values, and timestamps, plus items left at the default (no buyout).
    buyoutFixture.manager->Set(*items[0], makeChaosBuyout(9.0));
    buyoutFixture.manager->Set(*items[1], makeChaosBuyout(2.5));
    buyoutFixture.manager->Set(*items[3],
                               Buyout(1.0,
                                      Buyout::BUYOUT_TYPE_BUYOUT,
                                      Currency::CURRENCY_EXALTED_ORB,
                                      QDateTime::fromSecsSinceEpoch(1700000000)));
    buyoutFixture.manager->Set(*items[5],
                               Buyout(9.0,
                                      Buyout::BUYOUT_TYPE_BUYOUT,
                                      Currency::CURRENCY_CHAOS_ORB,
                                      QDateTime::fromSecsSinceEpoch(1600000000)));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Keyed", catalog);
    const auto &columns = search.columns();
    QVERIFY(columns.size() > 0);

    for (size_t column_index = 0; column_index < columns.size(); ++column_index) {
        const Column &column = *columns[column_index];
        for (const Qt::SortOrder order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            Bucket bucket(tab);
            bucket.AddItems(items);
            bucket.Sort(column, order);

            Items expected = items;
            std::sort(expected.begin(),
                      expected.end(),
                      [&column, order](const auto &lhs, const auto &rhs) {
                          if (order == Qt::AscendingOrder) {
                              return column.lt(lhs.get(), rhs.get());
                          } else {
                              return column.lt(rhs.get(), lhs.get());
                          }
                      });

            const QStringList keyed = itemOrderSignature(bucket.items());
            const QStringList comparator = itemOrderSignature(expected);
            if (keyed != comparator) {
                qWarning("column %zu (%s), order %d",
                         column_index,
                         qPrintable(column.name()),
                         static_cast<int>(order));
            }
            QCOMPARE(keyed, comparator);
        }
    }
}

void SearchTest::influenceColumnSortsByInfluence()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation tab = makeTestStashLocation("stash-inf", "Inf Tab", 0);
    buyoutFixture.manager->SetStashTabLocations({tab});

    // Names in a different order from the influences, so a column that
    // falls back to the name cannot produce the expected order.
    Items items;
    items.push_back(makeKeyedItem("none", "Alpha Ward", "Belt", tab));
    items.push_back(makeKeyedItem("shaper",
                                  "Bravo Ward",
                                  "Belt",
                                  tab,
                                  R"json(, "influences": {"shaper": true})json"));
    items.push_back(makeKeyedItem("elder",
                                  "Charlie Ward",
                                  "Belt",
                                  tab,
                                  R"json(, "influences": {"elder": true})json"));
    items.push_back(makeKeyedItem("hunter-warlord",
                                  "Delta Ward",
                                  "Belt",
                                  tab,
                                  R"json(, "influences": {"hunter": true, "warlord": true})json"));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Inf", catalog);
    const Column *inf = nullptr;
    for (const auto &column : search.columns()) {
        if (column->name() == "Inf") {
            inf = column.get();
        }
    }
    QVERIFY(inf);

    // The cell shows the influence icons and no text.
    QVERIFY(inf->value(*items[1]).toString().isEmpty());

    const QStringList expected{"none", "elder", "hunter-warlord", "shaper"};

    Bucket bucket(tab);
    bucket.AddItems(items);
    bucket.Sort(*inf, Qt::AscendingOrder);
    QStringList keyed;
    for (const auto &item : bucket.items()) {
        keyed.push_back(item->id());
    }
    QCOMPARE(keyed, expected);

    Items byComparator = items;
    std::sort(byComparator.begin(), byComparator.end(), [inf](const auto &lhs, const auto &rhs) {
        return inf->lt(lhs.get(), rhs.get());
    });
    QStringList compared;
    for (const auto &item : byComparator) {
        compared.push_back(item->id());
    }
    QCOMPARE(compared, expected);
}

// S1 pin (items-pipeline-m3.md, sort-correctness): id-less items tying on
// PrettyName order deterministically by hash across repeated sorts
// (F67 resolved — the comparator's third tie-break element is now the
// right-hand item's hash, not dead code).
void SearchTest::intendedTieBreakRestored()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation tab = makeTestStashLocation("stash-ties", "Tie Tab", 0);
    buyoutFixture.manager->SetStashTabLocations({tab});

    // Same PrettyName, no uid; distinct property values give distinct
    // hashes, which are all that can separate these items.
    Items ties;
    ties.push_back(makeKeyedItem("", "Echo Twin", "Ring", tab, propertyJson("Armour", "10")));
    ties.push_back(makeKeyedItem("", "Echo Twin", "Ring", tab, propertyJson("Armour", "20")));
    ties.push_back(makeKeyedItem("", "Echo Twin", "Ring", tab, propertyJson("Armour", "30")));
    for (const auto &item : ties) {
        QCOMPARE(item->id(), QString());
        QCOMPARE(item->PrettyName(), "Echo Twin Ring");
    }

    // The comparator is decisive on every tying pair: exactly one of
    // (a < b), (b < a) holds (before the F67 fix, neither did).
    for (size_t a = 0; a < ties.size(); ++a) {
        for (size_t b = a + 1; b < ties.size(); ++b) {
            QVERIFY((*ties[a] < *ties[b]) != (*ties[b] < *ties[a]));
        }
    }

    // The intended order is by hash.
    Items expected = ties;
    std::sort(expected.begin(), expected.end(), [](const auto &lhs, const auto &rhs) {
        return lhs->hash_v4() < rhs->hash_v4();
    });
    const QStringList ascending = itemOrderSignature(expected);
    QStringList descending = ascending;
    std::reverse(descending.begin(), descending.end());

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Ties", catalog);
    const Column &name_column = *search.columns()[0];

    // Every insertion order sorts to the same hash order, and repeated
    // sorts of the same bucket never reshuffle the ties.
    for (size_t rotation = 0; rotation < ties.size(); ++rotation) {
        Items arrival = ties;
        std::rotate(arrival.begin(),
                    arrival.begin() + static_cast<std::ptrdiff_t>(rotation),
                    arrival.end());

        Bucket bucket(tab);
        bucket.AddItems(arrival);
        bucket.Sort(name_column, Qt::AscendingOrder);
        QCOMPARE(itemOrderSignature(bucket.items()), ascending);
        bucket.Sort(name_column, Qt::AscendingOrder);
        QCOMPARE(itemOrderSignature(bucket.items()), ascending);
        bucket.Sort(name_column, Qt::DescendingOrder);
        QCOMPARE(itemOrderSignature(bucket.items()), descending);
    }
}

void SearchTest::reconciliationDischargesFailSafeDirtiness()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation tabA = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({tabA, tabB});
    LocationInventory inventory;
    inventory.ResetTo({tabA, tabB});

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Search", catalog, &inventory);

    Items initial;
    initial.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", tabA));
    initial.push_back(makeSearchItem("beta-item", "Beta Guard", "Copper Shield", tabB));
    search.FilterItems(initial);
    QVERIFY(!search.itemsDirty());

    // A skipped delta (the R1-7 fail-safe direction): the flag goes
    // dirty and the published state moves on without the model — tab A's
    // content was replaced, tab B was deleted, tab C was discovered.
    search.setItemsDirty(true);
    const ItemLocation tabC = makeTestStashLocation("stash-c", "Gamma Tab", 2);
    Items published;
    published.push_back(makeSearchItem("alpha-two", "Alpha Two", "Vaal Axe", tabA));
    buyoutFixture.manager->SetStashTabLocations({tabA, tabC});
    inventory.ResetTo({tabA, tabC});

    QSignalSpy resets(&search.model(), &QAbstractItemModel::modelReset);
    const auto result = search.ReconcileFinalSnapshot(published);

    // Authoritative at the row grain, via row operations only: the model
    // equals a fresh refilter of the published state, so the flag clears.
    QCOMPARE(resets.count(), 0);
    QVERIFY(result.content_changed);
    QVERIFY(!search.itemsDirty());
    QCOMPARE(search.GetCaption(), "Search [1]");
    const auto &buckets = search.buckets();
    QCOMPARE(buckets.size(), 2);
    QCOMPARE(buckets[0].location().id(), tabA.id());
    QCOMPARE(buckets[0].items().size(), 1);
    QCOMPARE(buckets[0].items()[0]->id(), "alpha-two");
    QCOMPARE(buckets[1].location().id(), tabC.id());
    QCOMPARE(buckets[1].items().size(), 0);

    // Clean again: a tab change refilters nothing.
    search.SetRefreshReason(RefreshReason::TabChanged);
    search.FilterItems(published);
    QCOMPARE(resets.count(), 0);
}

void SearchTest::reconciliationRehomesWrongBucketRow()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation tabA = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({tabA, tabB});
    LocationInventory inventory;
    inventory.ResetTo({tabA, tabB});

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    Search search(*buyoutFixture.manager, "Search", catalog, &inventory);

    const auto alpha_item = makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", tabA);
    const auto beta_item = makeSearchItem("beta-item", "Beta Guard", "Copper Shield", tabB);
    Items initial;
    initial.push_back(alpha_item);
    initial.push_back(beta_item);
    search.FilterItems(initial);

    // Fabricate the wrong-bucket state through the public delta API: the
    // arrival's own location keys to tab B, but ApplyTabDelta inserts
    // into the delta anchor's bucket — tab A.
    const auto wanderer = makeSearchItem("wanderer", "Wanderer", "Vaal Axe", tabB);
    const auto delta = search.ApplyTabDelta(tabA, {wanderer});
    QVERIFY(delta.processed);
    QCOMPARE(search.buckets()[0].location().id(), tabA.id());
    QCOMPARE(search.buckets()[0].items().size(), 1);
    QCOMPARE(search.buckets()[0].items()[0]->id(), "wanderer");

    // The snapshot publishes that same object (plus tab B's resident):
    // the per-key diff must re-home it — removed from A, inserted under
    // B, exactly one occurrence — via row operations.
    Items published;
    published.push_back(wanderer);
    published.push_back(beta_item);
    QSignalSpy resets(&search.model(), &QAbstractItemModel::modelReset);
    const auto result = search.ReconcileFinalSnapshot(published);
    QCOMPARE(resets.count(), 0);
    QVERIFY(result.content_changed);

    const auto &buckets = search.buckets();
    QCOMPARE(buckets.size(), 2);
    QCOMPARE(buckets[0].location().id(), tabA.id());
    QCOMPARE(buckets[0].items().size(), 0); // unfiltered: empty bucket row stays
    QCOMPARE(buckets[1].location().id(), tabB.id());
    QCOMPARE(buckets[1].items().size(), 2);
    int occurrences = 0;
    for (const auto &bucket : buckets) {
        for (const auto &item : bucket.items()) {
            occurrences += (item->id() == "wanderer") ? 1 : 0;
        }
    }
    QCOMPARE(occurrences, 1);
    QCOMPARE(search.GetCaption(), "Search [2]");
}

void SearchTest::blankModsRowLeavesSearchUnfiltered()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation emptyTab = makeTestStashLocation("stash-empty", "Empty Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, emptyTab});

    Items items;
    items.push_back(makeSearchItem("alpha-item", "Alpha Bite", "Vaal Axe", firstTab));

    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const qsizetype modsIndex = findFilterIndex<ModsPayload>(catalog, "Mods");
    QVERIFY(modsIndex >= 0);
    Search search(*buyoutFixture.manager, "Blank", catalog);
    search.setFilterState(modsIndex, ModsState{{ModRow{}}});
    search.FilterItems(items);

    // Unfiltered: the empty tab is still shown and buckets start collapsed.
    QCOMPARE(search.buckets().size(), 2);
    QCOMPARE(search.buckets()[1].location().id(), emptyTab.id());
    QVERIFY(!search.defaultExpanded());
}

QTEST_MAIN(SearchTest)

#include "tst_search.moc"
