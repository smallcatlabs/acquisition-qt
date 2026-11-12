// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QAbstractItemModel>
#include <QAbstractItemModelTester>
#include <QAction>
#include <QComboBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollBar>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QTreeView>

#include <memory>
#include <random>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "buyout.h"
#include "currency.h"
#include "mainwindowfixture.h"
#include "modelprobes.h"

class MainWindowTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void fixtureConstructsOffline();
    void legacyImportPropagationRefreshesPricingState();
    // #257: nothing writes buyouts while they are not loaded.
    void legacyImportRefusedWhenBuyoutsNotLoaded();
    void startupWarnsWhenBuyoutsNotLoaded();
    void refusedPriceEditSaysSoInStatusLine();
    void savedPriceEditClearsNotSavedNotice();
    void tabChangeActivatesSelectedSearch();
    // Renegotiated in M3 S6 from `itemsRefreshRefiltersBackgroundSearches`
    // (the F33-era eager background refilter rode the final-snapshot reset
    // seam): the snapshot now flags background searches items-dirty and
    // their own activation refilters (rule 1), so F33's freshness guarantee
    // holds exactly where the user can observe it.
    void itemsRefreshDefersBackgroundSearchesToActivation();
    void pendingEditFollowsOutgoingSearch();
    void deleteTabDance();
    void currentViewStatePins();

    // Items-pipeline M2, stage 3, post-S5: D6 stable-identity bucketing
    // and the surviving delta-consumer pins. Deltas apply immediately to
    // the active search in both view modes (M3 D3/D4). The D9 timer pins
    // died with the timer in S5 per the supersession map:
    // `throttleDoesNotRearm` (→ `staleOrderNeverSurvivesDelta`),
    // `tabSwitchBeforeTickPreservesDirty` (→
    // `appliedDeltasLeaveActiveSearchClean` +
    // `backgroundDeltaLeavesModelUntouched`),
    // `finalSnapshotCancelsPendingTick` and
    // `pendingTickSurvivesTerminalFailure` (→ the S6 reconciliation pins,
    // `metadataDeltaAppliesWithoutItemIntersection`,
    // `selectionIntentClearsOnTerminalFailure`), and the no-successor
    // pair `searchDeleteCancelsPendingTimer` /
    // `successfulRefilterCancelsPendingTick` (hazard gone with the
    // timer; residual interleavings go to `modelTesterPassesUnderDeltaStorm`).
    // `emptyDeltaMetadataLandsAtNextRefilter` was deleted in S4: R1-4
    // retired the M2 R7-2 exception it encoded — successor
    // `metadataDeltaAppliesWithoutItemIntersection`.
    void bucketsKeyOnStableIdDuringRefresh();
    void backgroundDeltaLeavesModelUntouched();
    void removalOnlyDeltaIntersects();
    void childReconciliationIntersectsVisibleGhosts();

    // Items-pipeline M2, stage 5, post-S5: the R6-3 restore-fidelity
    // contract. The rename and replacement pins assert the no-reset delta
    // path; `scrollAndCaptureSurviveThrottledReset` died with the
    // throttled reset in S5 and the capture/restore machinery is
    // retargeted to D6's user-refilter reset as its named replacement
    // (supersession map).
    // `reselectionSurvivesCrossTabMove` was deleted in S4 — its named
    // successor `selectionIntentSurvivesCrossTabMoveAcrossDeltas` lands
    // in the same commit window (supersession map; never red).
    void expansionSurvivesRenameByStableKey();
    void selectionSurvivesReplacementByStableIdentity();
    void scrollAndCaptureSurviveUserRefilter();

    // Items-pipeline M3, S0: the capture/restore half of the probe surface.
    void probeCountersTrackCaptureRestore();

    // Items-pipeline M3, S2: the buyout choke point and the five batching
    // rules (D1 rule 4, R1-6/R2-5/R3-3/R3-4). priceKeysFollowBuyoutEdits
    // closed its behavioral half in S2 (reorder at batch end) and
    // completed in S3 with its R3-2 resident-key assertions.
    void priceKeysFollowBuyoutEdits();
    void multiSelectionBuyoutEditReordersOnce();
    void pricingPassYieldsSingleModelUpdate();
    void snapshotPricingSequenceEmitsOneModelBatch();
    void priceCellsRepaintUnderAnySortColumn();
    void buyoutRepaintCoversEveryVisibleOccurrence();

    // Items-pipeline M3, S3: D2 deferred sorting (per-bucket flags, the
    // full transition table) and D1 key residency (hydration, eviction,
    // the invalidation contract). residentKeysScopedToActiveSearch
    // completed in S5 with its clean-By-Item eager-hydration clause
    // (R3-1 carve-out).
    void collapsedBucketsDeferSorting();
    void restoredExpansionSortsRestoredBucketsOnly();
    void filteredSearchSortsAllVisibleBuckets();
    void sortedOrderSurvivesCollapse();
    void keyResidencyFollowsMaterialization();
    void reexpandedBucketFlipHydratesOnce();
    void sortColumnSwitchResortsVisibleBucketsOnly();
    void residentKeysScopedToActiveSearch();
    void buyoutReorderScopedToAffectedMaterializedBuckets();

    // Items-pipeline M3, S4: D3's bucket-scoped delta operations, the
    // R1-4 metadata half, R1-7's renegotiated dirty flag (By-Tab half),
    // and the R1-3/R2-1 selection-intent contract.
    void unrelatedDeltaLeavesOtherBucketsUntouched();
    void deltaReplacesExactlyItsSourceRows();
    void childDeltaPreservesSiblingSourcesInParentBucket();
    void emptyDeltaEmptiesBucketWithoutRemovingIt();
    void deltaUpdatesVisibleIndexesIncrementally();
    void bucketRepositionsByMoveOnMetadataDelta();
    void metadataDeltaAppliesWithoutItemIntersection();
    void collapsedInvalidBucketResortsOnReexpand();
    void staleOrderNeverSurvivesDelta();
    void selectionIntentSurvivesCrossTabMoveAcrossDeltas();
    void selectionIntentClearsOnTerminalFailure();
    void appliedDeltasLeaveActiveSearchClean();

    // Items-pipeline M3, S4 review round 1 (permanent).
    // `selectionIntentCoversByItemFallback` and
    // `modeSwitchConsumesFallbackDirtiness` were fallback-scoped and were
    // renegotiated when the seam died in S5: intent-over-By-Item is
    // covered by `byItemSelectionSurvivesMerge`, and the mode-switch
    // coherence contract became `modeSwitchRendersAppliedDeltas`.
    void filteredSearchDropsEmptiedBucket();
    void metadataDeltaRefreshesSelectedPresentation();

    // Items-pipeline M3, S5: D4's flat-bucket per-delta contract, the
    // R3-1 eager activation carve-out, and the D9 throttle's full
    // retirement. appliedDeltasLeaveActiveSearchClean (above) closed its
    // By-Item half here too.
    void byItemMergeMatchesFullSort();
    void byItemRemovalOnlyDeltaErasesInPlace();
    void byItemSelectionSurvivesMerge();
    void byItemActivationDecidesDirtinessFirst();
    void modeSwitchRendersAppliedDeltas();
    // S5 review round 1: rule-5 repaint scoped to affected runs.
    void buyoutRepaintScopesToAffectedRuns();
    // S5 remedy A′ gate: the flat replace under the Qt model tester.
    void byItemReplaceSatisfiesModelTester();

    // Items-pipeline M3, S6: the R1-2 final row reconciliation and the
    // deletion of the last reset seam. The revalidation clauses inside
    // `selectionIntentSurvivesCrossTabMoveAcrossDeltas`,
    // `byItemSelectionSurvivesMerge`, and
    // `appliedDeltasLeaveActiveSearchClean` were re-proved
    // fallback-insensitively here (probe: reset count zero,
    // final_reconciliations entered) — merely staying green proved
    // nothing while the final-reset fallback also passed them.
    void noModelResetDuringRefresh();
    void finalReconciliationRemovesDeletedTabs();
    void finalReconciliationInsertsNewlyListedEmptyTabs();
    void modelTesterPassesUnderDeltaStorm();

    // Items-pipeline M3, S7 review round 1 (finding 2): the delta-path
    // column resize is a non-resetting debounce.
    void deltaResizeDebounceCoalescesBurst();

    // UI papercuts (November 2026).
    void refreshIntervalDialogEditsCurrentValue();
    void refreshIntervalDialogCancelChangesNothing();
    void activeThemeStaysCheckedWhenClicked();
    void activeLoggingLevelStaysCheckedWhenClicked();
    void priceBoxIgnoresTextThatIsNotAPrice_data();
    void priceBoxIgnoresTextThatIsNotAPrice();
    void priceBoxAcceptsDecimalPointOrComma_data();
    void priceBoxAcceptsDecimalPointOrComma();
    void removedSelectionResetsPriceEditor();
    void byItemHeaderIsNotPriceable();
    void priceEditSkipsGamePricedRows();
    void typedPriceCommitsOnceToMultiSelection();
    void clickOnAnotherRowCommitsToTheRowsTypedFor();
    void gamePricedRowsLeftAloneAreNamedInStatusLine();
    void refreshOfSelectedItemsTabKeepsTypedPrice();
    void renameOfSelectedTabKeepsTypedPrice();
    void searchTabClickCommitsToTheRowsTypedFor();
    void newSearchClickCommitsToTheRowsTypedFor();
    void viewModeSwitchCommitsToTheRowsTypedFor();
    void closingTheSearchCommitsToTheRowsTypedFor();
    void refreshRunsNoCommitWhileTyping();
    void typedPriceForGoneRowSaysSo();
    void refreshCollapsingMultiSelectionPricesTheTypedRows();
    void refreshReselectingDeselectedRowPricesTheTypedRows();
    void refilterWhileTypingPricesTheTypedRow();
    void messageBoxWhileTypingCommitsNothing();
    void typedValueThenCurrencyCommitsOnceAcrossEventLoop();
    void invalidTextOnEnterSaysWhy();
    void invalidTextOnTargetChangeSaysWhy();
    void headerSelectionSurvivesItemDetailsTimer();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> m_sink_hub;
};

static std::shared_ptr<Item> makeMainWindowItem(const QString &id,
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

static std::shared_ptr<Item> makeMainWindowItemWithNote(const QString &id,
                                                        const QString &name,
                                                        const QString &typeLine,
                                                        const ItemLocation &location,
                                                        const QString &note)
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
        "note": "%4",
        "typeLine": "%3",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0
    })json")
                                .arg(id, name, typeLine, note)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), location));
}

static QTabBar *findSearchTabs(MainWindow &window)
{
    for (auto *tabs : window.findChildren<QTabBar *>()) {
        if ((tabs->count() > 0) && (tabs->tabText(tabs->count() - 1) == "+")) {
            return tabs;
        }
    }
    return nullptr;
}

static QLineEdit *findNameFilter(MainWindow &window)
{
    for (auto *label : window.findChildren<QLabel *>()) {
        if (label->text() != "Name") {
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

static QStringList visibleItemNames(const QTreeView &view)
{
    const QAbstractItemModel *model = view.model();
    if (!model) {
        qFatal("visibleItemNames: the tree view has no model");
    }

    QStringList names;
    for (int bucketRow = 0; bucketRow < model->rowCount(); ++bucketRow) {
        const QModelIndex bucket = model->index(bucketRow, 0);
        for (int itemRow = 0; itemRow < model->rowCount(bucket); ++itemRow) {
            names.append(model->index(itemRow, 0, bucket).data().toString());
        }
    }
    return names;
}

static QModelIndex findBucket(const QAbstractItemModel &model, const QString &header)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex bucket = model.index(row, 0);
        if (bucket.data().toString().startsWith(header)) {
            return bucket;
        }
    }
    return QModelIndex();
}

static QStringList bucketItemNames(const QAbstractItemModel &model, const QModelIndex &bucket)
{
    QStringList names;
    for (int row = 0; row < model.rowCount(bucket); ++row) {
        names.append(model.index(row, 0, bucket).data().toString());
    }
    return names;
}

static QModelIndex findItemRow(const QAbstractItemModel &model,
                               const QModelIndex &bucket,
                               const QString &name)
{
    for (int row = 0; row < model.rowCount(bucket); ++row) {
        const QModelIndex index = model.index(row, 0, bucket);
        if (index.data().toString() == name) {
            return index;
        }
    }
    return QModelIndex();
}

// Drives the view-mode combo the way a user would (S5: the By-Item pins
// exercise the D4 flat-bucket path through the real mode-switch entry).
static void switchToByItemView(MainWindowFixture &fixture)
{
    auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
    if (!viewCombo) {
        qFatal("switchToByItemView: viewComboBox not found");
    }
    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);
}

// One user buyout command (M3 R2-5): the widget states a user would leave
// behind, then the command boundary itself: the type box's activated(), as
// when the user picks a type. setCurrentIndex/setText emit no activated or
// editingFinished, so this commits exactly once — the command applies to
// whatever the tree's selection model holds.
static void applyBuyoutCommand(MainWindowFixture &fixture,
                               int typeIndex,
                               int currencyIndex,
                               const QString &value)
{
    auto *buyoutType = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *buyoutCurrency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    auto *buyoutValue = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(buyoutType);
    QVERIFY(buyoutCurrency);
    QVERIFY(buyoutValue);
    buyoutType->setCurrentIndex(typeIndex);
    buyoutCurrency->setCurrentIndex(currencyIndex);
    buyoutValue->setText(value);
    emit buyoutType->activated(typeIndex);
}

void MainWindowTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // LogPanel attaches its sinks through the dist-sink hub that
    // logging::init installs on the main logger (F42), so the test logger
    // needs one too.
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    m_main_logger->sinks().push_back(m_sink_hub);
    spdlog::register_logger(m_main_logger);
}

void MainWindowTest::cleanupTestCase()
{
    QCOMPARE(static_cast<int>(m_sink_hub->sinks().size()), 0);
    spdlog::drop("main");
    m_sink_hub.reset();
    m_main_logger.reset();
}

void MainWindowTest::fixtureConstructsOffline()
{
    QCOMPARE(static_cast<int>(m_sink_hub->sinks().size()), 0);
    {
        MainWindowFixture fixture;
        QVERIFY(fixture.window);
        const auto *import_action = fixture.window->findChild<QAction *>(
            "actionImportLegacyBuyouts");
        QVERIFY(import_action);
        QCOMPARE(import_action->text(), QString("Recover legacy buyouts…"));
        const auto *plan_action = fixture.window->findChild<QAction *>(
            "actionImportLegacyBuyoutPlan");
        QVERIFY(plan_action);
        QCOMPARE(plan_action->text(), QString("Import buyout plan…"));
        QCOMPARE(static_cast<int>(m_sink_hub->sinks().size()), 2);
    }
    QCOMPARE(static_cast<int>(m_sink_hub->sinks().size()), 0);
}

void MainWindowTest::legacyImportPropagationRefreshesPricingState()
{
    MainWindowFixture fixture;
    const ItemLocation tab = makeTestStashLocation("priced-tab", "Priced", 0);
    const auto item = std::make_shared<Item>(makeTestItem(R"json({
        "verified":true,"w":1,"h":1,"icon":"","id":"propagated-item",
        "name":"","typeLine":"Test Sword","baseType":"Test Sword",
        "identified":true,"ilvl":1,"x":0,"y":0
    })json",
                                                          tab));
    Items items;
    items.push_back(item);
    fixture.itemsManager->OnItemsRefreshed(items, {tab}, false);

    const Buyout imported_tab = makeChaosBuyout(12.0);
    QCOMPARE(fixture.buyoutFixture.repo->saveLocationBuyout(imported_tab, tab.id(), tab.type(), true),
             BuyoutSaveResult::Saved);
    int change_count = 0;
    bool everything_changed = false;
    connect(fixture.buyoutFixture.manager.get(),
            &BuyoutManager::BuyoutsChanged,
            this,
            [&](const BuyoutChangeSet &changes) {
                ++change_count;
                everything_changed = changes.everything;
            });

    QVERIFY(QMetaObject::invokeMethod(fixture.window.get(),
                                      "OnLegacyBuyoutsImported",
                                      Qt::DirectConnection));

    QCOMPARE(change_count, 1);
    QVERIFY(everything_changed);
    QCOMPARE(fixture.buyoutFixture.manager->GetTab(tab).value, 12.0);
    const Buyout propagated = fixture.buyoutFixture.manager->Get(*item);
    QCOMPARE(propagated.value, 12.0);
    QVERIFY(propagated.inherited);
    QVERIFY(fixture.buyoutFixture.manager->GetRefreshLocked(tab));
    QVERIFY(fixture.shop->shop_data_outdated());
}

// Leaves the fixture's manager in the state a failed item read leaves it
// in at start-up (#257): the reload runs with the item table hidden.
static void unloadBuyouts(MainWindowFixture &fixture)
{
    QSqlQuery q(*fixture.buyoutFixture.db);
    QVERIFY(q.exec("ALTER TABLE item_buyouts RENAME TO item_buyouts_hidden"));
    fixture.buyoutFixture.manager->ReloadBuyouts();
    QVERIFY(q.exec("ALTER TABLE item_buyouts_hidden RENAME TO item_buyouts"));
    QVERIFY(!fixture.buyoutFixture.manager->BuyoutsLoaded());
}

// The text of the next modal message box, which is then closed.
static void closeNextMessageBox(QString &text)
{
    QTimer::singleShot(0, [&text] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (box) {
            text = box->text();
            box->done(QMessageBox::Cancel);
        }
    });
}

// #257: both import actions write buyouts, so with the buyouts not
// loaded they refuse before asking for a file.
void MainWindowTest::legacyImportRefusedWhenBuyoutsNotLoaded()
{
    MainWindowFixture fixture;
    unloadBuyouts(fixture);

    for (const char *action : {"OnImportLegacyBuyouts", "OnImportLegacyBuyoutPlan"}) {
        QString text;
        closeNextMessageBox(text);
        QVERIFY(QMetaObject::invokeMethod(fixture.window.get(), action, Qt::DirectConnection));
        QVERIFY2(text.contains("cannot save prices in this session"), qPrintable(text));
    }
}

// #257: a session that starts without its prices says so once, with the
// log's location, instead of leaving the user to find out from the shop.
void MainWindowTest::startupWarnsWhenBuyoutsNotLoaded()
{
    MainWindowFixture fixture;
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->findChildren<QMessageBox *>().isEmpty());

    unloadBuyouts(fixture);
    MainWindow window(*fixture.settings,
                      *fixture.networkManager,
                      *fixture.rateLimiter,
                      *fixture.itemsManager,
                      *fixture.buyoutFixture.manager,
                      *fixture.buyoutFixture.repo,
                      *fixture.stashRepo,
                      *fixture.characterRepo,
                      *fixture.currencyManager,
                      *fixture.shop,
                      *fixture.imageCache,
                      QDir(fixture.tempDir.path()));
    QCoreApplication::processEvents();

    const auto boxes = window.findChildren<QMessageBox *>();
    QCOMPARE(boxes.size(), 1);
    // True however BuyoutsLoaded() became false: a store written by a newer
    // build loads and shows its prices, it only cannot be written.
    QVERIFY2(boxes.front()->text().contains("cannot save prices in this session"),
             qPrintable(boxes.front()->text()));
    QVERIFY(!boxes.front()->text().contains("could not load"));
    const QString log_path = QDir::toNativeSeparators(
        QDir(fixture.tempDir.path()).filePath("log.txt"));
    QVERIFY2(boxes.front()->informativeText().contains(log_path),
             qPrintable(boxes.front()->informativeText()));
}

// One item in one tab, refreshed and selected in the tree.
static void selectOneItem(MainWindowFixture &fixture)
{
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);
    const ItemLocation tab = makeTestStashLocation("stash-257", "Dump", 0);
    fixture.itemsManager->OnItemsRefreshed({makeMainWindowItem("item-257", "Alpha", "Sword", tab)},
                                           {tab},
                                           false);
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tab.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "Alpha Sword"),
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
}

// Everything the status bar shows or holds: its temporary message and the
// text of its labels.
static QString statusText(MainWindowFixture &fixture)
{
    QStringList texts{fixture.window->statusBar()->currentMessage()};
    for (const QLabel *label : fixture.window->statusBar()->findChildren<QLabel *>()) {
        texts.append(label->text());
    }
    return texts.join('|');
}

// #257: a price edit the manager refused must not look as if it worked.
// The refusal itself is logged only at debug level, so the notice names
// the session-wide cause instead of pointing at the log.
void MainWindowTest::refusedPriceEditSaysSoInStatusLine()
{
    MainWindowFixture fixture;
    selectOneItem(fixture);

    unloadBuyouts(fixture);
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");

    const QString status = statusText(fixture);
    QVERIFY2(status.contains("Price not saved"), qPrintable(status));
    QVERIFY2(status.contains("cannot be saved in this session"), qPrintable(status));
    QVERIFY2(!status.contains("log file"), qPrintable(status));
    QVERIFY(!fixture.buyoutFixture.repo->loadItemBuyouts().value().contains("item-257"));
}

