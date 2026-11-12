// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include <QAbstractItemModelTester>
#include <QSignalSpy>
#include <QTreeView>

#include <algorithm>
#include <memory>

#include "filters/filterspec.h"
#include "items_model.h"
#include "search.h"
#include "testfixtures.h"

class ItemsModelTest : public QObject
{
    Q_OBJECT

private slots:
    void testerSurvivesRebuildModeSwitchAndSort();
    void selectionSurvivesSort();
    void sortDirectionMatchesOrder();
    // Folder-tree S2 (D4): the two conversions every bucket-or-item
    // decision goes through.
    void bucketRowConversionsRoundTrip();
};

static std::shared_ptr<Item> makeModelItem(const QString &id,
                                           const QString &name,
                                           const QString &typeLine,
                                           const ItemLocation &location)
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
        "y": 0
    })json")
                                .arg(id, name, typeLine)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), location));
}

void ItemsModelTest::testerSurvivesRebuildModeSwitchAndSort()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeModelItem("alpha-2", "Zulu Bite", "Vaal Axe", firstTab));
    items.push_back(makeModelItem("alpha-1", "Alpha Bite", "Copper Sword", firstTab));
    items.push_back(makeModelItem("beta-1", "Beta Guard", "Copper Shield", secondTab));

    QTreeView view;
    FilterCatalog catalog({});
    Search search(*buyoutFixture.manager, "Model", catalog);
    search.FilterItems(items);
    view.setModel(&search.model());

    auto *model = &search.model();
    QAbstractItemModelTester tester(model,
                                    QAbstractItemModelTester::FailureReportingMode::Fatal,
                                    this);

    QSignalSpy aboutToReset(model, &QAbstractItemModel::modelAboutToBeReset);
    QSignalSpy reset(model, &QAbstractItemModel::modelReset);
    search.SetRefreshReason(RefreshReason::SearchFormChanged);
    search.FilterItems(items);
    QCOMPARE(aboutToReset.count(), 1);
    QCOMPARE(reset.count(), 1);

    QSignalSpy dataChanged(model, &QAbstractItemModel::dataChanged);
    for (const auto &bucket : search.buckets()) {
        buyoutFixture.manager->SetRefreshChecked(bucket.location(), true);
    }
    model->refreshCheckStates();
    QCOMPARE(dataChanged.count(), 1);
    const auto dataChangedArguments = dataChanged.takeFirst();
    const auto roles = qvariant_cast<QList<int>>(dataChangedArguments.at(2));
    QCOMPARE(roles, QList<int>{Qt::CheckStateRole});

    aboutToReset.clear();
    reset.clear();
    search.SetViewMode(Search::ViewMode::ByItem);
    QCOMPARE(aboutToReset.count(), 1);
    QCOMPARE(reset.count(), 1);

    QStringList layoutEvents;
    QObject::connect(model,
                     &QAbstractItemModel::layoutAboutToBeChanged,
                     this,
                     [&](const QList<QPersistentModelIndex> &,
                         QAbstractItemModel::LayoutChangeHint) { layoutEvents.push_back("about"); });
    QObject::connect(model,
                     &QAbstractItemModel::layoutChanged,
                     this,
                     [&](const QList<QPersistentModelIndex> &,
                         QAbstractItemModel::LayoutChangeHint) {
                         layoutEvents.push_back("changed");
                     });
    QSignalSpy layoutAboutToChange(model, &QAbstractItemModel::layoutAboutToBeChanged);
    QSignalSpy layoutChanged(model, &QAbstractItemModel::layoutChanged);

    model->sort(0, Qt::AscendingOrder);
    QCOMPARE(layoutAboutToChange.count(), 1);
    QCOMPARE(layoutChanged.count(), 1);
    QCOMPARE(layoutEvents, QStringList({"about", "changed"}));
}

void ItemsModelTest::selectionSurvivesSort()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    buyoutFixture.manager->SetStashTabLocations({firstTab});

    Items items;
    items.push_back(makeModelItem("alpha-2", "Zulu Bite", "Vaal Axe", firstTab));
    items.push_back(makeModelItem("alpha-1", "Alpha Bite", "Copper Sword", firstTab));
    items.push_back(makeModelItem("alpha-3", "Mid Bite", "Iron Hammer", firstTab));
    items.push_back(makeModelItem("alpha-4", "Omega Bite", "Steel Dagger", firstTab));
    items.push_back(makeModelItem("alpha-5", "Beta Bite", "Bronze Mace", firstTab));

    QTreeView view;
    FilterCatalog catalog({});
    Search search(*buyoutFixture.manager, "Model", catalog);
    search.FilterItems(items);
    view.setModel(&search.model());

    auto *model = &search.model();

    const QModelIndex bucket0 = model->index(0, 0);
    QCOMPARE(model->rowCount(bucket0), 5);

    // Since S3, only materialized buckets sort (D2): mark the bucket
    // expanded the way the view's expand signal would, so the view-wide
    // sorts below reach it.
    model->OnBucketExpanded(0);

    // Sort one way first so the opposite re-sort below is guaranteed to
    // reorder rows regardless of fixture insertion order; a no-op sort would
    // make the remap assertion vacuous. (This test asserts only that the
    // arrangement changes; sortDirectionMatchesOrder pins which direction
    // each order enum produces.)
    model->sort(0, Qt::DescendingOrder);
    const QString first_before = model->index(0, 0, bucket0).data().toString();

    // Select rows 0-2 as one range, like a user shift-clicking three items.
    // The selection model tracks multi-row ranges with persistent indexes it
    // creates during layoutAboutToBeChanged; the model must remap them.
    const QItemSelection range(model->index(0, 0, bucket0), model->index(2, 0, bucket0));
    view.selectionModel()->select(range,
                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QStringList selected_before;
    for (const QModelIndex &idx : view.selectionModel()->selectedRows()) {
        selected_before << model->data(idx).toString();
    }
    selected_before.sort();
    QCOMPARE(selected_before.size(), 3);

    // Re-sort in the opposite order; the selection should follow the items.
    model->sort(0, Qt::AscendingOrder);
    QVERIFY(model->index(0, 0, bucket0).data().toString() != first_before);

    QStringList selected_after;
    for (const QModelIndex &idx : view.selectionModel()->selectedRows()) {
        selected_after << model->data(idx).toString();
    }
    selected_after.sort();
    QCOMPARE(selected_after, selected_before);
}

