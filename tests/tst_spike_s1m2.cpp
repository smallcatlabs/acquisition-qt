// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// SPIKE(S1-M2): mechanical checks for the D9 prototype's intersection and
// restore machinery, reproducing hands-on observations from the harness.
// Throwaway with the branch.

#include <QtTest/QtTest>

#include <QLabel>
#include <QLineEdit>
#include <QTreeView>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "mainwindowfixture.h"
#include "spikedataset.h"

class SpikeS1M2Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void renameIntoTabFilterAppearsAtTick();
    void renameOutOfTabFilterDisappearsAtTick();
    void expansionSurvivesRenameOfExpandedTab();
    void selectionSurvivesChurnTick();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> m_sink_hub;
};

static QLineEdit *findFilterEdit(MainWindow &window, const QString &labelText)
{
    for (auto *label : window.findChildren<QLabel *>()) {
        if (label->text() != labelText) {
            continue;
        }
        auto *group = label->parentWidget();
        if (!group) {
            continue;
        }
        const auto edits = group->findChildren<QLineEdit *>();
        if (!edits.isEmpty()) {
            return edits.front();
        }
    }
    return nullptr;
}

static QStringList visibleBucketHeaders(const QTreeView &view)
{
    QStringList headers;
    const QAbstractItemModel *model = view.model();
    for (int row = 0; row < model->rowCount(); ++row) {
        headers.append(model->index(row, 0).data().toString());
    }
    return headers;
}

void SpikeS1M2Test::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    m_main_logger->sinks().push_back(m_sink_hub);
    spdlog::register_logger(m_main_logger);
}

void SpikeS1M2Test::cleanupTestCase()
{
    spdlog::drop("main");
    m_sink_hub.reset();
    m_main_logger.reset();
}

void SpikeS1M2Test::renameIntoTabFilterAppearsAtTick()
{
    MainWindowFixture fixture;
    SpikeDataset dataset({.tab_count = 10, .mean_items_per_tab = 5, .seed = 42});
    fixture.itemsManager->OnItemsRefreshed(dataset.allItems(), dataset.locations(), true);
    QObject::connect(fixture.itemsManager.get(),
                     &ItemsManager::TabRefreshed,
                     fixture.window.get(),
                     &MainWindow::OnTabRefreshed);

    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *tab_filter = findFilterEdit(*fixture.window, "Tab");
    QVERIFY(tree);
    QVERIFY(tab_filter);

    // Type the filter and let the 350 ms debounce land.
    tab_filter->setFocus();
    QTest::keyClicks(tab_filter, "renamed");
    QTest::qWait(500);
    QCOMPARE(visibleBucketHeaders(*tree).size(), 0);

    // Rename a tab the way the harness does, as one delta.
    fixture.window->SpikeSetThrottleInterval(100);
    dataset.RenameTab(3, "Renamed 3 (v1)");
    fixture.buyoutFixture.manager->SetStashTabLocations(dataset.locations());
    fixture.itemsManager->SpikeApplyTabDelta(dataset.location(3), dataset.tabItems(3));

    // The delta's items now match the Tab filter, so the throttle must arm
    // (intersection half A) ...
    QVERIFY(fixture.window->SpikeThrottlePending());

    // ... and the tick must surface the renamed tab.
    QTest::qWait(300);
    const QStringList headers = visibleBucketHeaders(*tree);
    QCOMPARE(headers.size(), 1);
    QVERIFY(headers.front().contains("Renamed 3"));
}

void SpikeS1M2Test::renameOutOfTabFilterDisappearsAtTick()
{
    MainWindowFixture fixture;
    SpikeDataset dataset({.tab_count = 10, .mean_items_per_tab = 5, .seed = 42});
    dataset.RenameTab(3, "Renamed 3 (v1)");
    fixture.itemsManager->OnItemsRefreshed(dataset.allItems(), dataset.locations(), true);
    QObject::connect(fixture.itemsManager.get(),
                     &ItemsManager::TabRefreshed,
                     fixture.window.get(),
                     &MainWindow::OnTabRefreshed);

    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *tab_filter = findFilterEdit(*fixture.window, "Tab");
    QVERIFY(tree);
    QVERIFY(tab_filter);

    tab_filter->setFocus();
    QTest::keyClicks(tab_filter, "renamed");
    QTest::qWait(500);
    QCOMPARE(visibleBucketHeaders(*tree).size(), 1);

    // Rename it away from the filter: no delta item matches, but the visible
    // result came from this fetch source, so the removal half must arm.
    fixture.window->SpikeSetThrottleInterval(100);
    dataset.RenameTab(3, "Ordinary 3");
    fixture.buyoutFixture.manager->SetStashTabLocations(dataset.locations());
    fixture.itemsManager->SpikeApplyTabDelta(dataset.location(3), dataset.tabItems(3));

    QVERIFY(fixture.window->SpikeThrottlePending());
    QTest::qWait(300);
    QCOMPARE(visibleBucketHeaders(*tree).size(), 0);
}