// #257: after a save the database rejected (the repo logs it as an error),
// the next edit that is saved clears the notice.
void MainWindowTest::savedPriceEditClearsNotSavedNotice()
{
    MainWindowFixture fixture;
    selectOneItem(fixture);

    QSqlQuery q(*fixture.buyoutFixture.db);
    QVERIFY(q.exec("CREATE TRIGGER block_item_insert BEFORE INSERT ON item_buyouts "
                   "BEGIN SELECT RAISE(FAIL, 'test: insert blocked'); END"));
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");
    QString status = statusText(fixture);
    QVERIFY2(status.contains("Price not saved"), qPrintable(status));
    QVERIFY2(status.contains("log file"), qPrintable(status));

    QVERIFY(q.exec("DROP TRIGGER block_item_insert"));
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "8");
    status = statusText(fixture);
    QVERIFY2(!status.contains("Price not saved"), qPrintable(status));
    QCOMPARE(fixture.buyoutFixture.repo->loadItemBuyouts().value().at("item-257").value, 8.0);
}

void MainWindowTest::tabChangeActivatesSelectedSearch()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tabs);
    QVERIFY(tree);
    QVERIFY(name);
    QCOMPARE(tabs->currentIndex(), 0);
    QSignalSpy nameEdited(name, &QLineEdit::textEdited);

    const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation betaTab = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("alpha-item", "Alpha", "Sword", alphaTab));
    items.push_back(makeMainWindowItem("beta-item", "Beta", "Shield", betaTab));
    fixture.itemsManager->OnItemsRefreshed(items, {alphaTab, betaTab}, false);

    name->setFocus();
    QTest::keyClicks(name, "alpha");
    QCOMPARE(name->text(), "alpha");
    QCOMPARE(nameEdited.count(), 5);
    // The tab switch deterministically flushes the debounced edit before the
    // form is rebound to Search 2, and the flushed caption lands on the
    // outgoing search's own tab (F41).
    tabs->setCurrentIndex(1);
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
    QCOMPARE(tabs->tabText(1), "Search 2 [2]");

    QTest::keyClicks(name, "beta");
    tabs->setCurrentIndex(0);
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
    QCOMPARE(name->text(), "alpha");
    QCOMPARE(visibleItemNames(*tree), QStringList({"Alpha Sword"}));

    tabs->setCurrentIndex(1);
    QCOMPARE(tabs->tabText(1), "Search 2 [1]");
    QCOMPARE(name->text(), "beta");
    QCOMPARE(visibleItemNames(*tree), QStringList({"Beta Shield"}));
}

void MainWindowTest::itemsRefreshDefersBackgroundSearchesToActivation()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tabs);
    QVERIFY(name);

    const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation betaTab = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items initialItems;
    initialItems.push_back(makeMainWindowItem("alpha-one", "Alpha One", "Sword", alphaTab));
    initialItems.push_back(makeMainWindowItem("beta-one", "Beta One", "Shield", betaTab));
    fixture.itemsManager->OnItemsRefreshed(initialItems, {alphaTab, betaTab}, false);

    name->setFocus();
    QTest::keyClicks(name, "alpha");
    tabs->setCurrentIndex(1);

    Items changedItems = initialItems;
    changedItems.push_back(makeMainWindowItem("alpha-two", "Alpha Two", "Axe", alphaTab));
    fixture.itemsManager->OnItemsRefreshed(changedItems, {alphaTab, betaTab}, false);

    // S6 (R1-2/R1-7): the snapshot no longer refilters background
    // searches eagerly — Search 1 keeps its last-rendered caption while
    // the active search reconciles.
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
    QCOMPARE(tabs->tabText(1), "Search 2 [3]");

    // F33's guarantee is preserved where the user can see it: activation
    // consumes the items-dirty flag and refilters (rule 1), so the search
    // is fresh the moment it is shown.
    tabs->setCurrentIndex(0);
    QCOMPARE(tabs->tabText(0), "Search 1 [2]");
}

void MainWindowTest::pendingEditFollowsOutgoingSearch()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tabs);
    QVERIFY(name);

    const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation betaTab = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("alpha-item", "Alpha", "Sword", alphaTab));
    items.push_back(makeMainWindowItem("beta-item", "Beta", "Shield", betaTab));
    fixture.itemsManager->OnItemsRefreshed(items, {alphaTab, betaTab}, false);

    name->setFocus();
    QTest::keyClicks(name, "alpha");
    // No elapsed-time wait: OnTabChange synchronously flushes the 350ms
    // debounce while Search 1 is still the outgoing search, and the flushed
    // caption targets Search 1's own tab (F41).
    tabs->setCurrentIndex(1);
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
    tabs->setCurrentIndex(0);

    // Re-activating renders the saved caption through the widget tree.
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
}

void MainWindowTest::deleteTabDance()
{
    {
        MainWindowFixture fixture;
        auto *tabs = findSearchTabs(*fixture.window);
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);

        const QRect onlySearch = tabs->tabRect(0);
        QVERIFY(onlySearch.isValid());
        QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier, onlySearch.center());

        QCOMPARE(tabs->count(), 2);
        QVERIFY(tabs->tabText(0).startsWith("Search 2"));
        QCOMPARE(tabs->tabText(1), "+");
    }

    {
        MainWindowFixture fixture;
        auto *tabs = findSearchTabs(*fixture.window);
        QVERIFY(tabs);

        tabs->setCurrentIndex(1);
        tabs->setCurrentIndex(2);
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->currentIndex(), 2);

        const QRect currentSearch = tabs->tabRect(2);
        QVERIFY(currentSearch.isValid());
        QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier, currentSearch.center());

        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(tabs->tabText(1).startsWith("Search 2"));
    }
}

void MainWindowTest::currentViewStatePins()
{
    {
        MainWindowFixture fixture;
        auto *tabs = findSearchTabs(*fixture.window);
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        auto *name = findNameFilter(*fixture.window);
        QVERIFY(tabs);
        QVERIFY(tree);
        QVERIFY(name);

        const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
        const ItemLocation betaTab = makeTestStashLocation("stash-beta", "Beta Tab", 1);
        Items items;
        items.push_back(makeMainWindowItem("alpha-item", "Alpha", "Sword", alphaTab));
        items.push_back(makeMainWindowItem("beta-item", "Beta", "Shield", betaTab));
        fixture.itemsManager->OnItemsRefreshed(items, {alphaTab, betaTab}, false);

        const QModelIndex bucket = findBucket(*tree->model(), alphaTab.GetHeader());
        QVERIFY(bucket.isValid());
        tree->expand(bucket);
        QVERIFY(tree->isExpanded(bucket));
        const QModelIndex item = tree->model()->index(0, 0, bucket);
        QVERIFY(item.isValid());
        tree->selectionModel()->setCurrentIndex(item,
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);

        tabs->setCurrentIndex(1);
        name->setFocus();
        QTest::keyClicks(name, "beta");
        tabs->setCurrentIndex(0);
        const QModelIndex restoredBucket = findBucket(*tree->model(), alphaTab.GetHeader());
        QVERIFY(restoredBucket.isValid());
        QVERIFY(tree->isExpanded(restoredBucket));
        const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
        QCOMPARE(selectedRows.size(), 1);
        QCOMPARE(selectedRows.front().data().toString(), "Alpha Sword");
    }

    {
        MainWindowFixture fixture;
        auto *tabs = findSearchTabs(*fixture.window);
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        auto *name = findNameFilter(*fixture.window);
        auto *nameLabel = fixture.window->findChild<QLabel *>("nameLabel");
        auto *buyoutType = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
        auto *buyoutCurrency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
        auto *buyoutValue = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
        QVERIFY(tabs);
        QVERIFY(tree);
        QVERIFY(name);
        QVERIFY(nameLabel);
        QVERIFY(buyoutType);
        QVERIFY(buyoutCurrency);
        QVERIFY(buyoutValue);

        const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
        const ItemLocation betaTab = makeTestStashLocation("stash-beta", "Beta Tab", 1);
        Items items;
        items.push_back(makeMainWindowItem("alpha-item", "Alpha", "Sword", alphaTab));
        items.push_back(makeMainWindowItem("beta-item", "Beta", "Shield", betaTab));
        fixture.itemsManager->OnItemsRefreshed(items, {alphaTab, betaTab}, false);

        name->setFocus();
        QTest::keyClicks(name, "alpha");
        tabs->setCurrentIndex(1);
        QTest::keyClicks(name, "beta");
        tabs->setCurrentIndex(0);
        fixture.buyoutFixture.manager->SetTab(alphaTab, makeChaosBuyout(7));

        const QModelIndex alphaBucket = findBucket(*tree->model(), alphaTab.GetHeader());
        QVERIFY(alphaBucket.isValid());
        tree->selectionModel()->setCurrentIndex(alphaBucket,
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        QCOMPARE(nameLabel->text(), alphaTab.GetHeader());
        QCOMPARE(buyoutValue->text(), "7");

        tabs->setCurrentIndex(1);
        QVERIFY(!findBucket(*tree->model(), alphaTab.GetHeader()).isValid());
        QVERIFY(findBucket(*tree->model(), betaTab.GetHeader()).isValid());
        QCOMPARE(nameLabel->text(), "Select an item");
        QCOMPARE(buyoutType->currentIndex(), static_cast<int>(Buyout::BUYOUT_TYPE_INHERIT));
        QCOMPARE(buyoutCurrency->currentIndex(), static_cast<int>(Currency::CURRENCY_NONE));
        QCOMPARE(buyoutValue->text(), "");
        QVERIFY(!buyoutType->isEnabled());
        QVERIFY(!buyoutCurrency->isEnabled());
        QVERIFY(!buyoutValue->isEnabled());

        tabs->setCurrentIndex(0);
        QCOMPARE(nameLabel->text(), alphaTab.GetHeader());
        QCOMPARE(buyoutValue->text(), "7");
        // The restored bucket is highlighted in the tree, not just named in
        // the panel (F43).
        const QModelIndex restoredBucket = findBucket(*tree->model(), alphaTab.GetHeader());
        QVERIFY(restoredBucket.isValid());
        const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
        QCOMPARE(selectedRows.size(), 1);
        QCOMPARE(selectedRows.front(), restoredBucket);
    }

    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        QVERIFY(tree);

        const ItemLocation alphaTab = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
        Items items;
        items.push_back(makeMainWindowItem("zulu-item", "Zulu", "Sword", alphaTab));
        items.push_back(makeMainWindowItem("alpha-item", "Alpha", "Sword", alphaTab));
        items.push_back(makeMainWindowItem("middle-item", "Middle", "Sword", alphaTab));
        fixture.itemsManager->OnItemsRefreshed(items, {alphaTab}, false);

        auto *model = tree->model();
        const QModelIndex bucket = model->index(0, 0);
        const QModelIndex firstItem = model->index(0, 0, bucket);
        const QModelIndex secondItem = model->index(1, 0, bucket);
        QVERIFY(firstItem.isValid());
        QVERIFY(secondItem.isValid());

        tree->selectionModel()->setCurrentIndex(firstItem,
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        tree->selectionModel()->select(secondItem,
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 2);
        QStringList initiallySelectedNames;
        for (const QModelIndex &selected : tree->selectionModel()->selectedRows()) {
            initiallySelectedNames.append(selected.data().toString());
        }
        initiallySelectedNames.sort();

        QSignalSpy layoutChanged(model, &QAbstractItemModel::layoutChanged);
        const Qt::SortOrder nextOrder = tree->header()->sortIndicatorOrder() == Qt::AscendingOrder
                                            ? Qt::DescendingOrder
                                            : Qt::AscendingOrder;
        // Drive the header's sort path without depending on geometry from an
        // unshown window.
        tree->header()->setSortIndicator(0, nextOrder);

        QCOMPARE(layoutChanged.count(), 1);
        const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
        QCOMPARE(selectedRows.size(), 2);
        QStringList selectedNames;
        for (const QModelIndex &selected : selectedRows) {
            selectedNames.append(selected.data().toString());
        }
        selectedNames.sort();
        QCOMPARE(selectedNames, initiallySelectedNames);
    }
}

// M2 D6/R5-1 (stage 3): bucket identity is the stable (type, id) and each
// bucket renders the freshest metadata seen for that key. Mid-refresh deltas
// for a moved tab, a renamed tab, and a tab whose fresh position collides
// with an unrefreshed tab's stale position must produce exactly one bucket
// per stable tab id — no split, no stale header, no merge.
void MainWindowTest::bucketsKeyOnStableIdDuringRefresh()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    const ItemLocation tabD = makeTestStashLocation("stash-dddd", "Delta", 3);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-c", "GammaItem", "Axe", tabC));
    items.push_back(makeMainWindowItem("item-d", "DeltaItem", "Wand", tabD));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC, tabD}, false);

    // Mid-refresh deltas: A moves to position 5, B is renamed in place, and
    // C's fresh position collides with unrefreshed D's stale position 3.
    const ItemLocation movedA = makeTestStashLocation("stash-aaaa", "Alpha", 5);
    const ItemLocation renamedB = makeTestStashLocation("stash-bbbb", "Beta Renamed", 1);
    const ItemLocation collidedC = makeTestStashLocation("stash-cccc", "Gamma", 3);
    fixture.itemsManager
        ->OnTabRefreshed(movedA, {makeMainWindowItem("item-a2", "AlphaItem", "Sword", movedA)});
    fixture.itemsManager
        ->OnTabRefreshed(renamedB, {makeMainWindowItem("item-b2", "BetaItem", "Shield", renamedB)});
    fixture.itemsManager
        ->OnTabRefreshed(collidedC, {makeMainWindowItem("item-c2", "GammaItem", "Axe", collidedC)});

    // Force a refilter of the current search mid-refresh (the delta path
    // lands separately; any refilter must bucket soundly).
    fixture.window->OnSearchFormChange();

    const QAbstractItemModel *model = tree->model();
    QVERIFY(model);
    // Exactly one bucket per stable tab id: four tabs, four buckets.
    QCOMPARE(model->rowCount(), 4);
    QStringList headers;
    for (int row = 0; row < model->rowCount(); ++row) {
        headers.append(model->index(row, 0).data().toString());
    }
    // Fresh metadata renders: B under its new name, A at its new position
    // (after C and D, which collide on position 3 but never merge — the
    // stable id orders them deterministically).
    QCOMPARE(headers,
             QStringList(
                 {"#2, \"Beta Renamed\"", "#4, \"Gamma\"", "#4, \"Delta\"", "#6, \"Alpha\""}));
    // Each bucket holds exactly its tab's item: no split or merge moved an
    // item to a neighbor.
    QCOMPARE(visibleItemNames(*tree),
             QStringList({"BetaItem Shield", "GammaItem Axe", "DeltaItem Wand", "AlphaItem Sword"}));
}

// M2 D9 rule 1, post-S4 (R1-7): a delta not intersecting the current
// search performs no model operation; background searches are marked
// items-dirty verbatim, while the current search — which processed the
// delta by correctly adjudicating "no visible change" — stays clean, so
// switching away and back triggers no spurious refilter.
void MainWindowTest::backgroundDeltaLeavesModelUntouched()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tabs && tree && name);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    // Search 2 unfiltered in the background; search 1 current, filtered to
    // alpha so tab B contributes nothing visible.
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(0);
    name->setFocus();
    QTest::keyClicks(name, "alphaitem");
    // The form edit is debounced; wait for its refilter to land before
    // observing the delta path.
    QTRY_COMPARE_WITH_TIMEOUT(visibleItemNames(*tree), QStringList({"AlphaItem Sword"}), 2000);

    QAbstractItemModel *current_model = tree->model();
    QSignalSpy resets(current_model, &QAbstractItemModel::modelReset);

    // A delta for tab B: no visible source, no filter match — no model
    // operation on the current search, immediately or later.
    fixture.itemsManager
        ->OnTabRefreshed(tabB, {makeMainWindowItem("item-b2", "BetaItem Two", "Shield", tabB)});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Sword"}));

    // The background search was marked dirty (rule 1 verbatim) and
    // refilters on activation...
    tabs->setCurrentIndex(1);
    QVERIFY(visibleItemNames(*tree).contains("BetaItem Two Shield"));
    // ...but the current search processed the delta and stayed clean
    // (R1-7): switching back skips the refilter — the M2 behavior (dirty
    // → spurious full refilter) is explicitly renegotiated.
    QSignalSpy current_resets(current_model, &QAbstractItemModel::modelReset);
    tabs->setCurrentIndex(0);
    QCOMPARE(current_resets.count(), 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Sword"}));
}

// M2 D9, post-S4: the removal half of the intersection test at the new
// grain — an empty delta whose fetch source has visible rows removes
// exactly those rows immediately, as row operations, never a reset. The
// intersection contract's metadata half (R1-4) rides the same delta: the
// location anchor renders even though the item half is empty.
void MainWindowTest::removalOnlyDeltaIntersects()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Sword"}));

    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy removals(tree->model(), &QAbstractItemModel::rowsRemoved);
    // The empty delta also carries a fresh anchor (renamed in place):
    // both halves of the intersection contract apply at once.
    fixture.itemsManager->OnTabRefreshed(makeTestStashLocation("stash-aaaa", "Alpha Renamed", 0),
                                         {});

    // The rows leave now, by row operations; the bucket stays (deletion
    // is a snapshot-boundary effect) and renders the fresh name.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 1);
    QCOMPARE(visibleItemNames(*tree), QStringList());
    QCOMPARE(tree->model()->rowCount(), 1);
    const ItemLocation renamed = makeTestStashLocation("stash-aaaa", "Alpha Renamed", 0);
    QVERIFY(findBucket(*tree->model(), renamed.GetHeader()).isValid());
}

// M2 D9/R5-2/R6-2, post-S4 (D3): a ChildrenReconciled whose expected set
// excludes visible ghost children applies immediately as row removals
// scoped to the parent's bucket — no timer, no reset — and the applied
// state persists through a terminal failure (no final snapshot).
void MainWindowTest::childReconciliationIntersectsVisibleGhosts()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    // A parent tab whose published items include one fetched from a child
    // source (a Map-style ghost candidate).
    const ItemLocation parent = makeTestStashLocation("stash-pppp", "Maps", 0);
    ItemLocation child_fetch = parent;
    child_fetch.setFetchId("child-0001");
    Items items;
    items.push_back(makeMainWindowItem("item-p", "ParentItem", "Sword", parent));
    items.push_back(makeMainWindowItem("item-m", "GhostItem", "Shield", child_fetch));
    fixture.itemsManager->OnItemsRefreshed(items, {parent}, false);
    QCOMPARE(visibleItemNames(*tree).size(), 2);

    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy removals(tree->model(), &QAbstractItemModel::rowsRemoved);

    // The reconciliation's expected set is the parent alone: the visible
    // ghost leaves now, by row removals scoped to the parent's bucket.
    fixture.itemsManager->OnChildrenReconciled(parent,
                                               {FetchSourceKey{ItemLocationType::STASH,
                                                               "stash-pppp"}});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 1);
    QCOMPARE(visibleItemNames(*tree), QStringList({"ParentItem Sword"}));

    // A terminal failure follows — no final snapshot. The applied state
    // persists; nothing resurrects the ghost.
    fixture.window->OnRefreshFinished(RefreshOutcome{FailedRefresh{RateLimit::FetchError{}}});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"ParentItem Sword"}));
}

// M2 R6-3, post-S4 (D3/R1-4): expansion is keyed by the stable (type,
// id), and a rename delta now applies in place — dataChanged on the
// bucket row, no reset, no restore machinery — so the expanded tab keeps
// its expansion trivially and renders the new name immediately.
void MainWindowTest::expansionSurvivesRenameByStableKey()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    const QModelIndex bucket = findBucket(*tree->model(), tabA.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    QVERIFY(tree->isExpanded(bucket));

    // The delta carries the same stable id under a new label.
    const ItemLocation renamed = makeTestStashLocation("stash-aaaa", "AlphaPrime", 0);
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    fixture.itemsManager
        ->OnTabRefreshed(renamed, {makeMainWindowItem("item-a", "AlphaItem", "Sword", renamed)});

    // The bucket renders the fresh metadata immediately and is still
    // expanded; no reset ran and no restore machinery was needed (the
    // untouched state simply never moved).
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.expansion_captures, 0);
    QCOMPARE(probes.expansion_restores, 0);
    probes.enabled = false;
    QVERIFY(!findBucket(*tree->model(), tabA.GetHeader()).isValid());
    const QModelIndex renamedBucket = findBucket(*tree->model(), renamed.GetHeader());
    QVERIFY(renamedBucket.isValid());
    QVERIFY(tree->isExpanded(renamedBucket));
}