// F34: the Qt sort-order enums must produce the arrangement they name, so
// the header's sort indicator points the way the rows actually run.
void ItemsModelTest::sortDirectionMatchesOrder()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    buyoutFixture.manager->SetStashTabLocations({firstTab});

    Items items;
    items.push_back(makeModelItem("alpha-2", "Zulu Bite", "Vaal Axe", firstTab));
    items.push_back(makeModelItem("alpha-1", "Alpha Bite", "Copper Sword", firstTab));
    items.push_back(makeModelItem("alpha-3", "Mid Bite", "Iron Hammer", firstTab));

    FilterCatalog catalog({});
    Search search(*buyoutFixture.manager, "Model", catalog);
    search.FilterItems(items);
    auto *model = &search.model();
    const QModelIndex bucket0 = model->index(0, 0);
    QCOMPARE(model->rowCount(bucket0), 3);

    // Since S3, only materialized buckets sort (D2): materialize the
    // bucket as the view's expand signal would.
    model->OnBucketExpanded(0);

    const auto rowNames = [&]() {
        QStringList names;
        for (int row = 0; row < model->rowCount(bucket0); ++row) {
            names << model->index(row, 0, bucket0).data().toString();
        }
        return names;
    };

    model->sort(0, Qt::AscendingOrder);
    const QStringList ascending = rowNames();
    QStringList expected = ascending;
    std::sort(expected.begin(), expected.end());
    QCOMPARE(ascending, expected);

    model->sort(0, Qt::DescendingOrder);
    const QStringList descending = rowNames();
    std::reverse(expected.begin(), expected.end());
    QCOMPARE(descending, expected);
}

// Folder-tree D4: bucket row -> header index and model index -> (bucket row,
// item row) round-trip for every header and every item, in both view modes,
// and reject what is not this model's.
void ItemsModelTest::bucketRowConversionsRoundTrip()
{
    BuyoutManagerFixture buyoutFixture;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation emptyTab = makeTestStashLocation("stash-e", "Empty Tab", 1);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 2);
    buyoutFixture.manager->SetStashTabLocations({firstTab, emptyTab, secondTab});

    Items items;
    items.push_back(makeModelItem("alpha-1", "Alpha Bite", "Copper Sword", firstTab));
    items.push_back(makeModelItem("alpha-2", "Zulu Bite", "Vaal Axe", firstTab));
    items.push_back(makeModelItem("beta-1", "Beta Guard", "Copper Shield", secondTab));

    FilterCatalog catalog({});
    Search search(*buyoutFixture.manager, "Model", catalog);
    search.FilterItems(items);
    auto &model = search.model();

    const auto checkMode = [&]() {
        int headers = 0;
        int rows = 0;
        for (int bucket_row = 0; bucket_row < static_cast<int>(search.buckets().size());
             ++bucket_row) {
            const QModelIndex header = model.BucketIndex(bucket_row);
            QVERIFY(header.isValid());
            QCOMPARE(header.data().toString(), model.data(header).toString());
            const auto located = model.Locate(header);
            QCOMPARE(located.bucket_row, bucket_row);
            QCOMPARE(located.item_row, -1);
            QVERIFY(located.isBucket());
            QCOMPARE(model.BucketIndex(bucket_row, 2).column(), 2);
            QCOMPARE(model.BucketIndex(bucket_row, 2).row(), header.row());
            ++headers;
            for (int item_row = 0; item_row < model.rowCount(header); ++item_row) {
                const QModelIndex item = model.index(item_row, 1, header);
                const auto located_item = model.Locate(item);
                QCOMPARE(located_item.bucket_row, bucket_row);
                QCOMPARE(located_item.item_row, item_row);
                QVERIFY(located_item.isItem());
                ++rows;
            }
        }
        QCOMPARE(headers, static_cast<int>(search.buckets().size()));
        QCOMPARE(rows, static_cast<int>(search.items().size()));
        QVERIFY(!model.BucketIndex(-1).isValid());
        QVERIFY(!model.BucketIndex(static_cast<int>(search.buckets().size())).isValid());
        QCOMPARE(model.Locate(QModelIndex()).bucket_row, -1);
        QVERIFY(!model.Locate(QModelIndex()).isBucket());
        QVERIFY(!model.Locate(QModelIndex()).isItem());
    };
    checkMode();
    QCOMPARE(static_cast<int>(search.buckets().size()), 3);

    search.SetViewMode(Search::ViewMode::ByItem);
    checkMode();
    QCOMPARE(static_cast<int>(search.buckets().size()), 1);

    // An index from another model is never located.
    FilterCatalog other_catalog({});
    Search other(*buyoutFixture.manager, "Other", other_catalog);
    other.FilterItems(items);
    QCOMPARE(model.Locate(other.model().index(0, 0)).bucket_row, -1);
}

QTEST_MAIN(ItemsModelTest)

#include "tst_itemsmodel.moc"
