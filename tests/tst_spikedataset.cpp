// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include <QSet>

#include "itemcategories.h"
#include "spikedataset.h"

// Pins the properties M2 relies on from the dataset generator: the same
// (config, seed) reproduces the same collection and churn sequence (M2-M2's
// fixed recorded shapes, R7-3), and churn breaks pointer identity while
// stable item identity survives (what D6/R6-3 key on).
class SpikeDatasetTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void sameSeedReproducesCollectionAndChurn();
    void churnPreservesStableIdentityNotPointers();
    void namedPresetsMatchRecordedShapes();
};

static SpikeDataset::Config smallConfig()
{
    SpikeDataset::Config config;
    config.tab_count = 20;
    config.mean_items_per_tab = 30;
    return config;
}

static QStringList itemIds(const Items &items)
{
    QStringList ids;
    ids.reserve(items.size());
    for (const auto &item : items) {
        ids.push_back(item->id());
    }
    return ids;
}

void SpikeDatasetTest::initTestCase()
{
    // Item construction reads the process-global category tables.
    InitItemClasses(R"json({"TestClass":{"name":"Weapons"}})json");
    InitItemBaseTypes(
        R"json({"Metadata/Items/TestSword":{"item_class":"TestClass","name":"Test Sword","release_state":"released"}})json");
}

void SpikeDatasetTest::sameSeedReproducesCollectionAndChurn()
{
    SpikeDataset a(smallConfig());
    SpikeDataset b(smallConfig());

    QCOMPARE(a.tabCount(), 20);
    QCOMPARE(b.totalItems(), a.totalItems());
    QCOMPARE(static_cast<qsizetype>(a.allItems().size()), a.totalItems());
    for (int t = 0; t < a.tabCount(); ++t) {
        QCOMPARE(b.tabName(t), a.tabName(t));
        QCOMPARE(itemIds(b.tabItems(t)), itemIds(a.tabItems(t)));
    }

    // The churn sequence is part of the reproducible shape, not just the
    // starting collection.
    QCOMPARE(itemIds(b.ChurnTab(3, 0.5)), itemIds(a.ChurnTab(3, 0.5)));
    QCOMPARE(itemIds(b.ChurnTab(3, 0.5)), itemIds(a.ChurnTab(3, 0.5)));
}

void SpikeDatasetTest::churnPreservesStableIdentityNotPointers()
{
    SpikeDataset dataset(smallConfig());
    const int tab = 5;

    const Items before = dataset.tabItems(tab);
    const QStringList before_id_list = itemIds(before);
    const QSet<QString> before_ids(before_id_list.begin(), before_id_list.end());
    const Items after = dataset.ChurnTab(tab, 0.5);
    const QStringList after_id_list = itemIds(after);
    const QSet<QString> after_ids(after_id_list.begin(), after_id_list.end());

    // Removals are matched by arrivals, so the tab size is unchanged.
    QCOMPARE(after.size(), before.size());

    // Kept and modified items keep their ids; arrivals are genuinely new.
    const auto surviving = after_ids & before_ids;
    const auto arrivals = after_ids - before_ids;
    QVERIFY(!surviving.isEmpty());
    QVERIFY(!arrivals.isEmpty());
    QCOMPARE(surviving.size() + arrivals.size(), after_ids.size());

    // Every materialized Item is a new object, even for kept specs.
    for (const auto &new_item : after) {
        for (const auto &old_item : before) {
            QVERIFY(new_item.get() != old_item.get());
        }
    }
}

// The named presets are the recorded M2-M2/spike shapes (M3 S0): the
// M1-M3 budget rows cite "spike presets", so their definition is pinned
// here rather than re-tuned per harness.
void SpikeDatasetTest::namedPresetsMatchRecordedShapes()
{
    const auto k100 = SpikeDataset::Config::Preset("100k");
    QVERIFY(k100.has_value());
    QCOMPARE(k100->tab_count, 2000);
    QCOMPARE(k100->mean_items_per_tab, 50);
    QCOMPARE(k100->quad_share, 0.1);
    QCOMPARE(k100->seed, 20260729u);

    const auto k1m = SpikeDataset::Config::Preset("1m");
    QVERIFY(k1m.has_value());
    QCOMPARE(k1m->tab_count, 2600);
    QCOMPARE(k1m->mean_items_per_tab, 400);
    QCOMPARE(k1m->quad_share, 0.8);
    QCOMPARE(k1m->seed, 20260729u);

    // Smoke is the S1-M2 harness's recorded shape (50 tabs / mean 20,
    // ~1k items): same generator machinery and seed as the recorded
    // scales, small enough for functional runs.
    const auto smoke = SpikeDataset::Config::Preset("smoke");
    QVERIFY(smoke.has_value());
    QCOMPARE(smoke->tab_count, 50);
    QCOMPARE(smoke->mean_items_per_tab, 20);
    QCOMPARE(smoke->quad_share, 0.1);
    QCOMPARE(smoke->seed, 20260729u);
    SpikeDataset dataset(*smoke);
    QCOMPARE(dataset.tabCount(), 50);
    QVERIFY(dataset.totalItems() > 500);
    QVERIFY(dataset.totalItems() < 5000);

    QVERIFY(!SpikeDataset::Config::Preset("bogus").has_value());
}

QTEST_GUILESS_MAIN(SpikeDatasetTest)

#include "tst_spikedataset.moc"