// M2 R6-3, post-S4: a streamed replacement swaps the selected item's
// object for a new one with the same stable id, now applied as row
// operations. The selection follows the id through the intent machinery
// (R1-3 — this pin is re-expressed over it), and the details panel adopts
// the replacement object rather than rendering the dead one.
void MainWindowTest::selectionSurvivesReplacementByStableIdentity()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *locationLabel = fixture.window->findChild<QLabel *>("locationLabel");
    QVERIFY(tree);
    QVERIFY(locationLabel);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-z", "ZuluItem", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    const QModelIndex bucket = findBucket(*tree->model(), tabA.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    QModelIndex selected;
    for (int row = 0; row < tree->model()->rowCount(bucket); ++row) {
        const QModelIndex index = tree->model()->index(row, 0, bucket);
        if (index.data().toString() == "AlphaItem Sword") {
            selected = index;
        }
    }
    QVERIFY(selected.isValid());
    tree->selectionModel()->setCurrentIndex(selected,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    // The replacement delta: a NEW Item object with the same stable id and
    // a different rendered name, applied immediately as row operations.
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a", "AlphaItem Two", "Sword", tabA),
                          makeMainWindowItem("item-z", "ZuluItem", "Sword", tabA)});
    QCOMPARE(resets.count(), 0);

    // The selection followed the stable id to the replacement object.
    const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "AlphaItem Two Sword");

    // The details panel re-rendered from the adopted object (the deferred
    // update path a user selection takes).
    QTRY_COMPARE_WITH_TIMEOUT(locationLabel->text(), tabA.GetHeader(), 2000);
}

// M2 R6-3, retargeted in M3 S5 (supersession map): the throttled reset
// died with the timer, and the capture/restore machinery now serves D6's
// user-initiated refilter reset — capture immediately before the reset,
// restore by top-row anchor; when the anchored row does not survive the
// refilter, the raw scrollbar value is the fallback and the anchor's
// bucket header is never scrolled to the top in its place.
void MainWindowTest::scrollAndCaptureSurviveUserRefilter()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tree && name);
    fixture.window->resize(900, 500);
    fixture.window->show();

    // Enough items that the tree scrolls. Names are chosen so a "item"
    // name filter later excludes exactly the anchor row: descending name
    // order puts the Zeta block above "Middle Row" and the Alpha block
    // below it, giving scroll room on both sides.
    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    Items items;
    for (int n = 0; n < 20; ++n) {
        items.push_back(makeMainWindowItem(QString("item-z%1").arg(n),
                                           QString("ZetaItem %1").arg(n),
                                           "Sword",
                                           tabA));
    }
    items.push_back(makeMainWindowItem("item-mid", "Middle Row", "Sword", tabA));
    for (int n = 0; n < 40; ++n) {
        items.push_back(makeMainWindowItem(QString("item-a%1").arg(n),
                                           QString("AlphaItem %1").arg(n),
                                           "Sword",
                                           tabA));
    }
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    switchToByItemView(fixture);
    QTRY_VERIFY_WITH_TIMEOUT(tree->verticalScrollBar()->maximum() > 0, 2000);

    // Scroll the anchor row to the top.
    auto *model = tree->model();
    const QModelIndex anchor = findItemRow(*model, model->index(0, 0), "Middle Row Sword");
    QVERIFY(anchor.isValid());
    tree->scrollTo(anchor, QAbstractItemView::PositionAtTop);
    const QModelIndex topBefore = tree->indexAt(QPoint(0, 0));
    QVERIFY(topBefore.isValid());
    QVERIFY(topBefore.parent().isValid());
    QCOMPARE(topBefore.data().toString(), "Middle Row Sword");

    // A user refilter that keeps every row: the same item is back on top
    // after the reset (anchor restore, not raw-value coincidence).
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    fixture.window->OnSearchFormChange();
    QCOMPARE(resets.count(), 1);
    const QModelIndex topAfter = tree->indexAt(QPoint(0, 0));
    QVERIFY(topAfter.isValid());
    QCOMPARE(topAfter.data().toString(), "Middle Row Sword");

    // A filter edit that excludes the anchored row: the fallback is the
    // raw scrollbar value, never the anchor's bucket header scrolled to
    // the top.
    const int valueBefore = tree->verticalScrollBar()->value();
    name->setFocus();
    QTest::keyClicks(name, "item");
    fixture.window->OnSearchFormChange();
    QCOMPARE(resets.count(), 2);
    QVERIFY(!findItemRow(*tree->model(), tree->model()->index(0, 0), "Middle Row Sword").isValid());
    QCOMPARE(tree->verticalScrollBar()->value(), valueBefore);
    const QModelIndex topFallback = tree->indexAt(QPoint(0, 0));
    QVERIFY(topFallback.isValid());
    // Not the bucket header pinned to the top.
    QVERIFY(topFallback.parent().isValid());
}

// The S0 probe surface, window half: one snapshot refresh on a single
// search drives exactly one capture/restore cycle — the counters the M3
// pins will read to prove the restore machinery does NOT run on the
// delta path (`unrelatedDeltaLeavesOtherBucketsUntouched`).
void MainWindowTest::probeCountersTrackCaptureRestore()
{
    MainWindowFixture fixture;

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("alpha-one", "Alpha One", "Sword", tabA));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    // Initial population (D6) is the refresh boundary that still runs the
    // capture/restore reset machinery — S6 removed it from the non-initial
    // snapshot, which reconciles instead.
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, true);

    QCOMPARE(probes.expansion_captures, 1);
    QCOMPARE(probes.scroll_captures, 1);
    QCOMPARE(probes.expansion_restores, 1);
    QCOMPARE(probes.scroll_restores, 1);
    QCOMPARE(probes.reselects, 1);
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.final_reconciliations, 0);

    // The S6 site: a non-initial snapshot enters the row reconciliation
    // and none of the reset-path machinery.
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    QCOMPARE(probes.final_reconciliations, 1);
    QCOMPARE(probes.expansion_captures, 1);
    QCOMPARE(probes.scroll_captures, 1);
    QCOMPARE(probes.refilters, 1);
    probes.enabled = false;
}

// M3 S2, behavioral half of `priceKeysFollowBuyoutEdits` (D1 rule 4): with
// Price active, user buyout edits — item and tab level, set and clear —
// reorder affected rows at command end, one model update per command
// (R2-5/R3-3); Date is symmetric. The resident-key
// assertions (R3-2: entries rebuild before the reorder) land in S3.
void MainWindowTest::priceKeysFollowBuyoutEdits()
{
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        QVERIFY(tree);

        const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
        Items items;
        items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
        items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
        items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

        // Price ascending: unpriced (currency rank 0) sorts before priced.
        tree->header()->setSortIndicator(1, Qt::AscendingOrder);
        auto *model = tree->model();
        const QModelIndex bucket = findBucket(*model, tabA.GetHeader());
        QVERIFY(bucket.isValid());
        tree->expand(bucket);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));

        auto &probes = ModelProbes::instance();
        probes.reset();
        probes.enabled = true;

        // The expanded bucket's Price keys are resident (S3): the R3-2
        // assertions below hold the whole command sequence to "entries
        // rebuild, then one reorder on the resident vector" — zero full
        // key builds, one bucket sort per command, never a re-sort on
        // stale keys (the order assertions would catch one).
        QVERIFY(probes.live_key_bytes > 0);

        // Item-level set: the affected row moves at command end.
        tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "Alpha Sword"),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        probes.reset();
        applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");
        QCOMPARE(probes.model_updates, 1);
        QCOMPARE(probes.bucket_sorts, 1);
        QCOMPARE(probes.key_builds, 0);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Bravo Sword", "Charlie Sword", "Alpha Sword"}));

        // Tab-level set: the propagated inherited prices (9c on Bravo and
        // Charlie) reorder against Alpha's manual 7c, one update for the
        // whole command including the nested propagation pass (R3-3).
        tree->selectionModel()->setCurrentIndex(findBucket(*model, tabA.GetHeader()),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        probes.reset();
        applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "9");
        QCOMPARE(probes.model_updates, 1);
        QCOMPARE(probes.key_builds, 0);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));

        // Tab-level clear: the inherited prices vanish at command end.
        tree->selectionModel()->setCurrentIndex(findBucket(*model, tabA.GetHeader()),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        probes.reset();
        applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_INHERIT, Currency::CURRENCY_NONE, "");
        QCOMPARE(probes.model_updates, 1);
        QCOMPARE(probes.key_builds, 0);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Bravo Sword", "Charlie Sword", "Alpha Sword"}));

        // Item-level clear: back to the unpriced tie-break order.
        tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "Alpha Sword"),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        probes.reset();
        applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_INHERIT, Currency::CURRENCY_NONE, "");
        QCOMPARE(probes.model_updates, 1);
        QCOMPARE(probes.key_builds, 0);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));

        // Date symmetric: descending puts the freshly priced item (the only
        // valid last_update) first at command end.
        tree->header()->setSortIndicator(2, Qt::DescendingOrder);
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Charlie Sword", "Bravo Sword", "Alpha Sword"}));
        tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "Bravo Sword"),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);
        probes.reset();
        applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "5");
        QCOMPARE(probes.model_updates, 1);
        QCOMPARE(probes.key_builds, 0); // the Date keys rebuilt entries, not the vector
        QCOMPARE(bucketItemNames(*model, bucket),
                 QStringList({"Bravo Sword", "Charlie Sword", "Alpha Sword"}));
        probes.enabled = false;
    }
}

// M3 R2-5: with Price active in By-Item, one buyout command over a
// many-row selection produces exactly one reorder / model update at
// command end — the trailing PropagateTabBuyouts pass included (R3-3) —
// never one per selected row.
void MainWindowTest::multiSelectionBuyoutEditReordersOnce()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(tree);
    QVERIFY(viewCombo);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    tree->header()->setSortIndicator(1, Qt::AscendingOrder);
    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);

    auto *model = tree->model();
    const QModelIndex flatBucket = model->index(0, 0);
    QVERIFY(flatBucket.isValid());
    QCOMPARE(bucketItemNames(*model, flatBucket),
             QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword", "Delta Sword"}));

    // Select the first three rows; Delta stays unpriced.
    tree->selectionModel()->setCurrentIndex(model->index(0, 0, flatBucket),
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    tree->selectionModel()->select(model->index(1, 0, flatBucket),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    tree->selectionModel()->select(model->index(2, 0, flatBucket),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 3);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy layouts(model, &QAbstractItemModel::layoutChanged);
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "5");
    QCOMPARE(probes.model_updates, 1);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(layouts.count(), 1);
    probes.enabled = false;

    QCOMPARE(bucketItemNames(*model, flatBucket),
             QStringList({"Delta Sword", "Alpha Sword", "Bravo Sword", "Charlie Sword"}));
}

// M3 R1-6: a scoped (per-delta) or final pricing pass touching many items
// produces at most one reorder / model update on the active search, never
// one per Set.
void MainWindowTest::pricingPassYieldsSingleModelUpdate()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    tree->header()->setSortIndicator(1, Qt::AscendingOrder);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // The scoped pass: a delta whose notes price three items is one batch.
    Items delta;
    delta.push_back(makeMainWindowItemWithNote("item-a", "Alpha", "Sword", tabA, "~b/o 5 chaos"));
    delta.push_back(makeMainWindowItemWithNote("item-b", "Bravo", "Sword", tabA, "~b/o 3 chaos"));
    delta.push_back(makeMainWindowItemWithNote("item-c", "Charlie", "Sword", tabA, "~b/o 9 chaos"));
    delta.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabA));
    delta.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabA));
    probes.reset();
    fixture.itemsManager->OnTabRefreshed(tabA, delta);
    QCOMPARE(probes.model_updates, 1);

    // The final propagation pass alone: two inherited prices, one batch.
    fixture.buyoutFixture.manager->SetTab(tabA, makeChaosBuyout(2));
    probes.reset();
    fixture.itemsManager->PropagateTabBuyouts();
    QCOMPARE(probes.model_updates, 1);
    probes.enabled = false;
}

// M3 R3-4: one snapshot's ApplyAutoTabBuyouts ->
// ApplyAutoItemBuyouts -> PropagateTabBuyouts sequence produces at most
// one reorder / model update on the active search, never one per pass;
// buyout persistence writes still occur.
void MainWindowTest::snapshotPricingSequenceEmitsOneModelBatch()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    // The tab's label auto-prices the tab, Alpha's note auto-prices the
    // item, and Bravo inherits the tab price: three distinct passes mutate
    // buyout state in this one snapshot.
    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "~b/o 10 chaos", 0);
    Items items;
    items.push_back(makeMainWindowItemWithNote("item-a", "Alpha", "Sword", tabA, "~b/o 5 chaos"));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));

    tree->header()->setSortIndicator(1, Qt::AscendingOrder);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    QCOMPARE(probes.model_updates, 1);
    probes.enabled = false;

    // Persistence is outside batching and unchanged: the pricing writes
    // reached the repo.
    const auto itemBuyouts = fixture.buyoutFixture.repo->loadItemBuyouts().value();
    QVERIFY(itemBuyouts.contains("item-a"));
    const auto tabBuyouts = fixture.buyoutFixture.repo->loadLocationBuyouts().value();
    QVERIFY(tabBuyouts.contains("stash-alpha"));
}

// M3 R1-6, rule 5: with Name active, a buyout batch emits dataChanged for
// the affected visible Price/Date cells and performs no reordering — cell
// repaint is independent of the active sort column.
void MainWindowTest::priceCellsRepaintUnderAnySortColumn()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    // Name (column 0) stays the active sort column.
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    const QStringList namesBefore = bucketItemNames(*model, bucket);
    const QModelIndex target = findItemRow(*model, bucket, "Bravo Sword");
    QVERIFY(target.isValid());
    tree->selectionModel()->setCurrentIndex(target,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy repaints(model, &QAbstractItemModel::dataChanged);
    QSignalSpy layouts(model, &QAbstractItemModel::layoutChanged);
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");

    // The batch emitted once, repainted the affected Price/Date cells, and
    // reordered nothing.
    QCOMPARE(probes.model_updates, 1);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(layouts.count(), 0);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucket), namesBefore);

    QCOMPARE(repaints.count(), 1);
    const QModelIndex topLeft = repaints.at(0).at(0).toModelIndex();
    const QModelIndex bottomRight = repaints.at(0).at(1).toModelIndex();
    QCOMPARE(topLeft.parent(), bucket);
    QVERIFY(topLeft.row() <= target.row());
    QVERIFY(bottomRight.row() >= target.row());
    // The span covers the Price (1) and Date (2) columns.
    QVERIFY(topLeft.column() <= 1);
    QVERIFY(bottomRight.column() >= 2);
}

// M3 S2, review round 1: the rule-5 repaint must reach EVERY visible
// occurrence of an affected id. The R6-3 id index keeps only the first
// occurrence of a duplicated id (mid-refresh divergence can show one id in
// two tabs) and holds no entry for id-less items, so affected ids the
// index cannot fully represent fall back to an every-bucket scan.
void MainWindowTest::buyoutRepaintCoversEveryVisibleOccurrence()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-x", "MoverTwo", "Sword", tabB));
    items.push_back(makeMainWindowItem("", "Ghost", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    // Name stays the active sort column: pure repaint, no reordering.
    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketA.isValid());
    QVERIFY(bucketB.isValid());
    tree->expand(bucketA);
    tree->expand(bucketB);

    // Editing the duplicated id through its tab-A occurrence repaints BOTH
    // occurrences — one span per affected bucket, each covering exactly
    // the affected row (the unaffected Delta and Ghost rows stay outside).
    const QModelIndex moverA = findItemRow(*model, bucketA, "Mover Sword");
    const QModelIndex moverB = findItemRow(*model, bucketB, "MoverTwo Sword");
    QVERIFY(moverA.isValid());
    QVERIFY(moverB.isValid());
    tree->selectionModel()->setCurrentIndex(moverA,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QSignalSpy repaints(model, &QAbstractItemModel::dataChanged);
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");
    QCOMPARE(repaints.count(), 2);
    for (int n = 0; n < repaints.count(); ++n) {
        const QModelIndex topLeft = repaints.at(n).at(0).toModelIndex();
        const QModelIndex bottomRight = repaints.at(n).at(1).toModelIndex();
        const QModelIndex expected = (topLeft.parent() == bucketA) ? moverA : moverB;
        QCOMPARE(topLeft.parent(), expected.parent());
        QCOMPARE(topLeft.row(), expected.row());
        QCOMPARE(bottomRight.row(), expected.row());
    }

    // Editing the id-less item repaints its row through the same fallback:
    // the empty id has no index entry, only the scan can find it.
    const QModelIndex ghost = findItemRow(*model, bucketB, "Ghost Sword");
    QVERIFY(ghost.isValid());
    tree->selectionModel()->setCurrentIndex(ghost,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    repaints.clear();
    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "3");
    QCOMPARE(repaints.count(), 1);
    const QModelIndex topLeft = repaints.at(0).at(0).toModelIndex();
    const QModelIndex bottomRight = repaints.at(0).at(1).toModelIndex();
    QCOMPARE(topLeft.parent(), bucketB);
    QCOMPARE(topLeft.row(), ghost.row());
    QCOMPARE(bottomRight.row(), ghost.row());
}

// M3 D2 (S3): an unfiltered By-Tab refilter sorts no collapsed bucket and
// builds no keys for one; expanding one bucket sorts exactly that bucket,
// correctly. Unsorted is not unstable — a collapsed bucket keeps arrival
// order until something changes its contents.
void MainWindowTest::collapsedBucketsDeferSorting()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // The user refilter of the default-collapsed unfiltered view: the
    // whole sort/key toll disappears, not merely cheapens.
    fixture.window->OnSearchFormChange();
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(probes.keyed_compares, 0);

    // Arrival order, deterministically, until expansion sorts.
    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Alpha Sword", "Charlie Sword", "Bravo Sword"}));

    // Expanding sorts exactly the expanded bucket (default indicator:
    // Name descending), building exactly its keys.
    tree->expand(bucketA);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabA)], 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabB)], 0);
    QCOMPARE(probes.key_builds, 1);
    QCOMPARE(probes.key_builds_by_location[LocationInventory::KeyFor(tabB)], 0);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Charlie Sword", "Bravo Sword", "Alpha Sword"}));
}

// M3 D2 (S3): a user refilter with N saved expansions sorts exactly those
// N buckets on restore — the restore's expand signals find the fresh
// buckets' flags invalid and sort each, and nothing sorts the rest.
void MainWindowTest::restoredExpansionSortsRestoredBucketsOnly()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-gamma", "Gamma Tab", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabC));
    items.push_back(makeMainWindowItem("item-f", "Foxtrot", "Sword", tabC));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

    auto *model = tree->model();
    tree->expand(findBucket(*model, tabA.GetHeader()));
    tree->expand(findBucket(*model, tabC.GetHeader()));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    fixture.window->OnSearchFormChange();
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.bucket_sorts, 2);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabA)], 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabB)], 0);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabC)], 1);
    QCOMPARE(probes.key_builds, 2);
    probes.enabled = false;

    // The restored buckets are sorted; the collapsed one keeps arrival
    // order.
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabA.GetHeader())),
             QStringList({"Bravo Sword", "Alpha Sword"}));
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabB.GetHeader())),
             QStringList({"Delta Sword", "Charlie Sword"}));
}

// M3 D2 rule 5 (S3): a filtered search is default-expanded, so every
// visible bucket of the result presents sorted — established eagerly in
// the refilter's one view-wide pass, not per expand signal.
void MainWindowTest::filteredSearchSortsAllVisibleBuckets()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tree);
    QVERIFY(name);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-z", "Zulu", "Axe", tabA));
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    name->setFocus();
    QTest::keyClicks(name, "sword");
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    fixture.window->OnSearchFormChange();
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.bucket_sorts, 2);
    QCOMPARE(probes.key_builds, 2);
    probes.enabled = false;

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketA.isValid());
    QVERIFY(bucketB.isValid());
    QVERIFY(tree->isExpanded(bucketA));
    QVERIFY(tree->isExpanded(bucketB));
    QCOMPARE(bucketItemNames(*model, bucketA), QStringList({"Charlie Sword", "Alpha Sword"}));
    QCOMPARE(bucketItemNames(*model, bucketB), QStringList({"Echo Sword", "Delta Sword"}));
}

// M3 R1-5 (S3): expand (sort), collapse, re-expand with no intervening
// invalidation — no key build and no sort runs the second time, and the
// order is the sorted one. Arrival order is never reconstructed; collapse
// is a view event, not a model event.
void MainWindowTest::sortedOrderSurvivesCollapse()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Charlie Sword", "Bravo Sword", "Alpha Sword"}));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    tree->collapse(bucketA);
    tree->expand(bucketA);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(probes.keyed_compares, 0);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Charlie Sword", "Bravo Sword", "Alpha Sword"}));
}

// M3 R2-3 (S3): an expanded bucket's keys persist across re-sorts and
// direction flips (no key rebuild on a flip); collapsing evicts its keys
// (live key bytes drop) while its order and flag persist.
void MainWindowTest::keyResidencyFollowsMaterialization()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    auto &probes = ModelProbes::instance();
    const std::int64_t baseline = probes.live_key_bytes;

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    const std::int64_t resident = probes.live_key_bytes;
    QVERIFY(resident > baseline);

    // The flip re-sorts on the resident keys: no rebuild, bytes steady.
    probes.reset();
    probes.enabled = true;
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(probes.live_key_bytes, resident);
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));

    // Collapse evicts the keys and nothing else: the sorted order and
    // flag persist, so re-expansion does no work and shows the ascending
    // order.
    tree->collapse(bucketA);
    QCOMPARE(probes.live_key_bytes, baseline);
    tree->expand(bucketA);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.key_builds, 0);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));
}