// Post-freeze review finding 3: the hands-on rename check picks a random
// tab, which almost never hits one the operator expanded — this scenario
// deliberately renames THE expanded tab and requires its expansion to
// survive the throttled tick (R6-3 (1), stable keys).
void SpikeS1M2Test::expansionSurvivesRenameOfExpandedTab()
{
    MainWindowFixture fixture;
    SpikeDataset dataset({.tab_count = 10, .mean_items_per_tab = 5, .seed = 42});
    fixture.itemsManager->OnItemsRefreshed(dataset.allItems(), dataset.locations(), true);
    QObject::connect(fixture.itemsManager.get(),
                     &ItemsManager::TabRefreshed,
                     fixture.window.get(),
                     &MainWindow::OnTabRefreshed);

    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);
    QAbstractItemModel *model = tree->model();
    QVERIFY(model);

    // Unfiltered search: all tabs are buckets. Expand tab 3's bucket.
    const QString oldName = dataset.tabName(3);
    int bucketRow = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString().contains(oldName)) {
            bucketRow = row;
            break;
        }
    }
    QVERIFY(bucketRow >= 0);
    tree->expand(model->index(bucketRow, 0));
    QVERIFY(tree->isExpanded(model->index(bucketRow, 0)));

    // Rename that same tab and deliver it as a delta.
    fixture.window->SpikeSetThrottleInterval(100);
    dataset.RenameTab(3, "Freshly Renamed 3");
    fixture.buyoutFixture.manager->SetStashTabLocations(dataset.locations());
    fixture.itemsManager->SpikeApplyTabDelta(dataset.location(3), dataset.tabItems(3));
    QVERIFY(fixture.window->SpikeThrottlePending());
    QTest::qWait(300);

    // The bucket renders the new name and is still expanded.
    int renamedRow = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString().contains("Freshly Renamed 3")) {
            renamedRow = row;
            break;
        }
    }
    QVERIFY(renamedRow >= 0);
    QVERIFY(tree->isExpanded(model->index(renamedRow, 0)));
}

// Post-freeze review finding 3: selection survival across a churn tick was
// hands-judged but not automated. A delta replaces every Item object in the
// selected item's tab (same stable ids, new shared_ptrs); the selection
// must survive by stable identity (R6-3 (2)).
void SpikeS1M2Test::selectionSurvivesChurnTick()
{
    MainWindowFixture fixture;
    SpikeDataset dataset({.tab_count = 10, .mean_items_per_tab = 5, .seed = 42});
    fixture.itemsManager->OnItemsRefreshed(dataset.allItems(), dataset.locations(), true);
    QObject::connect(fixture.itemsManager.get(),
                     &ItemsManager::TabRefreshed,
                     fixture.window.get(),
                     &MainWindow::OnTabRefreshed);

    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);
    QAbstractItemModel *model = tree->model();
    QVERIFY(model);

    // Select the first item of tab 3's bucket.
    const QString tabName = dataset.tabName(3);
    int bucketRow = -1;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, 0).data().toString().contains(tabName)) {
            bucketRow = row;
            break;
        }
    }
    QVERIFY(bucketRow >= 0);
    const QModelIndex bucket = model->index(bucketRow, 0);
    tree->expand(bucket);
    const QModelIndex item = model->index(0, 0, bucket);
    QVERIFY(item.isValid());
    const QString itemName = item.data().toString();
    tree->selectionModel()->setCurrentIndex(item,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    // tabItems() materializes NEW Item objects with the same stable ids —
    // exactly what an M2 delta does to pointer identity.
    fixture.window->SpikeSetThrottleInterval(100);
    fixture.itemsManager->SpikeApplyTabDelta(dataset.location(3), dataset.tabItems(3));
    QVERIFY(fixture.window->SpikeThrottlePending());
    QTest::qWait(300);

    const QModelIndexList selected = tree->selectionModel()->selectedRows();
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected.front().data().toString(), itemName);
}

QTEST_MAIN(SpikeS1M2Test)

#include "tst_spike_s1m2.moc"