// M3 R3-1 (S3): expand (keys built), collapse (keys evicted), re-expand
// (valid flag: no sort, no key build — sorted-but-keyless): a direction
// flip then hydrates the bucket's keys exactly once and re-sorts
// correctly; a second flip rebuilds nothing.
void MainWindowTest::reexpandedBucketFlipHydratesOnce()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    tree->collapse(bucketA);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    tree->expand(bucketA);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);

    // The flip is the key-consuming event: one hydration, one sort.
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.key_builds, 1);
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Alpha Sword", "Bravo Sword", "Charlie Sword"}));

    // The second flip reuses the now-resident keys.
    tree->header()->setSortIndicator(0, Qt::DescendingOrder);
    QCOMPARE(probes.bucket_sorts, 2);
    QCOMPARE(probes.key_builds, 1);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"Charlie Sword", "Bravo Sword", "Alpha Sword"}));
}

// M3 R1-5/R2-3 (S3): switching the active column discards resident keys,
// clears every sorted flag, and re-sorts materialized buckets only (their
// keys rebuild — the old column's keys cannot order the new one); a
// direction flip after the switch re-sorts on the resident keys with no
// rebuild; collapsed buckets sort at their next expansion.
void MainWindowTest::sortColumnSwitchResortsVisibleBucketsOnly()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-gamma", "Gamma Tab", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-f", "Foxtrot", "Sword", tabC));
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabC));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

    auto *model = tree->model();
    tree->expand(findBucket(*model, tabA.GetHeader()));
    tree->expand(findBucket(*model, tabB.GetHeader()));

    // The switch (Name -> Quality): the materialized set rebuilds and
    // re-sorts; the collapsed bucket is untouched.
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    tree->header()->setSortIndicator(3, Qt::AscendingOrder);
    QCOMPARE(probes.bucket_sorts, 2);
    QCOMPARE(probes.key_builds, 2);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabC)], 0);
    QCOMPARE(probes.key_builds_by_location[LocationInventory::KeyFor(tabC)], 0);

    // The flip after the switch reuses the fresh resident keys.
    tree->header()->setSortIndicator(3, Qt::DescendingOrder);
    QCOMPARE(probes.bucket_sorts, 4);
    QCOMPARE(probes.key_builds, 2);

    // The collapsed bucket pays at its next expansion, under the new
    // column, exactly once.
    tree->expand(findBucket(*model, tabC.GetHeader()));
    QCOMPARE(probes.bucket_sorts, 5);
    QCOMPARE(probes.key_builds, 3);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabC)], 1);
    QCOMPARE(probes.key_builds_by_location[LocationInventory::KeyFor(tabC)], 1);
    probes.enabled = false;
}

// M3 R2-4 (S3, PARTIAL — fully closes in S5): with several searches,
// aggregate resident key memory never exceeds one search's worth;
// deactivating a search evicts its keys, and no refilter runs when the
// search is not dirty. Reactivation rehydrates a clean By-Tab search
// lazily, per bucket. The clean-By-Item eager-hydration clause (R3-1)
// lands with D4's eager activation in S5.
void MainWindowTest::residentKeysScopedToActiveSearch()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(tabs);
    QVERIFY(tree);
    QVERIFY(viewCombo);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "Charlie", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-d", "Delta", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto &probes = ModelProbes::instance();
    const std::int64_t none = probes.live_key_bytes;

    // Search 1 (By-Tab): one expanded bucket holds the only resident keys.
    tree->expand(findBucket(*tree->model(), tabA.GetHeader()));
    QVERIFY(probes.live_key_bytes > none);

    // Creating and switching to Search 2 deactivates Search 1: its keys
    // evict (background searches hold exactly 0), while its order, flags,
    // and expansion persist for reactivation.
    tabs->setCurrentIndex(1);
    QCOMPARE(probes.live_key_bytes, none);

    // Search 2 goes By-Item: the flat bucket's keys are the aggregate's
    // entire contents — one search's worth.
    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);
    const std::int64_t one_by_item = probes.live_key_bytes;
    QVERIFY(one_by_item > none);

    // A second By-Item search: the deactivation eviction keeps the
    // aggregate at exactly one search's worth — N background By-Item
    // searches hold none, not one flat bucket each.
    tabs->setCurrentIndex(2);
    QCOMPARE(probes.live_key_bytes, none);
    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);
    QCOMPARE(probes.live_key_bytes, one_by_item);

    // Reactivating clean Search 1: no refilter (the search is not
    // dirty), no eager hydration — the By-Tab expanded bucket stays
    // sorted-but-keyless until a key-consuming event.
    probes.reset();
    probes.enabled = true;
    tabs->setCurrentIndex(0);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(probes.live_key_bytes, none);
    const QModelIndex bucketA = findBucket(*tree->model(), tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    QVERIFY(tree->isExpanded(bucketA));
    QCOMPARE(bucketItemNames(*tree->model(), bucketA), QStringList({"Bravo Sword", "Alpha Sword"}));

    // The lazy rehydration, per bucket: the flip hydrates exactly the
    // expanded bucket.
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    QCOMPARE(probes.key_builds, 1);
    QCOMPARE(probes.key_builds_by_location[LocationInventory::KeyFor(tabA)], 1);
    QVERIFY(probes.live_key_bytes > none);

    // The clean-By-Item eager-hydration clause (S5, R3-1 carve-out — the
    // one deliberate exception to lazy): reactivating clean By-Item
    // Search 2 hydrates its flat bucket's keys at activation, with no
    // refilter — the aggregate returns to exactly one search's worth.
    probes.reset();
    tabs->setCurrentIndex(1);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.key_builds, 1);
    probes.enabled = false;
    QCOMPARE(probes.live_key_bytes, one_by_item);
}

// M3 S3, review round 1: the buyout batch's layout operation matches its
// affected MATERIALIZED scope. A Price-active batch touching only a
// collapsed bucket performs no reorder and emits no layout signals — the
// invalidation is flag-only, and the deferred sort pays at expansion with
// the fresh buyout in the order. A batch touching one expanded bucket
// scopes the whole layout dance to that bucket and leaves other expanded
// buckets' sorts untouched.
void MainWindowTest::buyoutReorderScopedToAffectedMaterializedBuckets()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    const auto alpha = makeMainWindowItem("item-a", "Alpha", "Sword", tabA);
    const auto delta = makeMainWindowItem("item-d", "Delta", "Sword", tabB);
    Items items;
    items.push_back(alpha);
    items.push_back(makeMainWindowItem("item-b", "Bravo", "Sword", tabA));
    items.push_back(delta);
    items.push_back(makeMainWindowItem("item-e", "Echo", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    // Price ascending; only bucket A is materialized.
    tree->header()->setSortIndicator(1, Qt::AscendingOrder);
    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    QCOMPARE(bucketItemNames(*model, bucketA), QStringList({"Alpha Sword", "Bravo Sword"}));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy layouts(model, &QAbstractItemModel::layoutChanged);

    // A direct manager mutation (the scoped pricing pass's shape) pricing
    // an item in collapsed B: one batch, cells repaint, but nothing
    // materialized is affected — no layout operation at all.
    fixture.buyoutFixture.manager->Set(*delta, makeChaosBuyout(5));
    QCOMPARE(probes.model_updates, 1);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(layouts.count(), 0);

    // The deferred sort pays at expansion, with the new price in the
    // order: Echo (unpriced, rank 0) now precedes Delta's 5c under Price
    // ascending — the tie-break order would have been Delta first.
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketB.isValid());
    tree->expand(bucketB);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(bucketItemNames(*model, bucketB), QStringList({"Echo Sword", "Delta Sword"}));

    // Pricing an item in expanded A: the layout operation scopes to A —
    // one layoutChanged naming A's subtree, one sort, no key build (the
    // resident entries rebuilt, R3-2), and B's sort count is untouched.
    probes.reset();
    layouts.clear();
    fixture.buyoutFixture.manager->Set(*alpha, makeChaosBuyout(7));
    QCOMPARE(probes.model_updates, 1);
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabA)], 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabB)], 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(layouts.count(), 1);
    const auto layout_parents = layouts.at(0).at(0).value<QList<QPersistentModelIndex>>();
    QCOMPARE(layout_parents.size(), 1);
    QCOMPARE(QModelIndex(layout_parents.front()), bucketA);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, bucketA), QStringList({"Bravo Sword", "Alpha Sword"}));
}

// M3 S4, the milestone's success criterion as a test (D3): refresh one
// tab; every other bucket's expansion, selection, and persistent indexes
// are untouched, and no restore machinery runs at all.
void MainWindowTest::unrelatedDeltaLeavesOtherBucketsUntouched()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "AlphaOne", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "AlphaTwo", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "BravoOne", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-b2", "BravoTwo", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketA.isValid());
    QVERIFY(bucketB.isValid());
    tree->expand(bucketA);
    tree->expand(bucketB);

    // Select a row in B and pin persistent indexes across its subtree.
    const QModelIndex selected = findItemRow(*model, bucketB, "BravoOne Sword");
    QVERIFY(selected.isValid());
    tree->selectionModel()->setCurrentIndex(selected,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    const QPersistentModelIndex persistentBucketB(bucketB);
    const QPersistentModelIndex persistentRowB(selected);
    const QStringList namesB = bucketItemNames(*model, bucketB);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);

    // Refresh tab A: a full replacement with fresh objects.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-a3", "AlphaThree", "Sword", tabA),
                                          makeMainWindowItem("item-a1", "AlphaOne", "Sword", tabA)});

    // Nothing outside A moved: no reset, no capture/restore/reselect
    // machinery, B's rows and persistent indexes bit-identical, and the
    // selection never lapsed.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.expansion_captures, 0);
    QCOMPARE(probes.expansion_restores, 0);
    QCOMPARE(probes.scroll_captures, 0);
    QCOMPARE(probes.scroll_restores, 0);
    QCOMPARE(probes.reselects, 0);
    QCOMPARE(probes.refilters, 0);
    probes.enabled = false;
    QVERIFY(tree->isExpanded(findBucket(*model, tabB.GetHeader())));
    QVERIFY(persistentBucketB.isValid());
    QVERIFY(persistentRowB.isValid());
    QCOMPARE(persistentRowB.data().toString(), "BravoOne Sword");
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabB.GetHeader())), namesB);
    const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "BravoOne Sword");
}

// M3 S4 (R1-1): a content delta's row operations touch exactly the rows
// fetched from its FetchSourceKey within the affected bucket; row
// accounting, filtered membership, and keyed order are correct after
// application.
void MainWindowTest::deltaReplacesExactlyItsSourceRows()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tree);
    QVERIFY(name);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "AlphaItem One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    // An active filter, so filtered membership is exercised: "item"
    // matches every seeded name (the arrivals include one it rejects).
    name->setFocus();
    QTest::keyClicks(name, "item");
    fixture.window->OnSearchFormChange();
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);

    auto *model = tree->model();
    // Expand A so the delta takes the visible-bucket merge path (R2-2).
    tree->expand(findBucket(*model, tabA.GetHeader()));
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy removals(model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy insertions(model, &QAbstractItemModel::rowsInserted);

    // The delta replaces tab A: one retained id under a fresh object, one
    // new item, and one arrival the active filter rejects.
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a4", "AlphaItem Zed", "Sword", tabA),
                          makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA),
                          makeMainWindowItem("item-a5", "NomatchAxe", "Sword", tabA)});

    // No reset; A holds exactly the filtered arrivals in keyed order; B's
    // rows were never touched.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 1);
    QVERIFY(insertions.count() >= 1);
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    QCOMPARE(bucketItemNames(*model, bucketA),
             QStringList({"AlphaItem Two Sword", "AlphaItem Zed Sword"}));
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketB.isValid());
    QCOMPARE(bucketItemNames(*model, bucketB), QStringList({"BetaItem Sword"}));
}

// M3 S4 (R1-1, R2-2): with an expanded parent bucket holding parent items
// plus two children's items whose sort keys interleave, one child's delta
// replaces only that child's rows AND the resulting bucket order is
// globally sorted — the arrivals are merged into the retained rows, not
// appended or sorted separately. Sibling and parent rows' persistent
// indexes survive.
void MainWindowTest::childDeltaPreservesSiblingSourcesInParentBucket()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation parent = makeTestStashLocation("stash-pppp", "Maps", 0);
    ItemLocation child1 = parent;
    child1.setFetchId("child-0001");
    ItemLocation child2 = parent;
    child2.setFetchId("child-0002");

    // Names interleave under the Name sort across all three sources.
    Items items;
    items.push_back(makeMainWindowItem("item-p1", "Alpha", "Sword", parent));
    items.push_back(makeMainWindowItem("item-c1a", "Bravo", "Sword", child1));
    items.push_back(makeMainWindowItem("item-c2a", "Charlie", "Sword", child2));
    items.push_back(makeMainWindowItem("item-p2", "Echo", "Sword", parent));
    items.push_back(makeMainWindowItem("item-c1b", "Foxtrot", "Sword", child1));
    items.push_back(makeMainWindowItem("item-c2b", "Golf", "Sword", child2));
    fixture.itemsManager->OnItemsRefreshed(items, {parent}, false);

    auto *model = tree->model();
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    const QModelIndex bucket = findBucket(*model, parent.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    QCOMPARE(bucketItemNames(*model, bucket),
             QStringList({"Alpha Sword",
                          "Bravo Sword",
                          "Charlie Sword",
                          "Echo Sword",
                          "Foxtrot Sword",
                          "Golf Sword"}));

    // Pin the sibling's and parent's rows.
    const QPersistentModelIndex alphaRow(findItemRow(*model, bucket, "Alpha Sword"));
    const QPersistentModelIndex charlieRow(findItemRow(*model, bucket, "Charlie Sword"));
    const QPersistentModelIndex golfRow(findItemRow(*model, bucket, "Golf Sword"));
    QVERIFY(alphaRow.isValid() && charlieRow.isValid() && golfRow.isValid());

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    // Child 1's delta: Bravo and Foxtrot leave; Delta and Hotel arrive,
    // interleaving with the retained rows under the sort.
    fixture.itemsManager->OnTabRefreshed(child1,
                                         {makeMainWindowItem("item-c1c", "Hotel", "Sword", child1),
                                          makeMainWindowItem("item-c1d", "Delta", "Sword", child1)});

    QCOMPARE(resets.count(), 0);
    // Globally sorted: the merge interleaved the arrivals; an append or a
    // separate arrivals-only sort would leave Delta and Hotel at the end.
    QCOMPARE(bucketItemNames(*model, findBucket(*model, parent.GetHeader())),
             QStringList({"Alpha Sword",
                          "Charlie Sword",
                          "Delta Sword",
                          "Echo Sword",
                          "Golf Sword",
                          "Hotel Sword"}));
    // Sibling and parent rows survived with their identities.
    QVERIFY(alphaRow.isValid());
    QVERIFY(charlieRow.isValid());
    QVERIFY(golfRow.isValid());
    QCOMPARE(alphaRow.data().toString(), "Alpha Sword");
    QCOMPARE(charlieRow.data().toString(), "Charlie Sword");
    QCOMPARE(golfRow.data().toString(), "Golf Sword");
}

// M3 S4: the M2 D6 boundary at the model layer — an empty replacement
// leaves an empty bucket row in an unfiltered search, never a bucket
// removal (deletion is a snapshot-boundary effect).
void MainWindowTest::emptyDeltaEmptiesBucketWithoutRemovingIt()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto *model = tree->model();
    const int top_level_rows = model->rowCount();
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);

    fixture.itemsManager->OnTabRefreshed(tabA, {});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(model->rowCount(), top_level_rows);
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    QCOMPARE(model->rowCount(bucketA), 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"BetaItem Sword"}));
}

// M3 S4: after a delta, the visible indexes answer as if freshly
// refiltered with no whole-collection rebuild — observed through the
// rebuild probe staying at zero while identity-dependent behavior
// (replacement adoption, cross-tab re-adoption, second-removal
// adjudication) works against the maintained indexes.
void MainWindowTest::deltaUpdatesVisibleIndexesIncrementally()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    const QModelIndex mover = findItemRow(*model, bucketA, "Mover Sword");
    QVERIFY(mover.isValid());
    tree->selectionModel()->setCurrentIndex(mover,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy removals(model, &QAbstractItemModel::rowsRemoved);

    // Replacement adoption: the same id under a fresh object — selection
    // follows through the id index (it answered with the new object).
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-x", "Mover Two", "Sword", tabA)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(tree->selectionModel()->selectedRows().front().data().toString(), "Mover Two Sword");

    // Cross-tab move: removal then insertion in another bucket; the
    // re-adoption walks the maintained id index.
    fixture.itemsManager->OnTabRefreshed(tabA, {});
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-x", "Mover Two", "Sword", tabB),
                                          makeMainWindowItem("item-b1", "BetaItem", "Sword", tabB)});
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketB.isValid());
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(tree->selectionModel()->selectedRows().front().parent(), bucketB);

    // Second-removal adjudication: tab A's source no longer has visible
    // rows, so its empty delta finds nothing to remove — no row
    // operations at all. (Narrowed by the spec's July 31 amendment: the
    // D9 source-index sets are deleted; source scoping is answered by
    // the items' own fetch keys during application.)
    const int removals_before = removals.count();
    fixture.itemsManager->OnTabRefreshed(tabA, {});
    QCOMPARE(removals.count(), removals_before);

    // All of it without a single whole-collection rebuild or refilter.
    QCOMPARE(probes.index_rebuilds, 0);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(resets.count(), 0);
    probes.enabled = false;
}

// M3 S4 (D3): a metadata delta that changes display ordering repositions
// the bucket via move operations; expansion and selection follow the
// stable (type, id) key — M2 R6-3's pins extended from reset-restore to
// move.
void MainWindowTest::bucketRepositionsByMoveOnMetadataDelta()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-c", "GammaItem", "Sword", tabC));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

    auto *model = tree->model();
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketB.isValid());
    tree->expand(bucketB);
    const QModelIndex selected = model->index(0, 0, bucketB);
    QVERIFY(selected.isValid());
    tree->selectionModel()->setCurrentIndex(selected,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy moves(model, &QAbstractItemModel::rowsMoved);

    // The delta moves B to the end of the display order, carrying the
    // tab's items under the fresh anchor (a delta replaces its source; an
    // empty one would also empty the tab).
    const ItemLocation movedB = makeTestStashLocation("stash-bbbb", "Beta", 5);
    fixture.itemsManager
        ->OnTabRefreshed(movedB, {makeMainWindowItem("item-b", "BetaItem", "Sword", movedB)});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(moves.count(), 1);
    QStringList headers;
    for (int row = 0; row < model->rowCount(); ++row) {
        headers.append(model->index(row, 0).data().toString());
    }
    QCOMPARE(headers, QStringList({"#1, \"Alpha\"", "#3, \"Gamma\"", "#6, \"Beta\""}));

    // Expansion and selection followed the stable key through the move.
    const QModelIndex movedBucket = findBucket(*model, movedB.GetHeader());
    QVERIFY(movedBucket.isValid());
    QCOMPARE(movedBucket.row(), 2);
    QVERIFY(tree->isExpanded(movedBucket));
    const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "BetaItem Sword");
    QCOMPARE(selectedRows.front().parent(), movedBucket);
}

// M3 S4 (R1-4): empty deltas carrying a rename, a move, and a
// new-empty-tab discovery each apply immediately — dataChanged / move /
// bucket insertion in an unfiltered search — with no final snapshot and
// no refilter, and the applied state persists after a terminal failure.
// (A color change rides the identical dataChanged path as the rename.)
// M2 R7-2's exception is retired: metadata-only deltas are inside the
// freshness statement now.
void MainWindowTest::metadataDeltaAppliesWithoutItemIntersection()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

    auto *model = tree->model();
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy insertions(model, &QAbstractItemModel::rowsInserted);

    // Empty deltas: B renamed in place, C moved, E newly discovered
    // (never in the published tab list).
    fixture.itemsManager->OnTabRefreshed(makeTestStashLocation("stash-bbbb", "Beta Renamed", 1), {});
    fixture.itemsManager->OnTabRefreshed(makeTestStashLocation("stash-cccc", "Gamma", 5), {});
    fixture.itemsManager->OnTabRefreshed(makeTestStashLocation("stash-eeee", "Epsilon", 4), {});

    // Everything rendered now: no reset, no refilter, no waiting for the
    // next user action or the final snapshot.
    const auto readHeaders = [&]() {
        QStringList headers;
        for (int row = 0; row < model->rowCount(); ++row) {
            headers.append(model->index(row, 0).data().toString());
        }
        return headers;
    };
    const QStringList expected(
        {"#1, \"Alpha\"", "#2, \"Beta Renamed\"", "#5, \"Epsilon\"", "#6, \"Gamma\""});
    QCOMPARE(readHeaders(), expected);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(insertions.count(), 1); // Epsilon's bucket row

    // A terminal failure emits no final snapshot; the applied state
    // persists — the M2 "invisible until user action after terminal
    // failure" caveat is gone.
    fixture.window->OnRefreshFinished(RefreshOutcome{FailedRefresh{RateLimit::FetchError{}}});
    QCOMPARE(readHeaders(), expected);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.refilters, 0);
    probes.enabled = false;
}

// M3 S4 (R1-5): expand (sort), collapse, replace the bucket's contents by
// delta, re-expand — the bucket re-sorts exactly once, correctly. The
// delta's application on the collapsed bucket is arrival-ordered and
// sort-free; the deferred sort pays at expansion.
void MainWindowTest::collapsedInvalidBucketResortsOnReexpand()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Alpha", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    auto *model = tree->model();
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    const QModelIndex bucket = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    QCOMPARE(bucketItemNames(*model, bucket), QStringList({"Alpha Sword", "Charlie Sword"}));
    tree->collapse(bucket);

    // The replacement lands on the collapsed bucket: no sort runs during
    // application (probe), and the rows sit in arrival order.
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-a3", "Zulu", "Sword", tabA),
                                          makeMainWindowItem("item-a4", "Bravo", "Sword", tabA),
                                          makeMainWindowItem("item-a5", "Yankee", "Sword", tabA)});
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);

    // Re-expansion sorts exactly that bucket, exactly once, correctly.
    tree->expand(findBucket(*model, tabA.GetHeader()));
    QCOMPARE(probes.bucket_sorts, 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabA)], 1);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabA.GetHeader())),
             QStringList({"Bravo Sword", "Yankee Sword", "Zulu Sword"}));
}

// M3 S4: a delta replacing a visible bucket's items yields fresh keyed
// order as part of application; stale order never persists on a visible
// bucket, and the order refresh is counted for that bucket alone.
void MainWindowTest::staleOrderNeverSurvivesDelta()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-alpha", "Alpha Tab", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-beta", "Beta Tab", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Charlie", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Alpha", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

    auto *model = tree->model();
    tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketA.isValid());
    QVERIFY(bucketB.isValid());
    tree->expand(bucketA);
    tree->expand(bucketB);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);

    // Arrival order (Zulu, Bravo, Mike) deliberately disagrees with the
    // sort; the application must present keyed order immediately.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-a3", "Zulu", "Sword", tabA),
                                          makeMainWindowItem("item-a4", "Bravo", "Sword", tabA),
                                          makeMainWindowItem("item-a5", "Mike", "Sword", tabA)});

    QCOMPARE(resets.count(), 0);
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabA.GetHeader())),
             QStringList({"Bravo Sword", "Mike Sword", "Zulu Sword"}));
    // The order refresh was counted for A alone; B's order never moved.
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabA)], 1);
    QCOMPARE(probes.bucket_sorts_by_location[LocationInventory::KeyFor(tabB)], 0);
    probes.enabled = false;
    QCOMPARE(bucketItemNames(*model, findBucket(*model, tabB.GetHeader())),
             QStringList({"BetaItem Sword"}));
}

// M3 S4 (R1-3): the successor of M2's reselectionSurvivesCrossTabMove
// under no-reset machinery. With an item selected, one delta removes it;
// several deltas later another inserts it in a different tab — the
// selection re-adopts by stable id through the global index. If the
// refresh ends without the id reappearing, the final boundary clears the
// selection; a user selection made in between wins outright.
void MainWindowTest::selectionIntentSurvivesCrossTabMoveAcrossDeltas()
{
    // Clause 1: removal → unrelated delta → insertion re-adopts.
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        auto *locationLabel = fixture.window->findChild<QLabel *>("locationLabel");
        QVERIFY(tree);
        QVERIFY(locationLabel);

        const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
        const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
        const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
        Items items;
        items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
        items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
        items.push_back(makeMainWindowItem("item-c", "GammaItem", "Axe", tabC));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

        auto *model = tree->model();
        const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
        QVERIFY(bucketA.isValid());
        tree->expand(bucketA);
        const QModelIndex mover = model->index(0, 0, bucketA);
        QVERIFY(mover.isValid());
        QCOMPARE(mover.data().toString(), "Mover Sword");
        tree->selectionModel()->setCurrentIndex(mover,
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);

        QSignalSpy resets(model, &QAbstractItemModel::modelReset);

        // The removal delta: the visual selection lapses, the intent lives.
        fixture.itemsManager->OnTabRefreshed(tabA, {});
        QCOMPARE(resets.count(), 0);
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);

        // Several deltas later — an unrelated one in between.
        fixture.itemsManager
            ->OnTabRefreshed(tabC, {makeMainWindowItem("item-c", "GammaItem", "Axe", tabC)});
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);

        // The insertion in another tab: re-adoption by stable id.
        fixture.itemsManager
            ->OnTabRefreshed(tabB,
                             {makeMainWindowItem("item-x", "Mover", "Sword", tabB),
                              makeMainWindowItem("item-b", "BetaItem", "Shield", tabB)});
        QCOMPARE(resets.count(), 0);
        const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
        QCOMPARE(selectedRows.size(), 1);
        QCOMPARE(selectedRows.front().data().toString(), "Mover Sword");
        const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
        QVERIFY(bucketB.isValid());
        QCOMPARE(selectedRows.front().parent(), bucketB);
        // The details panel adopted the replacement object.
        QTRY_COMPARE_WITH_TIMEOUT(locationLabel->text(), tabB.GetHeader(), 2000);
    }

    // Clause 2: the refresh ends without the id reappearing — the final
    // boundary clears the selection, and a later refresh reinserting the
    // id does not resurrect it.
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        QVERIFY(tree);

        const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
        Items items;
        items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
        items.push_back(makeMainWindowItem("item-y", "Stayer", "Sword", tabA));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

        auto *model = tree->model();
        const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
        tree->expand(bucketA);
        const QModelIndex mover = findItemRow(*model, bucketA, "Mover Sword");
        QVERIFY(mover.isValid());
        tree->selectionModel()->setCurrentIndex(mover,
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);

        const auto stayer = makeMainWindowItem("item-y", "Stayer", "Sword", tabA);
        fixture.itemsManager->OnTabRefreshed(tabA, {stayer});
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);

        // The refresh completes; the id never reappeared. Revalidated
        // fallback-insensitively in S6: the clause also passed under the
        // temporary final-reset seam, so the probes must show the row
        // reconciliation — not a reset — performed the intent clearing.
        QSignalSpy final_resets(model, &QAbstractItemModel::modelReset);
        auto &probes = ModelProbes::instance();
        probes.reset();
        probes.enabled = true;
        Items final_items;
        final_items.push_back(stayer);
        fixture.itemsManager->OnItemsRefreshed(final_items, {tabA}, false);
        fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        QCOMPARE(final_resets.count(), 0);
        QCOMPARE(probes.final_reconciliations, 1);
        QCOMPARE(probes.refilters, 0);
        probes.enabled = false;
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);

        // A later refresh reinserting the id must not reselect it.
        fixture.itemsManager->OnTabRefreshed(tabA,
                                             {makeMainWindowItem("item-x", "Mover", "Sword", tabA),
                                              stayer});
        QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
    }

    // Clause 3: a user selection made mid-window wins outright.
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        QVERIFY(tree);

        const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
        const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
        Items items;
        items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
        items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);

        auto *model = tree->model();
        const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
        const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
        tree->expand(bucketA);
        tree->expand(bucketB);
        tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucketA, "Mover Sword"),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);

        // Removal lapses the selection; the user then picks another item.
        fixture.itemsManager->OnTabRefreshed(tabA, {});
        tree->selectionModel()->setCurrentIndex(findItemRow(*model,
                                                            findBucket(*model, tabB.GetHeader()),
                                                            "BetaItem Shield"),
                                                QItemSelectionModel::ClearAndSelect
                                                    | QItemSelectionModel::Rows);

        // The old id reappears: the user's selection stands.
        fixture.itemsManager->OnTabRefreshed(tabA,
                                             {makeMainWindowItem("item-x", "Mover", "Sword", tabA)});
        const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
        QCOMPARE(selectedRows.size(), 1);
        QCOMPARE(selectedRows.front().data().toString(), "BetaItem Shield");
    }
}

// M3 S4 (R2-1): deltas remove the selected item, then the refresh fails
// terminally — no final snapshot. The intent is cleared at the terminal
// event, and a later refresh reinserting the same id does not reselect it.
void MainWindowTest::selectionIntentClearsOnTerminalFailure()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-y", "Stayer", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);

    auto *model = tree->model();
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->expand(bucketA);
    const QModelIndex mover = findItemRow(*model, bucketA, "Mover Sword");
    QVERIFY(mover.isValid());
    tree->selectionModel()->setCurrentIndex(mover,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    // The removal delta lapses the selection; the intent is alive.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-y", "Stayer", "Sword", tabA)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);

    // Terminal failure: no final snapshot, and the intent window closes
    // with an absence check against the visible result.
    fixture.window->OnRefreshFinished(RefreshOutcome{FailedRefresh{RateLimit::FetchError{}}});

    // A later refresh reinserts the id: no reselection.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-x", "Mover", "Sword", tabA),
                                          makeMainWindowItem("item-y", "Stayer", "Sword", tabA)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
}

// M3 S4/S5 (R1-7), complete: after the active search applies (or
// correctly adjudicates) a series of deltas — in EITHER view mode —
// switching away and back triggers no refilter. The By-Item half closed
// in S5 with the fallback's deletion: the flat merge applies
// immediately, fallback-free, and the search stays clean.
void MainWindowTest::appliedDeltasLeaveActiveSearchClean()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tabs && tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    // A second search to switch through.
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(0);

    // The reset spy is scoped to THIS search's model: the background
    // search refilters on its own activation by design (rule 1), and that
    // must not count against the active search's cleanliness.
    QAbstractItemModel *first_model = tree->model();
    QSignalSpy first_resets(first_model, &QAbstractItemModel::modelReset);

    // A series the active By-Tab search processes: a content replacement,
    // an empty (removal) delta, and a metadata-only delta.
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA)});
    fixture.itemsManager->OnTabRefreshed(tabB, {});
    fixture.itemsManager->OnTabRefreshed(makeTestStashLocation("stash-bbbb", "Beta Renamed", 1), {});
    QCOMPARE(first_resets.count(), 0);

    // Switching away and back triggers no refilter: the deltas left the
    // search clean (M2 rule 1 renegotiated for the active search).
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(0);
    QCOMPARE(first_resets.count(), 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Two Sword"}));

    // The By-Item half (S5): the flat merge applies the delta
    // immediately — no reset, content fresh at once — and the search
    // stays just as clean, so the switch-away round trip still triggers
    // no refilter.
    switchToByItemView(fixture);
    const int resets_after_switch = first_resets.count();
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a3", "AlphaItem Three", "Sword", tabA)});
    QCOMPARE(first_resets.count(), resets_after_switch);
    QVERIFY(visibleItemNames(*tree).contains("AlphaItem Three Sword"));
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(0);
    QCOMPARE(first_resets.count(), resets_after_switch);
    QVERIFY(visibleItemNames(*tree).contains("AlphaItem Three Sword"));

    // S6: the final reconciliation is the R1-7 clearing boundary and
    // performs no reset itself, so the post-snapshot switch-away round
    // trip still refilters nothing.
    Items final_items;
    final_items.push_back(makeMainWindowItem("item-a3", "AlphaItem Three", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(final_items,
                                           {tabA,
                                            makeTestStashLocation("stash-bbbb", "Beta Renamed", 1)},
                                           false);
    QCOMPARE(first_resets.count(), resets_after_switch);
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(0);
    QCOMPARE(first_resets.count(), resets_after_switch);
    QVERIFY(visibleItemNames(*tree).contains("AlphaItem Three Sword"));
}

// M3 S4 review round 1: a filtered search hides empty buckets, so a
// bucket a delta empties leaves the view — the delta path converges to
// the freshly-refiltered state — and reappears when a delta brings
// matching items back. The unfiltered search keeps its empty row
// (emptyDeltaEmptiesBucketWithoutRemovingIt, unchanged).
void MainWindowTest::filteredSearchDropsEmptiedBucket()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    auto *nameLabel = fixture.window->findChild<QLabel *>("nameLabel");
    QVERIFY(tree && name && nameLabel);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Sword", tabB));
    items.push_back(makeMainWindowItem("item-c", "NomatchThing", "Sword", tabC));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);

    // Filter "item": C's only item is rejected, so the search is filtered
    // and C's empty bucket is hidden — two buckets visible.
    name->setFocus();
    QTest::keyClicks(name, "item");
    fixture.window->OnSearchFormChange();
    auto *model = tree->model();
    QCOMPARE(model->rowCount(), 2);

    // Select A's header so the drop also clears the details pane.
    const QModelIndex bucketA = findBucket(*model, tabA.GetHeader());
    QVERIFY(bucketA.isValid());
    tree->selectionModel()->setCurrentIndex(bucketA,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QCOMPARE(nameLabel->text(), tabA.GetHeader());

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy removals(model, &QAbstractItemModel::rowsRemoved);

    // The empty delta empties A: its rows leave, then its bucket row.
    fixture.itemsManager->OnTabRefreshed(tabA, {});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 2); // the child-row run, then the bucket row
    QCOMPARE(model->rowCount(), 1);
    QVERIFY(!findBucket(*model, tabA.GetHeader()).isValid());
    QCOMPARE(nameLabel->text(), "Select an item");

    // Matching arrivals bring the bucket back at its display position.
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA)});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->index(0, 0).data().toString(), tabA.GetHeader());

    // A replacement whose arrivals all fail the filter empties too.
    fixture.itemsManager
        ->OnTabRefreshed(tabB, {makeMainWindowItem("item-b2", "NomatchAxe", "Sword", tabB)});
    QCOMPARE(resets.count(), 0);
    QCOMPARE(model->rowCount(), 1);
    QVERIFY(!findBucket(*model, tabB.GetHeader()).isValid());

    // Round 2: "filtered" means ANY filter is active, not "something was
    // rejected" — the old snapshot could be flipped by one delta, a
    // whole-view change no bucket-scoped operation expresses. An active
    // filter that happens to reject nothing still hides empty tabs, and
    // a delta whose arrivals are rejected converges the same way.
    {
        MainWindowFixture fixture2;
        auto *tree2 = fixture2.window->findChild<QTreeView *>("treeView");
        auto *name2 = findNameFilter(*fixture2.window);
        QVERIFY(tree2 && name2);

        const ItemLocation tabD = makeTestStashLocation("stash-dddd", "Delta", 0);
        const ItemLocation tabE = makeTestStashLocation("stash-eeee", "Echo", 1);
        Items all_match;
        all_match.push_back(makeMainWindowItem("item-d", "DeltaItem", "Sword", tabD));
        fixture2.itemsManager->OnItemsRefreshed(all_match, {tabD, tabE}, false);

        // The filter matches every visible item: the search is filtered
        // regardless, so E's empty bucket is hidden.
        name2->setFocus();
        QTest::keyClicks(name2, "item");
        fixture2.window->OnSearchFormChange();
        auto *model2 = tree2->model();
        QCOMPARE(model2->rowCount(), 1);

        // A delta whose arrivals are all rejected empties D, and the
        // emptied bucket leaves — the flag cannot be flipped by a delta.
        QSignalSpy resets2(model2, &QAbstractItemModel::modelReset);
        fixture2.itemsManager
            ->OnTabRefreshed(tabD, {makeMainWindowItem("item-d2", "Nomatch", "Sword", tabD)});
        QCOMPARE(resets2.count(), 0);
        QCOMPARE(model2->rowCount(), 0);
    }
}

// M3 S4 review round 1 (R1-4): a metadata delta refreshes the selected
// presentation state the reset-reselect cycle used to refresh
// implicitly — a selected bucket header re-renders its pane, and a
// RETAINED item (a sibling source's rows untouched by the delta) renders
// its tab's canonical header on the location line.
void MainWindowTest::metadataDeltaRefreshesSelectedPresentation()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *nameLabel = fixture.window->findChild<QLabel *>("nameLabel");
    auto *locationLabel = fixture.window->findChild<QLabel *>("locationLabel");
    QVERIFY(tree && nameLabel && locationLabel);

    const ItemLocation parent = makeTestStashLocation("stash-pppp", "Maps", 0);
    ItemLocation child = parent;
    child.setFetchId("child-0001");
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-p", "ParentItem", "Sword", parent));
    items.push_back(makeMainWindowItem("item-m", "ChildItem", "Shield", child));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Sword", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {parent, tabB}, false);

    auto *model = tree->model();

    // A selected bucket header follows a rename in place.
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    QVERIFY(bucketB.isValid());
    tree->selectionModel()->setCurrentIndex(bucketB,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QCOMPARE(nameLabel->text(), tabB.GetHeader());
    const ItemLocation renamedB = makeTestStashLocation("stash-bbbb", "Beta Renamed", 1);
    fixture.itemsManager
        ->OnTabRefreshed(renamedB, {makeMainWindowItem("item-b", "BetaItem", "Sword", renamedB)});
    QCOMPARE(nameLabel->text(), renamedB.GetHeader());

    // A retained item renders the canonical header: the child delta
    // renames the parent's anchor without touching the parent-source
    // rows, and the selected ParentItem's location line follows.
    const QModelIndex parentBucket = findBucket(*model, parent.GetHeader());
    QVERIFY(parentBucket.isValid());
    tree->expand(parentBucket);
    const QModelIndex parentItem = findItemRow(*model, parentBucket, "ParentItem Sword");
    QVERIFY(parentItem.isValid());
    tree->selectionModel()->setCurrentIndex(parentItem,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QTRY_COMPARE_WITH_TIMEOUT(locationLabel->text(), parent.GetHeader(), 2000);

    ItemLocation renamedChild = makeTestStashLocation("stash-pppp", "Maps Renamed", 0);
    renamedChild.setFetchId("child-0001");
    fixture.itemsManager
        ->OnTabRefreshed(renamedChild,
                         {makeMainWindowItem("item-m2", "ChildItem Two", "Shield", renamedChild)});
    const ItemLocation renamedParent = makeTestStashLocation("stash-pppp", "Maps Renamed", 0);
    QCOMPARE(locationLabel->text(), renamedParent.GetHeader());
    // The selection never lapsed: the retained row was untouched.
    const QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "ParentItem Sword");
}

// M3 S5 (D4 rule 2): after any sequence of deltas — content
// replacements, a cross-tab move, a new-tab discovery, and a
// removal-only replacement — the flat bucket's order equals a
// from-scratch keyed sort of the same filtered collection, and no delta
// paid a reset.
void MainWindowTest::byItemMergeMatchesFullSort()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Mike One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "Bravo One", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-b2", "Zulu One", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    switchToByItemView(fixture);

    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);

    // A content replacement whose arrivals interleave with the retained
    // rows under the sort.
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA),
                          makeMainWindowItem("item-a3", "November One", "Sword", tabA)});
    // A replacement that also moves an item cross-tab into this source
    // (item-a2 left tabA in the previous delta and arrives in tabB).
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-b2", "Zulu One", "Shield", tabB),
                                          makeMainWindowItem("item-a2", "Mike One", "Shield", tabB)});
    // A tab discovered mid-refresh.
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    fixture.itemsManager->OnTabRefreshed(tabC,
                                         {makeMainWindowItem("item-c1", "Quebec One", "Axe", tabC)});
    // A removal-only replacement.
    fixture.itemsManager->OnTabRefreshed(tabA, {});

    QCOMPARE(resets.count(), 0);
    const QStringList merged = visibleItemNames(*tree);
    QCOMPARE(merged, QStringList({"Zulu One Shield", "Quebec One Axe", "Mike One Shield"}));

    // The from-scratch comparison: a full user refilter (D6) rebuilds and
    // keyed-sorts the same collection; the merged order must equal it.
    fixture.window->OnSearchFormChange();
    QCOMPARE(visibleItemNames(*tree), merged);
}

// M3 S5 (D4 rule 2, removal half): an empty replacement removes exactly
// the source's rows via contiguous row operations — no reset, no full
// re-sort, no key build — and the retained rows keep their sorted order.
void MainWindowTest::byItemRemovalOnlyDeltaErasesInPlace()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    // Names interleave under the descending sort so the erased source's
    // rows form multiple contiguous runs.
    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Golf One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a3", "Zeta One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "Echo One", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-b2", "Yankee One", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    switchToByItemView(fixture);
    QCOMPARE(visibleItemNames(*tree),
             QStringList({"Zeta One Sword",
                          "Yankee One Shield",
                          "Golf One Sword",
                          "Echo One Shield",
                          "Alpha One Sword"}));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy removals(tree->model(), &QAbstractItemModel::rowsRemoved);

    fixture.itemsManager->OnTabRefreshed(tabA, {});

    // Three scattered rows leave as three contiguous runs; nothing else
    // runs — the in-place erase preserved order and keys.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(removals.count(), 3);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);
    probes.enabled = false;
    QCOMPARE(visibleItemNames(*tree), QStringList({"Yankee One Shield", "Echo One Shield"}));
}

// M3 S5 (D4/R1-3): a selected item retains selection through a merge
// that moves its row; mid-refresh absence retains the intent and a
// cross-tab reinsertion re-adopts it; an id absent at the final
// reconciliation clears it, so a later refresh cannot reselect.
void MainWindowTest::byItemSelectionSurvivesMerge()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-x", "Mover", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-y", "Stayer", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    switchToByItemView(fixture);

    auto *model = tree->model();
    const QModelIndex mover = findItemRow(*model, model->index(0, 0), "Mover Sword");
    QVERIFY(mover.isValid());
    tree->selectionModel()->setCurrentIndex(mover,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);

    // A merge that inserts a row above the selection moves its row; the
    // selection follows.
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-b", "BetaItem", "Shield", tabB),
                                          makeMainWindowItem("item-z", "Zebra", "Shield", tabB)});
    QCOMPARE(resets.count(), 0);
    QModelIndexList selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "Mover Sword");

    // The removal lapses the visual selection; the intent stays alive
    // mid-refresh, and a cross-tab reinsertion re-adopts by stable id.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-y", "Stayer", "Sword", tabA)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-b", "BetaItem", "Shield", tabB),
                                          makeMainWindowItem("item-z", "Zebra", "Shield", tabB),
                                          makeMainWindowItem("item-x", "Mover", "Shield", tabB)});
    selectedRows = tree->selectionModel()->selectedRows();
    QCOMPARE(selectedRows.size(), 1);
    QCOMPARE(selectedRows.front().data().toString(), "Mover Shield");

    // Absent at the final reconciliation: the intent clears, and a later
    // refresh reinserting the id does not reselect. Revalidated
    // fallback-insensitively in S6 (probe: reset count zero, the
    // reconciliation entered) — the clause also passed under the
    // temporary final-reset seam.
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-b", "BetaItem", "Shield", tabB),
                                          makeMainWindowItem("item-z", "Zebra", "Shield", tabB)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    Items final_items;
    final_items.push_back(makeMainWindowItem("item-y", "Stayer", "Sword", tabA));
    final_items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    final_items.push_back(makeMainWindowItem("item-z", "Zebra", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(final_items, {tabA, tabB}, false);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.final_reconciliations, 1);
    QCOMPARE(probes.refilters, 0);
    probes.enabled = false;
    fixture.itemsManager->OnTabRefreshed(tabB,
                                         {makeMainWindowItem("item-b", "BetaItem", "Shield", tabB),
                                          makeMainWindowItem("item-z", "Zebra", "Shield", tabB),
                                          makeMainWindowItem("item-x", "Mover", "Shield", tabB)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
}

// M3 S5 (R3-1): reactivation decides dirtiness first. A clean By-Item
// search hydrates its flat bucket's keys eagerly at activation with no
// refilter, and the first delta then merges without any key build; a
// dirty one refilters exactly once and that sort supplies the keys — no
// separate hydration before or after.
void MainWindowTest::byItemActivationDecidesDirtinessFirst()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tabs && tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Bravo One", "Sword", tabA));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
    switchToByItemView(fixture);
    // A second search to switch through.
    tabs->setCurrentIndex(1);

    auto &probes = ModelProbes::instance();

    // Clean reactivation: one key build (the eager hydration), no
    // refilter.
    probes.reset();
    probes.enabled = true;
    tabs->setCurrentIndex(0);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.key_builds, 1);
    QVERIFY(probes.live_key_bytes > 0);

    // The first delta merges against the resident keys: zero key builds
    // during application, and the merge lands immediately.
    probes.reset();
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA),
                          makeMainWindowItem("item-a3", "Charlie One", "Sword", tabA)});
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(visibleItemNames(*tree), QStringList({"Charlie One Sword", "Alpha One Sword"}));

    // Dirty reactivation: the background delta marked this search dirty;
    // activation refilters exactly once and the sort supplies the keys —
    // one key build total, no separate hydration.
    tabs->setCurrentIndex(1);
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA),
                                          makeMainWindowItem("item-a4", "Delta One", "Sword", tabA)});
    probes.reset();
    tabs->setCurrentIndex(0);
    QCOMPARE(probes.refilters, 1);
    QCOMPARE(probes.key_builds, 1);
    probes.enabled = false;
    QCOMPARE(visibleItemNames(*tree), QStringList({"Delta One Sword", "Alpha One Sword"}));
}

// M3 S5 (renegotiated from S4's modeSwitchConsumesFallbackDirtiness —
// the fallback dirtiness died with the seam): deltas applied while
// By-Item is active render in By-Tab after a mode switch, content from
// the rebuilt tab buckets and metadata from the canonical inventory,
// with exactly the switch's own single reset — no refilter, even after
// a terminal failure.
void MainWindowTest::modeSwitchRendersAppliedDeltas()
{
    // Clause 1: content applied by the flat merge survives the switch.
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
        QVERIFY(tree && viewCombo);

        const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
        Items items;
        items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA}, false);
        switchToByItemView(fixture);

        // The delta applies immediately (no fallback, no staleness).
        fixture.itemsManager
            ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA)});
        QVERIFY(visibleItemNames(*tree).contains("AlphaItem Two Sword"));
        QVERIFY(!visibleItemNames(*tree).contains("AlphaItem Sword"));

        // The switch to By-Tab rebuilds the tab buckets from the
        // maintained flat collection: one reset (the switch's own), no
        // refilter, applied content rendered.
        auto *model = tree->model();
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        viewCombo->setCurrentIndex(0);
        emit viewCombo->activated(0);
        QCOMPARE(resets.count(), 1);
        QVERIFY(visibleItemNames(*tree).contains("AlphaItem Two Sword"));
        QVERIFY(!visibleItemNames(*tree).contains("AlphaItem Sword"));
    }

    // Clause 2: a metadata-only delta lands through the canonical
    // inventory at the rebuild, terminal failure notwithstanding.
    {
        MainWindowFixture fixture;
        auto *tree = fixture.window->findChild<QTreeView *>("treeView");
        auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
        QVERIFY(tree && viewCombo);

        const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
        const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
        Items items;
        items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
        fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
        switchToByItemView(fixture);

        const ItemLocation renamedB = makeTestStashLocation("stash-bbbb", "Beta Renamed", 1);
        fixture.itemsManager->OnTabRefreshed(renamedB, {});
        fixture.window->OnRefreshFinished(RefreshOutcome{FailedRefresh{RateLimit::FetchError{}}});

        auto *model = tree->model();
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        viewCombo->setCurrentIndex(0);
        emit viewCombo->activated(0);
        QCOMPARE(resets.count(), 1);
        QVERIFY(findBucket(*tree->model(), renamedB.GetHeader()).isValid());
        QVERIFY(!findBucket(*tree->model(), tabB.GetHeader()).isValid());
    }
}

// M3 S5 review round 1 (batching rule 5 refined): affected rows repaint
// as MAXIMAL contiguous runs — never one first-to-last spanning
// rectangle, which cost O(collection) view-side work per priced delta
// once the flat bucket made the span the whole collection. In By-Item a
// tab-level change resolves to the tab's rows by each item's location,
// never the whole flat bucket; only `everything` repaints a full
// bucket.
void MainWindowTest::buyoutRepaintScopesToAffectedRuns()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    // Names interleave under the descending sort so tab A's rows scatter
    // through tab B's: Zeta(A) Yankee(B) Golf(A) Echo(B) Alpha(A).
    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const auto a1 = makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA);
    const auto a2 = makeMainWindowItem("item-a2", "Golf One", "Sword", tabA);
    const auto a3 = makeMainWindowItem("item-a3", "Zeta One", "Sword", tabA);
    Items items{a1,
                a2,
                a3,
                makeMainWindowItem("item-b1", "Echo One", "Shield", tabB),
                makeMainWindowItem("item-b2", "Yankee One", "Shield", tabB)};
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    switchToByItemView(fixture);

    auto *model = tree->model();
    const QModelIndex flat = model->index(0, 0);
    QCOMPARE(bucketItemNames(*model, flat),
             QStringList({"Zeta One Sword",
                          "Yankee One Shield",
                          "Golf One Sword",
                          "Echo One Shield",
                          "Alpha One Sword"}));
    QSignalSpy repaints(model, &QAbstractItemModel::dataChanged);

    const auto affectedRows = [&repaints, &flat]() {
        std::set<int> rows;
        for (int n = 0; n < repaints.count(); ++n) {
            const QModelIndex topLeft = repaints.at(n).at(0).toModelIndex();
            const QModelIndex bottomRight = repaints.at(n).at(1).toModelIndex();
            if (topLeft.parent() != flat) {
                continue;
            }
            for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                rows.insert(row);
            }
        }
        return rows;
    };

    // Scattered item-level changes: one batch pricing tab A's three
    // scattered rows emits three single-row rectangles, not one span
    // covering the interleaved B rows.
    {
        const BuyoutBatch batch(*fixture.buyoutFixture.manager);
        fixture.buyoutFixture.manager->Set(*a1, makeChaosBuyout(3));
        fixture.buyoutFixture.manager->Set(*a2, makeChaosBuyout(4));
        fixture.buyoutFixture.manager->Set(*a3, makeChaosBuyout(5));
    }
    QCOMPARE(repaints.count(), 3);
    for (int n = 0; n < repaints.count(); ++n) {
        QCOMPARE(repaints.at(n).at(0).toModelIndex().row(),
                 repaints.at(n).at(1).toModelIndex().row());
    }
    QCOMPARE(affectedRows(), (std::set<int>{0, 2, 4}));

    // A tab-level change resolves to the tab's rows by item location:
    // exactly tab B's two scattered rows, no whole-bucket rectangle and
    // no header emission (the flat header renders no tab buyout).
    repaints.clear();
    fixture.buyoutFixture.manager->SetTab(tabB, makeChaosBuyout(7));
    QCOMPARE(repaints.count(), 2);
    for (int n = 0; n < repaints.count(); ++n) {
        QVERIFY(repaints.at(n).at(0).toModelIndex().parent() == flat);
        QVERIFY(repaints.at(n).at(0).toModelIndex().column() >= 1);
    }
    QCOMPARE(affectedRows(), (std::set<int>{1, 3}));

    // `everything` is the one shape that repaints the full bucket, as a
    // single rectangle.
    repaints.clear();
    fixture.buyoutFixture.manager->ReloadBuyouts();
    QCOMPARE(repaints.count(), 1);
    QCOMPARE(repaints.at(0).at(0).toModelIndex().row(), 0);
    QCOMPARE(repaints.at(0).at(1).toModelIndex().row(), 4);
}

// M3 S5 remedy A′ gate: shrink, equal, grow, empty, and removal-only
// replacements — every one with removal and insertion runs interleaved
// through retained rows — run under QAbstractItemModelTester, with a
// live selection and a persistent index surviving throughout, no reset,
// and the end state equal to a from-scratch refilter.
void MainWindowTest::byItemReplaceSatisfiesModelTester()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    ItemLocation child_fetch = tabB;
    child_fetch.setFetchId("child-0001");
    Items items;
    items.push_back(makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a2", "Golf One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-a3", "Zeta One", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b1", "Echo One", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-b2", "Yankee One", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-g", "Ghost One", "Shield", child_fetch));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    switchToByItemView(fixture);

    auto *model = tree->model();
    QAbstractItemModelTester tester(model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    QSignalSpy resets(model, &QAbstractItemModel::modelReset);

    // A surviving tab-B row keeps the selection; a persistent index on
    // another tab-B row must track every shuffle.
    const QModelIndex flat = model->index(0, 0);
    const QModelIndex echo = findItemRow(*model, flat, "Echo One Shield");
    QVERIFY(echo.isValid());
    tree->selectionModel()->setCurrentIndex(echo,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QPersistentModelIndex yankee(findItemRow(*model, flat, "Yankee One Shield"));
    QVERIFY(yankee.isValid());

    const auto stillCoherent = [&]() -> bool {
        if (resets.count() != 0) {
            return false;
        }
        const QModelIndexList selected = tree->selectionModel()->selectedRows();
        if ((selected.size() != 1) || (selected.front().data().toString() != "Echo One Shield")) {
            return false;
        }
        return yankee.isValid() && (yankee.data().toString() == "Yankee One Shield");
    };

    // Grow: 3 -> 5, arrivals interleaving above, between, and below the
    // retained tab-B rows.
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {makeMainWindowItem("item-a1", "Alpha One", "Sword", tabA),
                                          makeMainWindowItem("item-a4", "Bravo One", "Sword", tabA),
                                          makeMainWindowItem("item-a5", "Hotel One", "Sword", tabA),
                                          makeMainWindowItem("item-a6", "Xray One", "Sword", tabA),
                                          makeMainWindowItem("item-a3", "Zeta One", "Sword", tabA)});
    QVERIFY2(stillCoherent(), "grow");

    // Equal cardinality, fully different rows.
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a7", "Charlie One", "Sword", tabA),
                          makeMainWindowItem("item-a8", "Foxtrot One", "Sword", tabA),
                          makeMainWindowItem("item-a9", "India One", "Sword", tabA),
                          makeMainWindowItem("item-a10", "Victor One", "Sword", tabA),
                          makeMainWindowItem("item-a11", "Whiskey One", "Sword", tabA)});
    QVERIFY2(stillCoherent(), "equal");

    // Shrink: 5 -> 2.
    fixture.itemsManager
        ->OnTabRefreshed(tabA,
                         {makeMainWindowItem("item-a12", "Delta One", "Sword", tabA),
                          makeMainWindowItem("item-a13", "Uniform One", "Sword", tabA)});
    QVERIFY2(stillCoherent(), "shrink");

    // Removal-only via the aggregate reconciliation: the ghost's child
    // source leaves tab B's rows in place.
    fixture.itemsManager->OnChildrenReconciled(tabB,
                                               {FetchSourceKey{ItemLocationType::STASH,
                                                               "stash-bbbb"}});
    QVERIFY(!findItemRow(*model, model->index(0, 0), "Ghost One Shield").isValid());
    QVERIFY2(stillCoherent(), "reconcile");

    // Empty replacement.
    fixture.itemsManager->OnTabRefreshed(tabA, {});
    QVERIFY2(stillCoherent(), "empty");
    QCOMPARE(visibleItemNames(*tree), QStringList({"Yankee One Shield", "Echo One Shield"}));

    // The end state equals a from-scratch refilter of the same
    // collection.
    const QStringList applied = visibleItemNames(*tree);
    fixture.window->OnSearchFormChange();
    QCOMPARE(visibleItemNames(*tree), applied);
}

void MainWindowTest::noModelResetDuringRefresh()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    items.push_back(makeMainWindowItem("item-c", "GammaItem", "Axe", tabC));
    // Initial population takes the one legitimate refresh-boundary reset.
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, true);

    auto *model = tree->model();
    tree->expand(findBucket(*model, tabA.GetHeader()));

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    QSignalSpy moves(model, &QAbstractItemModel::rowsMoved);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // A complete refresh with the window open: content deltas, an empty
    // delta, a metadata delta, a child reconciliation, the final
    // snapshot (which also deletes a tab — snapshot-only work), and the
    // terminal event. The delta and snapshot share Item objects, the way
    // the worker publishes them.
    const auto alpha_fresh = makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA);
    const auto beta_fresh = makeMainWindowItem("item-b", "BetaItem", "Shield", tabB);
    fixture.itemsManager->OnTabRefreshed(tabA, {alpha_fresh});
    fixture.itemsManager->OnTabRefreshed(tabB, {beta_fresh});
    fixture.itemsManager->OnTabRefreshed(tabC, {});
    const ItemLocation tabB_renamed = makeTestStashLocation("stash-bbbb", "Beta Renamed", 1);
    fixture.itemsManager->OnTabRefreshed(tabB_renamed, {});
    fixture.itemsManager->OnChildrenReconciled(tabA, {FetchSourceKey::ForLocation(tabA)});
    Items final_items;
    final_items.push_back(alpha_fresh);
    final_items.push_back(beta_fresh);
    fixture.itemsManager->OnItemsRefreshed(final_items, {tabA, tabB_renamed}, false);
    fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});

    // Zero resets on the current search's model — and none anywhere in
    // the window (probes.model_resets counts every beginResetModel); the
    // final snapshot performed only the row reconciliation, no refilter.
    // Zero moves too: the tabs kept their positions, so the reconciled
    // order pass must take its is_sorted fast path (S6 review round 1),
    // never a repositioning move.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(moves.count(), 0);
    QCOMPARE(probes.model_resets, 0);
    QCOMPARE(probes.refilters, 0);
    QCOMPARE(probes.final_reconciliations, 1);
    probes.enabled = false;

    // The refresh's whole effect landed through row operations.
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Two Sword", "BetaItem Shield"}));
    QVERIFY(findBucket(*model, tabB_renamed.GetHeader()).isValid());
    QVERIFY(!findBucket(*model, tabC.GetHeader()).isValid());
}

void MainWindowTest::finalReconciliationRemovesDeletedTabs()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tabs && tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, true);

    auto *model = tree->model();
    const QModelIndex bucketB = findBucket(*model, tabB.GetHeader());
    tree->expand(bucketB);
    // Select the doomed tab's item, so the boundary's index answer is
    // observable through the selection contract.
    tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucketB, "BetaItem Shield"),
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // The refresh fetches tab A only; no delta ever removes tab B — its
    // deletion is snapshot-boundary work (M2 D6).
    const auto alpha_fresh = makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA);
    fixture.itemsManager->OnTabRefreshed(tabA, {alpha_fresh});
    fixture.itemsManager->OnItemsRefreshed({alpha_fresh}, {tabA}, false);
    fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});

    // The reconciliation — not a reset — removed the bucket and its rows.
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.final_reconciliations, 1);
    probes.enabled = false;
    QVERIFY(!findBucket(*model, tabB.GetHeader()).isValid());
    QCOMPARE(model->rowCount(), 1);

    // Its items left the visible indexes: the maintained count renders,
    // and the closed selection intent cannot resurrect the id when a
    // later refresh reinserts it.
    QCOMPARE(tabs->tabText(0), "Search 1 [1]");
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
    fixture.itemsManager->OnTabRefreshed(tabA,
                                         {alpha_fresh,
                                          makeMainWindowItem("item-b", "BetaItem", "Shield", tabA)});
    QCOMPARE(tree->selectionModel()->selectedRows().size(), 0);
}

void MainWindowTest::finalReconciliationInsertsNewlyListedEmptyTabs()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(tree);
    QVERIFY(name);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabC = makeTestStashLocation("stash-cccc", "Gamma", 2);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-c", "GammaItem", "Sword", tabC));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabC}, true);

    auto *model = tree->model();
    QCOMPARE(model->rowCount(), 2);

    QSignalSpy resets(model, &QAbstractItemModel::modelReset);
    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // A newly discovered, never-fetched tab exists only in the final
    // snapshot's published list: the unfiltered search shows it as an
    // empty bucket at its display position.
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC}, false);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(probes.final_reconciliations, 1);
    probes.enabled = false;
    QCOMPARE(model->rowCount(), 3);
    QVERIFY(model->index(1, 0).data().toString().startsWith(tabB.GetHeader()));
    QCOMPARE(model->rowCount(model->index(1, 0)), 0);

    // A filtered search continues to hide empty buckets — the known
    // empty tab and a newly listed one alike.
    name->setFocus();
    QTest::keyClicks(name, "sword");
    fixture.window->OnSearchFormChange();
    QVERIFY(!findBucket(*model, tabB.GetHeader()).isValid());

    QSignalSpy filtered_resets(model, &QAbstractItemModel::modelReset);
    const ItemLocation tabD = makeTestStashLocation("stash-dddd", "Delta", 3);
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB, tabC, tabD}, false);
    QCOMPARE(filtered_resets.count(), 0);
    QVERIFY(!findBucket(*model, tabB.GetHeader()).isValid());
    QVERIFY(!findBucket(*model, tabD.GetHeader()).isValid());
    QCOMPARE(visibleItemNames(*tree), QStringList({"AlphaItem Sword", "GammaItem Sword"}));
}

void MainWindowTest::modelTesterPassesUnderDeltaStorm()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(tree);
    QVERIFY(viewCombo);

    // The world is tracked at the fetch-source grain, the way
    // ItemsManager publishes it (M2 D6): each tab's entry is what the
    // last delta for that source carried, so a snapshot mid cross-tab
    // move legitimately publishes a duplicated id — the divergence the
    // index machinery exists for.
    struct TabState
    {
        QString id;
        QString label;
        int position;
        Items items;
    };
    std::vector<TabState> world;
    int tab_counter = 0;
    int item_counter = 0;
    const auto locationFor = [](const TabState &tab) {
        return makeTestStashLocation(tab.id, tab.label, tab.position);
    };
    const auto makeStormItem = [&](const QString &id, const TabState &tab) {
        return makeMainWindowItem(id, QString("Item %1").arg(id), "Sword", locationFor(tab));
    };
    const auto newTab = [&] {
        TabState tab;
        tab.id = QString("storm-%1").arg(tab_counter);
        tab.label = QString("Storm %1").arg(tab_counter);
        tab.position = tab_counter;
        ++tab_counter;
        return tab;
    };
    const auto flatten = [&] {
        Items all;
        for (const auto &tab : world) {
            all.insert(all.end(), tab.items.begin(), tab.items.end());
        }
        return all;
    };
    const auto tabList = [&] {
        std::vector<ItemLocation> tabs;
        for (const auto &tab : world) {
            tabs.push_back(locationFor(tab));
        }
        return tabs;
    };

    for (int n = 0; n < 5; ++n) {
        world.push_back(newTab());
        for (int k = 0; k < 2; ++k) {
            world.back().items.push_back(
                makeStormItem(QString("it-%1").arg(item_counter++), world.back()));
        }
    }
    fixture.itemsManager->OnItemsRefreshed(flatten(), tabList(), true);

    // The tester rides the current search's model through the whole
    // storm; FailureReportingMode::QtTest turns any protocol violation
    // into a test failure at the offending operation.
    QAbstractItemModelTester tester(tree->model(),
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);

    std::mt19937 rng(20260731u);
    const auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    for (int step = 0; step < 240; ++step) {
        switch (pick(12)) {
        case 0:
        case 1:
        case 2: {
            // Content delta — occasionally migrating an id from another
            // tab's source (the cross-tab divergence).
            TabState &tab = world[static_cast<size_t>(pick(static_cast<int>(world.size())))];
            Items fresh;
            const int count = pick(4);
            for (int n = 0; n < count; ++n) {
                fresh.push_back(makeStormItem(QString("it-%1").arg(item_counter++), tab));
            }
            if ((count > 0) && (pick(4) == 0)) {
                const TabState &other
                    = world[static_cast<size_t>(pick(static_cast<int>(world.size())))];
                if ((&other != &tab) && !other.items.empty()) {
                    fresh.push_back(makeStormItem(other.items.front()->id(), tab));
                }
            }
            tab.items = fresh;
            fixture.itemsManager->OnTabRefreshed(locationFor(tab), fresh);
            break;
        }
        case 3: {
            // Empty delta: the fetch source empties, nothing else.
            TabState &tab = world[static_cast<size_t>(pick(static_cast<int>(world.size())))];
            tab.items.clear();
            fixture.itemsManager->OnTabRefreshed(locationFor(tab), {});
            break;
        }
        case 4: {
            // Metadata delta: a fresh anchor (rename or move) carried by
            // a re-fetch of the tab's current content (R1-4's metadata
            // half applies before the content half).
            TabState &tab = world[static_cast<size_t>(pick(static_cast<int>(world.size())))];
            if (pick(2) == 0) {
                tab.label += QStringLiteral("~");
            } else {
                tab.position = pick(10);
            }
            fixture.itemsManager->OnTabRefreshed(locationFor(tab), tab.items);
            break;
        }
        case 5: {
            // Child reconciliation: expected-set erase under the parent's
            // stable key — the no-op form and the erase-everything form.
            TabState &tab = world[static_cast<size_t>(pick(static_cast<int>(world.size())))];
            if (pick(2) == 0) {
                fixture.itemsManager->OnChildrenReconciled(locationFor(tab),
                                                           {FetchSourceKey::ForLocation(
                                                               locationFor(tab))});
            } else {
                tab.items.clear();
                fixture.itemsManager->OnChildrenReconciled(locationFor(tab), {});
            }
            break;
        }
        case 6: {
            // New-tab discovery mid-refresh (R6-1): an empty delta whose
            // anchor no search has seen.
            if (world.size() < 9) {
                world.push_back(newTab());
                fixture.itemsManager->OnTabRefreshed(locationFor(world.back()), {});
            }
            break;
        }
        case 7:
        case 8: {
            // Final reconciliation — sometimes with a tab deletion, a
            // position rebase, or a newly listed unfetched tab, the
            // snapshot-only mutations (M2 D6).
            if ((pick(3) == 0) && (world.size() > 2)) {
                world.erase(world.begin() + pick(static_cast<int>(world.size())));
            }
            if (pick(3) == 0) {
                world[static_cast<size_t>(pick(static_cast<int>(world.size())))].position = pick(10);
            }
            if ((pick(4) == 0) && (world.size() < 9)) {
                world.push_back(newTab());
            }
            fixture.itemsManager->OnItemsRefreshed(flatten(), tabList(), false);
            fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
            break;
        }
        case 9: {
            // Expansion change.
            auto *model = tree->model();
            if (model->rowCount() > 0) {
                const QModelIndex bucket = model->index(pick(model->rowCount()), 0);
                if (tree->isExpanded(bucket)) {
                    tree->collapse(bucket);
                } else {
                    tree->expand(bucket);
                }
            }
            break;
        }
        case 10: {
            // Sort click.
            static constexpr int columns[] = {0, 1, 2, 4};
            tree->sortByColumn(columns[pick(4)],
                               (pick(2) == 0) ? Qt::AscendingOrder : Qt::DescendingOrder);
            break;
        }
        case 11: {
            // View-mode switch through the user entry (no-op re-selects
            // included).
            const int mode = pick(2);
            viewCombo->setCurrentIndex(mode);
            emit viewCombo->activated(mode);
            break;
        }
        }
    }

    // Settle deterministically in By-Tab — bucket ownership is the
    // structural claim — then check coherence: after a last snapshot,
    // the maintained model matches a from-scratch refilter of the
    // published collection STRUCTURALLY — top-level order, bucket
    // headers (metadata), and per-bucket membership (S6 review round 1:
    // a globally sorted name list could not see an item parked in the
    // wrong bucket, a stale header, or a misordered tab). Within-bucket
    // order for collapsed buckets is deliberately lazy (D2), so each
    // bucket compares membership, not sequence.
    viewCombo->setCurrentIndex(0);
    emit viewCombo->activated(0);
    fixture.itemsManager->OnItemsRefreshed(flatten(), tabList(), false);
    fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});

    const auto structuralSignature = [&tree] {
        const QAbstractItemModel *model = tree->model();
        QStringList signature;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex bucket = model->index(row, 0);
            QStringList members = bucketItemNames(*model, bucket);
            members.sort();
            signature.append(bucket.data().toString() + " | " + members.join(", "));
        }
        return signature;
    };
    const QStringList applied = structuralSignature();
    QVERIFY(!applied.isEmpty());

    // The storm actually stormed (the distribution is stdlib-dependent,
    // so floors, not exact counts) — and the delta path never once fell
    // back to a refilter: the single refilter below is the verification
    // pass itself.
    QVERIFY2(probes.final_reconciliations >= 10, "snapshot boundary underexercised");
    QCOMPARE(probes.refilters, 0);

    fixture.window->OnSearchFormChange();
    QCOMPARE(probes.refilters, 1);
    probes.enabled = false;
    QCOMPARE(structuralSignature(), applied);
}

// S7 review round 1 (finding 2): every applied delta used to arm the
// 0 ms resize coalescer, so a refresh burst paid one ~10 ms
// ResizeTreeColumns pass per reply (S7 record, M2-M2 addendum). The
// delta-path arming is now a non-resetting single-shot debounce. Two
// clauses: a burst of applied deltas within the interval yields
// exactly ONE resize pass, and an immediate resize (user action or
// final reconciliation) supersedes a pending debounced one instead of
// being followed by a redundant second pass.
void MainWindowTest::deltaResizeDebounceCoalescesBurst()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation tabA = makeTestStashLocation("stash-aaaa", "Alpha", 0);
    const ItemLocation tabB = makeTestStashLocation("stash-bbbb", "Beta", 1);
    Items items;
    items.push_back(makeMainWindowItem("item-a", "AlphaItem", "Sword", tabA));
    items.push_back(makeMainWindowItem("item-b", "BetaItem", "Shield", tabB));
    fixture.itemsManager->OnItemsRefreshed(items, {tabA, tabB}, false);
    // The seeding reconciliation schedules an immediate resize (0 ms
    // coalescer); let it fire before counting.
    QTest::qWait(50);

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;

    // Clause 1: two model-changing deltas in SEPARATE event-loop turns
    // (each production reply is its own turn — a same-turn pair would be
    // coalesced even by the old 0 ms arming and prove nothing). Inside
    // the interval neither has resized yet; then exactly one debounced
    // pass, and no second one after another full interval.
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a2", "AlphaItem Two", "Sword", tabA)});
    QTest::qWait(50);
    fixture.itemsManager
        ->OnTabRefreshed(tabB, {makeMainWindowItem("item-b2", "BetaItem Two", "Shield", tabB)});
    QTest::qWait(50);
    QCOMPARE(probes.column_resizes, 0);
    QTRY_COMPARE_WITH_TIMEOUT(probes.column_resizes, 1, 2000);
    QTest::qWait(400);
    QCOMPARE(probes.column_resizes, 1);

    // Clause 2: a delta arms the debounce, then a user refilter's
    // immediate resize runs first and supersedes it — the debounce
    // deadline passes with no third pass.
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a3", "AlphaItem Three", "Sword", tabA)});
    QCOMPARE(probes.column_resizes, 1);
    fixture.window->OnSearchFormChange();
    QTRY_COMPARE_WITH_TIMEOUT(probes.column_resizes, 2, 2000);
    QTest::qWait(400);
    QCOMPARE(probes.column_resizes, 2);

    // Clause 3 (the queued-timeout race): the debounce deadline expires
    // while the event loop is blocked (qSleep processes nothing), so its
    // timeout is queued when the immediate schedule happens. Supersession
    // must act at scheduling time — a stop only inside ResizeTreeColumns
    // runs after the queued timeout already fired, two passes.
    fixture.itemsManager
        ->OnTabRefreshed(tabA, {makeMainWindowItem("item-a4", "AlphaItem Four", "Sword", tabA)});
    QCOMPARE(probes.column_resizes, 2);
    QTest::qSleep(400); // deadline passes unprocessed
    fixture.window->ScheduleResizeTreeColumns();
    QTest::qWait(400); // both timers would fire here without the fix
    QCOMPARE(probes.column_resizes, 3);
    probes.enabled = false;
}

// Answers the next modal QInputDialog from the event loop: `answer` runs
// against it, then it is accepted or rejected. The dialog's bounds are
// reported back for the caller to check.
struct IntDialogSeen
{
    bool seen{false};
    int value{-1};
    int minimum{-1};
    int maximum{-1};
};

static void answerNextIntDialog(IntDialogSeen &seen, int new_value, bool accept)
{
    QTimer::singleShot(0, [&seen, new_value, accept] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            return;
        }
        seen.seen = true;
        seen.value = dialog->intValue();
        seen.minimum = dialog->intMinimum();
        seen.maximum = dialog->intMaximum();
        dialog->setIntValue(new_value);
        if (accept) {
            dialog->accept();
        } else {
            dialog->reject();
        }
    });
}

// #276: the dialog opens on the current interval with 1 as its minimum, so
// the interval can be lowered as well as raised.
void MainWindowTest::refreshIntervalDialogEditsCurrentValue()
{
    MainWindowFixture fixture;
    auto *action = fixture.window->findChild<QAction *>("actionSetTabRefreshInterval");
    QVERIFY(action);
    fixture.settings->setValue("autoupdate_interval", 45);

    IntDialogSeen seen;
    answerNextIntDialog(seen, 12, true);
    action->trigger();

    QVERIFY(seen.seen);
    QCOMPARE(seen.value, 45);
    QCOMPARE(seen.minimum, 1);
    QVERIFY(seen.maximum >= 45);
    QCOMPARE(fixture.settings->value("autoupdate_interval").toInt(), 12);
}

void MainWindowTest::refreshIntervalDialogCancelChangesNothing()
{
    MainWindowFixture fixture;
    auto *action = fixture.window->findChild<QAction *>("actionSetTabRefreshInterval");
    QVERIFY(action);
    fixture.settings->setValue("autoupdate_interval", 45);

    IntDialogSeen seen;
    answerNextIntDialog(seen, 12, false);
    action->trigger();

    QVERIFY(seen.seen);
    QCOMPARE(fixture.settings->value("autoupdate_interval").toInt(), 45);
}

// #284: the theme actions are exclusive; clicking the active one keeps it
// checked, and choosing another unchecks the rest.
void MainWindowTest::activeThemeStaysCheckedWhenClicked()
{
    MainWindowFixture fixture;
    auto *dark = fixture.window->findChild<QAction *>("actionSetDarkTheme");
    auto *light = fixture.window->findChild<QAction *>("actionSetLightTheme");
    auto *platform = fixture.window->findChild<QAction *>("actionSetDefaultTheme");
    QVERIFY(dark);
    QVERIFY(light);
    QVERIFY(platform);

    QVERIFY(platform->isChecked());
    platform->trigger();
    QVERIFY(platform->isChecked());
    QVERIFY(!dark->isChecked());
    QVERIFY(!light->isChecked());

    dark->trigger();
    QVERIFY(dark->isChecked());
    QVERIFY(!platform->isChecked());
    QVERIFY(!light->isChecked());
    QCOMPARE(fixture.settings->value("theme").toString(), QString("dark"));
    dark->trigger();
    QVERIFY(dark->isChecked());
    QCOMPARE(fixture.settings->value("theme").toString(), QString("dark"));
}

void MainWindowTest::activeLoggingLevelStaysCheckedWhenClicked()
{
    MainWindowFixture fixture;
    const auto level = spdlog::get_level();
    auto *warn = fixture.window->findChild<QAction *>("actionLoggingWARN");
    auto *info = fixture.window->findChild<QAction *>("actionLoggingINFO");
    QVERIFY(warn);
    QVERIFY(info);

    warn->trigger();
    QVERIFY(warn->isChecked());
    warn->trigger();
    QVERIFY(warn->isChecked());
    QVERIFY(!info->isChecked());
    info->trigger();
    QVERIFY(info->isChecked());
    QVERIFY(!warn->isChecked());
    spdlog::set_level(level);
}

// Selects the one item of a one-tab refresh and sets the buyout type and
// currency the way a user would, leaving the value box enabled and empty.
static std::shared_ptr<Item> selectItemForPricing(MainWindowFixture &fixture)
{
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    if (!tree) {
        qFatal("selectItemForPricing: treeView not found");
    }
    const ItemLocation tab = makeTestStashLocation("stash-price", "Price Tab", 0);
    auto item = makeMainWindowItem("price-item", "Priced", "Sword", tab);
    Items items;
    items.push_back(item);
    fixture.itemsManager->OnItemsRefreshed(items, {tab}, false);
    const QModelIndex bucket = findBucket(*tree->model(), tab.GetHeader());
    const QModelIndex row = findItemRow(*tree->model(), bucket, "Priced Sword");
    if (!row.isValid()) {
        qFatal("selectItemForPricing: the item row is missing");
    }
    tree->selectionModel()->setCurrentIndex(row,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    return item;
}

// #252: text that is not a number greater than zero writes nothing, even
// when editing finishes.
void MainWindowTest::priceBoxIgnoresTextThatIsNotAPrice_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("ratio") << "79/20";
    QTest::newRow("letters") << "abc";
    QTest::newRow("zero") << "0";
    QTest::newRow("thousands comma") << "1,000";
    QTest::newRow("negative") << "-3";
    QTest::newRow("lone point") << ".";
}

void MainWindowTest::priceBoxIgnoresTextThatIsNotAPrice()
{
    QFETCH(QString, text);
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(value);
    QVERIFY(value->isEnabled());
    QVERIFY(fixture.buyoutFixture.manager->Get(*item).IsNull());

    QSignalSpy changes(fixture.buyoutFixture.manager.get(), &BuyoutManager::BuyoutsChanged);
    QTest::keyClicks(value, text);
    QTest::keyClick(value, Qt::Key_Return);

    QCOMPARE(value->text(), text);
    QVERIFY(fixture.buyoutFixture.manager->Get(*item).IsNull());
    QCOMPARE(changes.count(), 0);
}

void MainWindowTest::priceBoxAcceptsDecimalPointOrComma_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("point") << "2.5";
    QTest::newRow("comma") << "2,5";
}

void MainWindowTest::priceBoxAcceptsDecimalPointOrComma()
{
    QFETCH(QString, text);
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(value);

    QTest::keyClicks(value, text);
    QTest::keyClick(value, Qt::Key_Return);

    const Buyout stored = fixture.buyoutFixture.manager->Get(*item);
    QCOMPARE(stored.type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(stored.currency.type, Currency::CURRENCY_CHAOS_ORB);
    QCOMPARE(stored.value, 2.5);
    QCOMPARE(value->text(), text);
}

// #282: when a refresh removes the selected item, the price editor is reset
// as it is for any lapsed selection, not left enabled on the old price.
void MainWindowTest::removedSelectionResetsPriceEditor()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *nameLabel = fixture.window->findChild<QLabel *>("nameLabel");
    auto *buyoutType = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *buyoutValue = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(tree);
    QVERIFY(nameLabel);
    QVERIFY(buyoutType);
    QVERIFY(buyoutValue);

    const ItemLocation tab = makeTestStashLocation("stash-gone", "Gone", 0);
    const auto item = makeMainWindowItem("gone-item", "Gone", "Sword", tab);
    Items items;
    items.push_back(item);
    fixture.itemsManager->OnItemsRefreshed(items, {tab}, false);
    fixture.buyoutFixture.manager->Set(*item, makeChaosBuyout(5));

    const QModelIndex row = findItemRow(*tree->model(),
                                        findBucket(*tree->model(), tab.GetHeader()),
                                        "Gone Sword");
    QVERIFY(row.isValid());
    tree->selectionModel()->setCurrentIndex(row,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QCOMPARE(buyoutValue->text(), "5");
    QVERIFY(buyoutValue->isEnabled());

    fixture.itemsManager->OnTabRefreshed(tab, {});

    QCOMPARE(nameLabel->text(), "Select an item");
    QCOMPARE(buyoutValue->text(), "");
    QVERIFY(!buyoutValue->isEnabled());
    QVERIFY(!buyoutType->isEnabled());
}

// #282: the single header row of the By-Item view has no location
// (ItemLocation::IsValid() is false; the model labels it "All Items"). It
// cannot be priced, and Check All / Uncheck All skip it, so nothing is
// stored under an empty id.
void MainWindowTest::byItemHeaderIsNotPriceable()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *buyoutType = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    QVERIFY(tree);
    QVERIFY(buyoutType);

    const ItemLocation tab = makeTestStashLocation("stash-flat", "Flat", 0);
    Items items;
    items.push_back(makeMainWindowItem("flat-item", "Flat", "Sword", tab));
    fixture.itemsManager->OnItemsRefreshed(items, {tab}, false);
    switchToByItemView(fixture);

    const QModelIndex header = tree->model()->index(0, 0);
    QCOMPARE(header.data().toString(), "All Items");
    tree->selectionModel()->setCurrentIndex(header,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QVERIFY(!buyoutType->isEnabled());

    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "5");
    QVERIFY(fixture.buyoutFixture.manager->GetTab(ItemLocation()).IsNull());

    fixture.window->OnUncheckAll();
    QVERIFY(fixture.buyoutFixture.manager->GetRefreshChecked(ItemLocation()));
}

// A price edit over a selection leaves every row priced in the game alone:
// an item whose note holds a price, and a tab whose name does, together
// with the items in that tab. The other rows take the price.
void MainWindowTest::priceEditSkipsGamePricedRows()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);

    const ItemLocation dump = makeTestStashLocation("stash-dump", "Dump", 0);
    const ItemLocation priced = makeTestStashLocation("stash-priced", "~price 2 chaos", 1);
    const auto noted = makeMainWindowItemWithNote("noted", "Noted", "Sword", dump, "~b/o 5 chaos");
    const auto plain = makeMainWindowItem("plain", "Plain", "Sword", dump);
    const auto inside = makeMainWindowItem("inside", "Inside", "Sword", priced);
    fixture.itemsManager->OnItemsRefreshed({noted, plain, inside}, {dump, priced}, false);

    auto *model = tree->model();
    const QModelIndex dumpBucket = findBucket(*model, dump.GetHeader());
    const QModelIndex pricedBucket = findBucket(*model, priced.GetHeader());
    QVERIFY(dumpBucket.isValid());
    QVERIFY(pricedBucket.isValid());
    tree->expand(dumpBucket);
    tree->expand(pricedBucket);
    auto *selection = tree->selectionModel();
    selection->setCurrentIndex(findItemRow(*model, dumpBucket, "Plain Sword"),
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    for (const QModelIndex &row : {findItemRow(*model, dumpBucket, "Noted Sword"),
                                   findItemRow(*model, pricedBucket, "Inside Sword"),
                                   pricedBucket}) {
        QVERIFY(row.isValid());
        selection->select(row, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    QCOMPARE(selection->selectedRows().size(), 4);

    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");

    const auto &manager = *fixture.buyoutFixture.manager;
    QCOMPARE(manager.Get(*plain).value, 7.0);
    QVERIFY(!manager.Get(*plain).IsGameSet());
    QCOMPARE(manager.Get(*noted).value, 5.0);
    QVERIFY(manager.Get(*noted).IsGameSet());
    QCOMPARE(manager.GetTab(priced).value, 2.0);
    QVERIFY(manager.GetTab(priced).IsGameSet());
    QCOMPARE(manager.Get(*inside).value, 2.0);
    QVERIFY(manager.Get(*inside).IsInherited());
}

// Selects two unpriced items of one tab and returns them, the value box
// enabled and empty (the type and currency chosen as a user would).
static std::pair<std::shared_ptr<Item>, std::shared_ptr<Item>> selectTwoItemsForPricing(
    MainWindowFixture &fixture)
{
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    const ItemLocation tab = makeTestStashLocation("stash-pair", "Pair", 0);
    auto first = makeMainWindowItem("pair-first", "First", "Sword", tab);
    auto second = makeMainWindowItem("pair-second", "Second", "Sword", tab);
    fixture.itemsManager->OnItemsRefreshed({first, second}, {tab}, false);
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tab.GetHeader());
    tree->expand(bucket);
    tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "First Sword"),
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    tree->selectionModel()->select(findItemRow(*model, bucket, "Second Sword"),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    return {first, second};
}

// #252: typing "150" over a multi-selection wrote 1, 15 and 150 to every
// selected row. Now nothing is written until editing finishes, and then
// every row takes 150 in one command.
void MainWindowTest::typedPriceCommitsOnceToMultiSelection()
{
    MainWindowFixture fixture;
    const auto [first, second] = selectTwoItemsForPricing(fixture);
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(value && value->isEnabled());
    const auto &manager = *fixture.buyoutFixture.manager;

    QSignalSpy changes(fixture.buyoutFixture.manager.get(), &BuyoutManager::BuyoutsChanged);
    QTest::keyClicks(value, "150");
    QCOMPARE(changes.count(), 0);
    QVERIFY(manager.Get(*first).IsNull());
    QVERIFY(manager.Get(*second).IsNull());

    QTest::keyClick(value, Qt::Key_Return);
    QCOMPARE(changes.count(), 1);
    QCOMPARE(manager.Get(*first).value, 150.0);
    QCOMPARE(manager.Get(*second).value, 150.0);
}

// #252: a click on another row while a price is being typed commits the
// price to the rows that were selected while it was typed, not to the row
// clicked: the box loses focus, and finishes editing, before the click
// changes the selection.
void MainWindowTest::clickOnAnotherRowCommitsToTheRowsTypedFor()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(tree && value);
    fixture.window->resize(900, 500);
    fixture.window->show();
    QVERIFY(QTest::qWaitForWindowActive(fixture.window.get()));

    const ItemLocation tab = makeTestStashLocation("stash-click", "Click", 0);
    auto typed = makeMainWindowItem("click-typed", "Typed", "Sword", tab);
    auto clicked = makeMainWindowItem("click-clicked", "Clicked", "Sword", tab);
    fixture.itemsManager->OnItemsRefreshed({typed, clicked}, {tab}, false);
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tab.GetHeader());
    tree->expand(bucket);
    const QModelIndex typedRow = findItemRow(*model, bucket, "Typed Sword");
    const QModelIndex clickedRow = findItemRow(*model, bucket, "Clicked Sword");
    QVERIFY(typedRow.isValid() && clickedRow.isValid());
    tree->selectionModel()->setCurrentIndex(typedRow,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);

    value->setFocus();
    QTRY_VERIFY(value->hasFocus());
    QTest::keyClicks(value, "150");
    const auto &manager = *fixture.buyoutFixture.manager;
    QVERIFY(manager.Get(*typed).IsNull());

    const QRect rect = tree->visualRect(clickedRow);
    QVERIFY(rect.isValid());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());

    QCOMPARE(tree->selectionModel()->selectedRows(), QModelIndexList({clickedRow}));
    QCOMPARE(manager.Get(*typed).value, 150.0);
    QVERIFY(manager.Get(*clicked).IsNull());
}

// A price edit over a selection that holds a row priced in the game says
// in the status line that the row was left alone, instead of failing
// silently on it.
void MainWindowTest::gamePricedRowsLeftAloneAreNamedInStatusLine()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);
    const ItemLocation tab = makeTestStashLocation("stash-mixed", "Mixed", 0);
    const auto noted = makeMainWindowItemWithNote("m-noted", "Noted", "Sword", tab, "~b/o 5 chaos");
    const auto plain = makeMainWindowItem("m-plain", "Plain", "Sword", tab);
    fixture.itemsManager->OnItemsRefreshed({noted, plain}, {tab}, false);
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tab.GetHeader());
    tree->expand(bucket);
    tree->selectionModel()->setCurrentIndex(findItemRow(*model, bucket, "Plain Sword"),
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    tree->selectionModel()->select(findItemRow(*model, bucket, "Noted Sword"),
                                   QItemSelectionModel::Select | QItemSelectionModel::Rows);

    applyBuyoutCommand(fixture, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_CHAOS_ORB, "7");

    QCOMPARE(fixture.buyoutFixture.manager->Get(*plain).value, 7.0);
    QCOMPARE(fixture.buyoutFixture.manager->Get(*noted).value, 5.0);
    const QString status = statusText(fixture);
    QVERIFY2(status.contains("in the game"), qPrintable(status));
    QVERIFY2(!status.contains("Price not saved"), qPrintable(status));
}

// Shows the window and gives the price box focus, as a user typing into it
// would leave things. Nothing the editor does depends on the focus; the
// tests that use this only make sure focus moves change nothing.
static QLineEdit *focusPriceBox(MainWindowFixture &fixture)
{
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    if (!value) {
        qFatal("focusPriceBox: the value box is missing");
    }
    fixture.window->resize(900, 500);
    fixture.window->show();
    fixture.window->activateWindow();
    value->setFocus();
    return value;
}

// A refresh delta for the selected item's tab, landing while a price is
// being typed, leaves the typed text alone, and finishing the edit then
// prices the item.
void MainWindowTest::refreshOfSelectedItemsTabKeepsTypedPrice()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = focusPriceBox(fixture);
    QTest::keyClicks(value, "150");

    const ItemLocation tab = item->location();
    fixture.itemsManager->OnTabRefreshed(tab,
                                         {makeMainWindowItem("price-item", "Priced", "Sword", tab),
                                          makeMainWindowItem("new-item", "Newer", "Sword", tab)});
    QCOMPARE(value->text(), "150");

    QTest::keyClick(value, Qt::Key_Return);
    QCOMPARE(fixture.buyoutFixture.manager->Get(*item).value, 150.0);
}

// A metadata delta that renames the selected tab redraws the editor (the
// header follows its stable key); a price being typed for the tab survives
// it, and finishing the edit prices the tab.
void MainWindowTest::renameOfSelectedTabKeepsTypedPrice()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    QVERIFY(tree);
    const ItemLocation tab = makeTestStashLocation("stash-rename", "Before", 0);
    fixture.itemsManager->OnItemsRefreshed({makeMainWindowItem("r-item", "Kept", "Sword", tab)},
                                           {tab},
                                           false);
    const QModelIndex bucket = findBucket(*tree->model(), tab.GetHeader());
    QVERIFY(bucket.isValid());
    tree->selectionModel()->setCurrentIndex(bucket,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    auto *value = focusPriceBox(fixture);
    QTest::keyClicks(value, "150");

    const ItemLocation renamed = makeTestStashLocation("stash-rename", "After", 0);
    fixture.itemsManager->OnTabRefreshed(renamed,
                                         {makeMainWindowItem("r-item", "Kept", "Sword", renamed)});
    QVERIFY(findBucket(*tree->model(), renamed.GetHeader()).isValid());
    QCOMPARE(value->text(), "150");

    QTest::keyClick(value, Qt::Key_Return);
    QCOMPARE(fixture.buyoutFixture.manager->GetTab(renamed).value, 150.0);
}

// Selects one row of the current search's tree.
static void selectRow(MainWindowFixture &fixture, const QString &tab_header, const QString &row)
{
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, tab_header);
    QVERIFY(bucket.isValid());
    tree->expand(bucket);
    const QModelIndex index = findItemRow(*model, bucket, row);
    QVERIFY(index.isValid());
    tree->selectionModel()->setCurrentIndex(index,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
}

// Two items in one tab, each selected in its own search: Alpha in the first
// search, Bravo in the second, with the first search current.
static std::pair<std::shared_ptr<Item>, std::shared_ptr<Item>> twoSearchesTwoSelections(
    MainWindowFixture &fixture, QTabBar *&tabs)
{
    tabs = findSearchTabs(*fixture.window);
    const ItemLocation tab = makeTestStashLocation("stash-two", "Two", 0);
    auto alpha = makeMainWindowItem("two-alpha", "Alpha", "Sword", tab);
    auto bravo = makeMainWindowItem("two-bravo", "Bravo", "Sword", tab);
    fixture.itemsManager->OnItemsRefreshed({alpha, bravo}, {tab}, false);
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(1).center()); // "+"
    selectRow(fixture, tab.GetHeader(), "Bravo Sword");
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(0).center());
    selectRow(fixture, tab.GetHeader(), "Alpha Sword");
    return {alpha, bravo};
}

// Types a chaos price into the focused value box without finishing it.
static void typePrice(MainWindowFixture &fixture, const QString &text)
{
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    QTest::keyClicks(focusPriceBox(fixture), text);
}

// A click on another search's tab does not take focus from the value box,
// yet changes what is selected: the price typed for the first search's
// row is committed to it, not to the row selected in the other search.
void MainWindowTest::searchTabClickCommitsToTheRowsTypedFor()
{
    MainWindowFixture fixture;
    QTabBar *tabs = nullptr;
    const auto [alpha, bravo] = twoSearchesTwoSelections(fixture, tabs);
    typePrice(fixture, "150");

    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(1).center());
    QCOMPARE(tabs->currentIndex(), 1);

    const auto &manager = *fixture.buyoutFixture.manager;
    QCOMPARE(manager.Get(*alpha).value, 150.0);
    QVERIFY(manager.Get(*bravo).IsNull());
    // The editor now shows the second search's row, not the typed value.
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QCOMPARE(value->text(), QString());
}

// The same for "+", which opens a search with nothing selected.
void MainWindowTest::newSearchClickCommitsToTheRowsTypedFor()
{
    MainWindowFixture fixture;
    auto *tabs = findSearchTabs(*fixture.window);
    const auto item = selectItemForPricing(fixture);
    QTest::keyClicks(focusPriceBox(fixture), "150");

    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(1).center());
    QCOMPARE(tabs->count(), 3);

    QCOMPARE(fixture.buyoutFixture.manager->Get(*item).value, 150.0);
    QCOMPARE(fixture.buyoutFixture.manager->Get(*item).type, Buyout::BUYOUT_TYPE_BUYOUT);
}

// Switching the view mode shows the same selected row again: the typed
// price stays pending, and Enter then prices the row it was typed for.
void MainWindowTest::viewModeSwitchCommitsToTheRowsTypedFor()
{
    MainWindowFixture fixture;
    auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(viewCombo);
    const auto item = selectItemForPricing(fixture);
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QTest::keyClicks(value, "150");

    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);
    QCOMPARE(value->text(), "150");
    QTest::keyClick(value, Qt::Key_Return);

    QCOMPARE(fixture.buyoutFixture.manager->Get(*item).value, 150.0);
}

// Closing the search whose row the price was typed for (a middle click on
// its tab) commits the price to that row before the search goes.
void MainWindowTest::closingTheSearchCommitsToTheRowsTypedFor()
{
    MainWindowFixture fixture;
    QTabBar *tabs = nullptr;
    const auto [alpha, bravo] = twoSearchesTwoSelections(fixture, tabs);
    typePrice(fixture, "150");

    QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier, tabs->tabRect(0).center());
    QCOMPARE(tabs->count(), 2);

    QCOMPARE(fixture.buyoutFixture.manager->Get(*alpha).value, 150.0);
    QVERIFY(fixture.buyoutFixture.manager->Get(*bravo).IsNull());
}

// A refresh delta landing while a price is typed commits nothing: no
// buyout batch, no commit from the editor, and the focus stays in the box.
void MainWindowTest::refreshRunsNoCommitWhileTyping()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = focusPriceBox(fixture);
    QTest::keyClicks(value, "150");
    auto *panel = fixture.window->findChild<QObject *>("buyoutPanel");
    QVERIFY(panel);
    QSignalSpy committed(panel, SIGNAL(committed(PriceTargets, Buyout)));
    QVERIFY(committed.isValid());
    QSignalSpy changes(fixture.buyoutFixture.manager.get(), &BuyoutManager::BuyoutsChanged);

    const ItemLocation tab = item->location();
    fixture.itemsManager->OnTabRefreshed(tab,
                                         {makeMainWindowItem("price-item", "Priced", "Sword", tab)});

    QCOMPARE(committed.count(), 0);
    QCOMPARE(changes.count(), 0);
    QCOMPARE(value->text(), "150");
}

// A price typed for a row that a refresh then removes is committed when the
// editor stops showing that row: nothing takes it, and the status line
// says the row is no longer there.
void MainWindowTest::typedPriceForGoneRowSaysSo()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QTest::keyClicks(value, "150");

    fixture.itemsManager->OnTabRefreshed(item->location(), {});

    const QString status = statusText(fixture);
    QVERIFY2(status.contains("no longer there"), qPrintable(status));
}

// Three unpriced items of one tab, the tab expanded; nothing selected yet.
struct ThreeRows
{
    std::shared_ptr<Item> a, b, c;
    QModelIndex rowA, rowB, rowC;
    ItemLocation tab;
};
static ThreeRows threeRows(MainWindowFixture &fixture)
{
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    ThreeRows r;
    r.tab = makeTestStashLocation("stash-three", "Three", 0);
    r.a = makeMainWindowItem("three-a", "Alpha", "Sword", r.tab);
    r.b = makeMainWindowItem("three-b", "Bravo", "Sword", r.tab);
    r.c = makeMainWindowItem("three-c", "Charlie", "Sword", r.tab);
    fixture.itemsManager->OnItemsRefreshed({r.a, r.b, r.c}, {r.tab}, false);
    auto *model = tree->model();
    const QModelIndex bucket = findBucket(*model, r.tab.GetHeader());
    tree->expand(bucket);
    r.rowA = findItemRow(*model, bucket, "Alpha Sword");
    r.rowB = findItemRow(*model, bucket, "Bravo Sword");
    r.rowC = findItemRow(*model, bucket, "Charlie Sword");
    return r;
}

// The same three items again, as a refresh of their tab delivers them.
static Items threeRowsAgain(const ThreeRows &r)
{
    return {makeMainWindowItem("three-a", "Alpha", "Sword", r.tab),
            makeMainWindowItem("three-b", "Bravo", "Sword", r.tab),
            makeMainWindowItem("three-c", "Charlie", "Sword", r.tab)};
}

// Picks a chaos buyout and types a value into the price box, unfinished.
// No focus is needed: the keys go to the box itself.
static QLineEdit *typeUnfinished(MainWindowFixture &fixture, const QString &text)
{
    auto *type = fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    auto *value = fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    emit type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    QTest::keyClicks(value, text);
    return value;
}

// A refresh delta for the selected rows' tab collapses a multi-selection to
// the current row; the price typed for both rows goes to both.
void MainWindowTest::refreshCollapsingMultiSelectionPricesTheTypedRows()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    const ThreeRows r = threeRows(fixture);
    tree->selectionModel()->setCurrentIndex(r.rowA,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    tree->selectionModel()->select(r.rowB, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    auto *value = typeUnfinished(fixture, "150");

    fixture.itemsManager->OnTabRefreshed(r.tab, threeRowsAgain(r));
    QTest::keyClick(value, Qt::Key_Return);

    const auto &manager = *fixture.buyoutFixture.manager;
    QCOMPARE(manager.Get(*r.a).value, 150.0);
    QCOMPARE(manager.Get(*r.b).value, 150.0);
    QVERIFY(manager.Get(*r.c).IsNull());
}

// Click A, ctrl-click B and C, ctrl-click C again: C stays current but is
// not selected. A refresh reselects C; the price typed for A and B goes to
// A and B, never to C.
void MainWindowTest::refreshReselectingDeselectedRowPricesTheTypedRows()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    const ThreeRows r = threeRows(fixture);
    auto *selection = tree->selectionModel();
    selection->setCurrentIndex(r.rowA,
                               QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->setCurrentIndex(r.rowB, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->setCurrentIndex(r.rowC, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selection->select(r.rowC, QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
    QCOMPARE(selection->currentIndex(), r.rowC);
    QCOMPARE(selection->selectedRows().size(), 2);
    auto *value = typeUnfinished(fixture, "150");

    fixture.itemsManager->OnTabRefreshed(r.tab, threeRowsAgain(r));
    QTest::keyClick(value, Qt::Key_Return);

    const auto &manager = *fixture.buyoutFixture.manager;
    QCOMPARE(manager.Get(*r.a).value, 150.0);
    QCOMPARE(manager.Get(*r.b).value, 150.0);
    QVERIFY(manager.Get(*r.c).IsNull());
}

// A search-form change that filters the typed-for row out changes what is
// shown; the price goes to the row it was typed for at that moment.
void MainWindowTest::refilterWhileTypingPricesTheTypedRow()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *name = findNameFilter(*fixture.window);
    QVERIFY(name);
    const ThreeRows r = threeRows(fixture);
    tree->selectionModel()->setCurrentIndex(r.rowA,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    typeUnfinished(fixture, "150");

    name->setText("Bravo");
    fixture.window->OnSearchFormChange();

    const auto &manager = *fixture.buyoutFixture.manager;
    QCOMPARE(manager.Get(*r.a).value, 150.0);
    QVERIFY(manager.Get(*r.b).IsNull());
}

// A message box the program opens by itself while a price is typed (it
// takes focus) commits nothing: "1" of "15" is not saved, and "15" is.
void MainWindowTest::messageBoxWhileTypingCommitsNothing()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = focusPriceBox(fixture);
    QTest::keyClicks(value, "1");
    QSignalSpy changes(fixture.buyoutFixture.manager.get(), &BuyoutManager::BuyoutsChanged);

    QTimer::singleShot(0, fixture.window.get(), [&fixture] {
        QTimer::singleShot(50, [] {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                box->accept();
            }
        });
        fixture.window->OnNotifyUser("Refreshing");
    });
    QTest::qWait(300);
    QCOMPARE(changes.count(), 0);

    QTest::keyClicks(value, "5");
    QTest::keyClick(value, Qt::Key_Return);
    QCOMPARE(fixture.buyoutFixture.manager->Get(*item).value, 15.0);
}

// A value typed, the event loop turning, then a currency chosen from its
// list: one commit, with the typed value and the chosen currency.
void MainWindowTest::typedValueThenCurrencyCommitsOnceAcrossEventLoop()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = focusPriceBox(fixture);
    auto *currency = fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    QTest::keyClicks(value, "150");
    QSignalSpy changes(fixture.buyoutFixture.manager.get(), &BuyoutManager::BuyoutsChanged);

    QTest::mouseClick(currency, Qt::LeftButton);
    QTest::qWait(50);
    currency->hidePopup();
    QTest::qWait(50);
    currency->setCurrentIndex(Currency::CURRENCY_DIVINE_ORB);
    emit currency->activated(Currency::CURRENCY_DIVINE_ORB);
    QTest::qWait(50);

    QCOMPARE(changes.count(), 1);
    const Buyout stored = fixture.buyoutFixture.manager->Get(*item);
    QCOMPARE(stored.value, 150.0);
    QCOMPARE(stored.currency.type, Currency::CURRENCY_DIVINE_ORB);
}

// Enter on text that is not a price saves nothing, keeps the text, and the
// status line says why.
void MainWindowTest::invalidTextOnEnterSaysWhy()
{
    MainWindowFixture fixture;
    const auto item = selectItemForPricing(fixture);
    auto *value = typeUnfinished(fixture, "abc");
    QTest::keyClick(value, Qt::Key_Return);

    QVERIFY(fixture.buyoutFixture.manager->Get(*item).IsNull());
    QCOMPARE(value->text(), "abc");
    const QString status = statusText(fixture);
    QVERIFY2(status.contains("not a price"), qPrintable(status));
}

// Text that is not a price, pending when another row is selected, is not
// applied, and the status line says so.
void MainWindowTest::invalidTextOnTargetChangeSaysWhy()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    const ThreeRows r = threeRows(fixture);
    tree->selectionModel()->setCurrentIndex(r.rowA,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    typeUnfinished(fixture, "abc");

    tree->selectionModel()->setCurrentIndex(r.rowB,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);

    QVERIFY(fixture.buyoutFixture.manager->Get(*r.a).IsNull());
    const QString status = statusText(fixture);
    QVERIFY2(status.contains("not a price"), qPrintable(status));
}

// #282: selecting a tab header after an item stops the item's pending
// details update, which would otherwise replace the header's details.
void MainWindowTest::headerSelectionSurvivesItemDetailsTimer()
{
    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *nameLabel = fixture.window->findChild<QLabel *>("nameLabel");
    QVERIFY(tree);
    QVERIFY(nameLabel);

    const ItemLocation tab = makeTestStashLocation("stash-head", "Head", 0);
    Items items;
    items.push_back(makeMainWindowItem("head-item", "Head", "Sword", tab));
    fixture.itemsManager->OnItemsRefreshed(items, {tab}, false);

    const QModelIndex bucket = findBucket(*tree->model(), tab.GetHeader());
    const QModelIndex row = findItemRow(*tree->model(), bucket, "Head Sword");
    QVERIFY(row.isValid());
    tree->selectionModel()->setCurrentIndex(row,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    tree->selectionModel()->setCurrentIndex(bucket,
                                            QItemSelectionModel::ClearAndSelect
                                                | QItemSelectionModel::Rows);
    QCOMPARE(nameLabel->text(), tab.GetHeader());

    QTest::qWait(300); // past the 100 ms details timer
    QCOMPARE(nameLabel->text(), tab.GetHeader());
}

QTEST_MAIN(MainWindowTest)

#include "tst_mainwindow.moc"
