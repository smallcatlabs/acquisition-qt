// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Folder level in the By-Tab items tree (docs/design/folder-tree.md),
// driven through the real window the way the refresh pipeline drives it:
// snapshots through ItemsManager::OnItemsRefreshed, deltas through
// ItemsManager::OnTabRefreshed. Every scenario runs in an unfiltered and a
// filtered search, with QAbstractItemModelTester attached in fatal mode,
// and checks after each step that the maintained tree equals the tree a
// fresh refilter of the same published state builds (the convergence
// criterion), with no model reset on the refresh path.

#include <QtTest/QtTest>

#include <QAbstractItemModelTester>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QTabBar>
#include <QTreeView>

#include <map>
#include <memory>
#include <random>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "buyout.h"
#include "currency.h"
#include "items_model.h"
#include "mainwindowfixture.h"
#include "modelprobes.h"

class FolderTreeTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // S3: the tree (D2, D3, D5, D7, D8) and the convergence scenarios.
    void baseTreeNestsMembersUnderFolders_data();
    void baseTreeNestsMembersUnderFolders();
    void memberArrivesWhileFolderAbsent_data(); // scenario 1
    void memberArrivesWhileFolderAbsent();
    void lastVisibleMemberLeaves_data(); // scenario 2
    void lastVisibleMemberLeaves();
    void emptyFolderDeltaKeepsMembers_data(); // scenario 3
    void emptyFolderDeltaKeepsMembers();
    void tabMovesBetweenFolders_data(); // scenario 4
    void tabMovesBetweenFolders();
    void folderIndexChangeMovesItsRun_data(); // scenario 5
    void folderIndexChangeMovesItsRun();
    void folderDeletedAtSnapshotKeepsMembers_data(); // scenario 6
    void folderDeletedAtSnapshotKeepsMembers();
    void memberDiscoveredBeforeFolder_data(); // scenario 7
    void memberDiscoveredBeforeFolder();
    void unknownFolderIdStaysTopLevel_data(); // scenario 8
    void unknownFolderIdStaysTopLevel();
    void viewModeRoundTripAfterDeltas_data(); // scenario 9
    void viewModeRoundTripAfterDeltas();
    void nestedBucketSortsAndResorts_data(); // scenario 10
    void nestedBucketSortsAndResorts();
    void moveIntoAbsentFolder_data(); // scenario 4, the folder entered absent
    void moveIntoAbsentFolder();
    void snapshotOnlyMetadataSettlesTree_data(); // scenarios 4 and 5 via snapshot
    void snapshotOnlyMetadataSettlesTree();
    void nestedSelectionSurvivesMoves();
    void foldersUnaffectedAccountKeepsFlatTree();
    void folderStormConverges_data();
    void folderStormConverges();

    // S4: the view (D9).
    void foldersExpandByDefault_data();
    void foldersExpandByDefault();
    void collapsedFolderStaysCollapsed_data();
    void collapsedFolderStaysCollapsed();
    void insertedFolderExpands_data();
    void insertedFolderExpands();
    void expandAllAndCollapseAllCoverNestedHeaders();
    void folderExpansionMaterializesNothing();
    void nestedTabExpansionSurvivesRefilter();
    void folderHeaderSelectionShowsHeaderText();
    void nestedScrollAnchorSurvivesRefilter();
    void collapsedRowsSurviveReselectionOnRefresh_data();
    void collapsedRowsSurviveReselectionOnRefresh();

    // S5: checkboxes, Refresh Selected, buyouts (D10, D11, R5-R7, A1).
    void folderCheckStateDerivesFromVisibleMembers_data();
    void folderCheckStateDerivesFromVisibleMembers();
    void folderClickCascadesToVisibleMembers();
    void lockedMemberKeepsFolderPartial();
    void nestedTabCheckRepaintsFolder();
    void checkSelectedAndCheckAllOnFolders();
    void refreshCheckStatesCoversNestedHeaders();
    void refreshSelectedOnFolderSendsCheckedMembers_data();
    void refreshSelectedOnFolderSendsCheckedMembers();
    void folderHeaderDisablesBuyoutWidgets();
    void buyoutEditSkipsFolderRows();

private:
    std::shared_ptr<spdlog::logger> m_main_logger;
    std::shared_ptr<spdlog::sinks::dist_sink_mt> m_sink_hub;
};

namespace {

    ItemLocation makeFolder(const QString &id, const QString &name, unsigned index)
    {
        poe::StashTab stash;
        stash.id = id;
        stash.name = name;
        stash.type = "Folder";
        stash.index = index;
        return ItemLocation(stash);
    }

    ItemLocation makeTab(const QString &id,
                         const QString &name,
                         unsigned index,
                         const QString &folder = {})
    {
        poe::StashTab stash;
        stash.id = id;
        stash.name = name;
        stash.type = "PremiumStash";
        stash.index = index;
        if (!folder.isEmpty()) {
            stash.folder = folder;
        }
        return ItemLocation(stash);
    }

    std::shared_ptr<Item> makeItem(const QString &id,
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

    // The published world at the fetch-source grain, the way ItemsManager
    // holds it: the tab list in listing order and each tab's items.
    struct World
    {
        std::vector<ItemLocation> tabs;
        std::map<QString, Items> items;

        void setTab(const ItemLocation &location)
        {
            for (auto &tab : tabs) {
                if (tab.id() == location.id()) {
                    tab = location;
                    return;
                }
            }
            tabs.push_back(location);
        }
        void removeTab(const QString &id)
        {
            std::erase_if(tabs, [&id](const ItemLocation &tab) { return tab.id() == id; });
            items.erase(id);
        }
        const ItemLocation &tab(const QString &id) const
        {
            for (const auto &tab : tabs) {
                if (tab.id() == id) {
                    return tab;
                }
            }
            qFatal("World::tab: unknown tab");
        }
        Items flatten() const
        {
            Items all;
            for (const auto &tab : tabs) {
                const auto it = items.find(tab.id());
                if (it != items.end()) {
                    all.insert(all.end(), it->second.begin(), it->second.end());
                }
            }
            return all;
        }
    };

    // The base world: two folders with members, a loose tab with a match
    // and a loose tab without one.
    //   F "Folder F" #1 : A "Alpha" #2 (sword), B "Beta" #3 (sword)
    //   G "Folder G" #4 : C "Gamma" #5 (sword)
    //   D "Delta" #6 (sword), E "Echo" #7 (shield only)
    World baseWorld()
    {
        World world;
        world.setTab(makeFolder("folder-f", "Folder F", 0));
        world.setTab(makeTab("tab-a", "Alpha", 1, "folder-f"));
        world.setTab(makeTab("tab-b", "Beta", 2, "folder-f"));
        world.setTab(makeFolder("folder-g", "Folder G", 3));
        world.setTab(makeTab("tab-c", "Gamma", 4, "folder-g"));
        world.setTab(makeTab("tab-d", "Delta", 5));
        world.setTab(makeTab("tab-e", "Echo", 6));
        world.items["tab-a"] = {makeItem("a1", "A1", "Sword", world.tab("tab-a"))};
        world.items["tab-b"] = {makeItem("b1", "B1", "Sword", world.tab("tab-b"))};
        world.items["tab-c"] = {makeItem("c1", "C1", "Sword", world.tab("tab-c"))};
        world.items["tab-d"] = {makeItem("d1", "D1", "Sword", world.tab("tab-d"))};
        world.items["tab-e"] = {makeItem("e1", "E1", "Shield", world.tab("tab-e"))};
        return world;
    }

    QLineEdit *findNameFilter(MainWindow &window)
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

    // The tree as text: a header line per bucket, nested headers indented,
    // a tab's items (sorted — collapsed buckets keep arrival order, D2)
    // after a colon. A folder line has no colon.
    QStringList treeSignature(const QTreeView &view)
    {
        const auto *model = qobject_cast<const ItemsModel *>(view.model());
        if (!model) {
            qFatal("treeSignature: the view has no ItemsModel");
        }
        QStringList lines;
        const auto bucketLine = [model](const QModelIndex &header, const QString &indent) {
            QStringList names;
            for (int row = 0; row < model->rowCount(header); ++row) {
                names.append(model->index(row, 0, header).data().toString());
            }
            names.sort();
            return indent + header.data().toString() + ": " + names.join(", ");
        };
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex top = model->index(row, 0);
            const QModelIndex first = model->index(0, 0, top);
            const bool folder = first.isValid() && model->Locate(first).isBucket();
            const bool empty_folder = !first.isValid() && top.data().toString().contains("Folder");
            if (folder || empty_folder) {
                lines.append(top.data().toString());
                for (int child = 0; child < model->rowCount(top); ++child) {
                    lines.append(bucketLine(model->index(child, 0, top), "  "));
                }
            } else {
                lines.append(bucketLine(top, ""));
            }
        }
        return lines;
    }

    // The scenario harness: a window seeded with a world, optionally
    // filtered to sword matches, with the model tester and a reset spy
    // attached after seeding.
    struct Harness
    {
        explicit Harness(const World &seed, bool filtered)
            : world(seed)
        {
            tree = fixture.window->findChild<QTreeView *>("treeView");
            if (!tree) {
                qFatal("Harness: no treeView");
            }
            fixture.itemsManager->OnItemsRefreshed(world.flatten(), world.tabs, true);
            if (filtered) {
                auto *name = findNameFilter(*fixture.window);
                if (!name) {
                    qFatal("Harness: no name filter");
                }
                name->setFocus();
                QTest::keyClicks(name, "sword");
                fixture.window->OnSearchFormChange();
            }
            model = qobject_cast<ItemsModel *>(tree->model());
            tester = std::make_unique<QAbstractItemModelTester>(
                model, QAbstractItemModelTester::FailureReportingMode::Fatal);
            resets = std::make_unique<QSignalSpy>(model, &QAbstractItemModel::modelReset);
        }

        void delta(const ItemLocation &location, const Items &items)
        {
            world.setTab(location);
            world.items[location.id()] = items;
            fixture.itemsManager->OnTabRefreshed(location, items);
        }
        void snapshot()
        {
            fixture.itemsManager->OnItemsRefreshed(world.flatten(), world.tabs, false);
            fixture.window->OnRefreshFinished(RefreshOutcome{CompletedRefresh{}});
        }
        QStringList refiltered()
        {
            fixture.window->OnSearchFormChange();
            const QStringList signature = treeSignature(*tree);
            resets->clear(); // the refilter's own reset is the D6 boundary
            return signature;
        }

        MainWindowFixture fixture;
        World world;
        QTreeView *tree{nullptr};
        ItemsModel *model{nullptr};
        std::unique_ptr<QAbstractItemModelTester> tester;
        std::unique_ptr<QSignalSpy> resets;
    };

    QStringList withoutEcho(QStringList lines, bool filtered)
    {
        if (filtered) {
            lines.removeAll(QStringLiteral("#7, \"Echo\": E1 Shield"));
        }
        return lines;
    }

} // namespace

// Applied tree equals the expected shape, no reset ran on the refresh
// path, and a fresh refilter of the same published state builds the same
// tree. A macro so a failing comparison returns from the test itself.
#define CHECK_TREE(harness, expected) \
    do { \
        const QStringList applied_ = treeSignature(*(harness).tree); \
        QCOMPARE(applied_, (expected)); \
        QCOMPARE((harness).resets->count(), 0); \
        QCOMPARE((harness).refiltered(), applied_); \
    } while (0)

static void addFilteredColumn()
{
    QTest::addColumn<bool>("filtered");
    QTest::newRow("unfiltered") << false;
    QTest::newRow("filtered") << true;
}

void FolderTreeTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    // LogPanel attaches its sinks through the dist-sink hub that
    // logging::init installs on the main logger (F42).
    m_main_logger = std::make_shared<spdlog::logger>("main");
    m_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    m_main_logger->sinks().push_back(m_sink_hub);
    spdlog::register_logger(m_main_logger);
}

void FolderTreeTest::cleanupTestCase()
{
    spdlog::drop("main");
    m_sink_hub.reset();
    m_main_logger.reset();
}

void FolderTreeTest::baseTreeNestsMembersUnderFolders_data()
{
    addFilteredColumn();
}

void FolderTreeTest::baseTreeNestsMembersUnderFolders()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    const QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "  #3, \"Beta\": B1 Sword",
                                              "#4, \"Folder G\"",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "#6, \"Delta\": D1 Sword",
                                              "#7, \"Echo\": E1 Shield"},
                                             filtered);
    CHECK_TREE(h, expected);
    // D5: a nested header's parent is its folder; an item's parent is its
    // tab; a folder is top-level.
    const QModelIndex folder = h.model->index(0, 0);
    const QModelIndex alpha = h.model->index(0, 0, folder);
    const QModelIndex item = h.model->index(0, 0, alpha);
    QCOMPARE(alpha.parent(), folder);
    QCOMPARE(item.parent(), alpha);
    QVERIFY(!folder.parent().isValid());
    QVERIFY(h.model->Locate(alpha).isBucket());
    QVERIFY(h.model->Locate(item).isItem());
    QCOMPARE(h.model->Locate(item).bucket_row, h.model->Locate(alpha).bucket_row);
}

void FolderTreeTest::memberArrivesWhileFolderAbsent_data()
{
    addFilteredColumn();
}

// Scenario 1: in a filtered search folder G is absent (its only member has
// no match); a delta gives the member a match — G is inserted first and the
// member lands under it.
void FolderTreeTest::memberArrivesWhileFolderAbsent()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    seed.items["tab-c"] = {makeItem("c1", "C1", "Shield", seed.tab("tab-c"))};
    Harness h(seed, filtered);
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "#4, \"Folder G\"",
                                        "  #5, \"Gamma\": C1 Shield",
                                        "#6, \"Delta\": D1 Sword",
                                        "#7, \"Echo\": E1 Shield"},
                                       filtered);
    if (filtered) {
        expected.removeAll("#4, \"Folder G\"");
        expected.removeAll("  #5, \"Gamma\": C1 Shield");
    }
    CHECK_TREE(h, expected);

    h.delta(h.world.tab("tab-c"), {makeItem("c2", "C2", "Sword", h.world.tab("tab-c"))});
    expected = withoutEcho({"#1, \"Folder F\"",
                            "  #2, \"Alpha\": A1 Sword",
                            "  #3, \"Beta\": B1 Sword",
                            "#4, \"Folder G\"",
                            "  #5, \"Gamma\": C2 Sword",
                            "#6, \"Delta\": D1 Sword",
                            "#7, \"Echo\": E1 Shield"},
                           filtered);
    CHECK_TREE(h, expected);
}

void FolderTreeTest::lastVisibleMemberLeaves_data()
{
    addFilteredColumn();
}

// Scenario 2: folder G's last visible member loses its match — in a
// filtered search the member and then the folder leave.
void FolderTreeTest::lastVisibleMemberLeaves()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.delta(h.world.tab("tab-c"), {makeItem("c2", "C2", "Shield", h.world.tab("tab-c"))});
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "#4, \"Folder G\"",
                                        "  #5, \"Gamma\": C2 Shield",
                                        "#6, \"Delta\": D1 Sword",
                                        "#7, \"Echo\": E1 Shield"},
                                       filtered);
    if (filtered) {
        expected.removeAll("#4, \"Folder G\"");
        expected.removeAll("  #5, \"Gamma\": C2 Shield");
    }
    CHECK_TREE(h, expected);

    // One member of two leaving keeps the folder.
    h.delta(h.world.tab("tab-a"), {});
    expected.replaceInStrings("  #2, \"Alpha\": A1 Sword", "  #2, \"Alpha\": ");
    if (filtered) {
        expected.removeAll("  #2, \"Alpha\": ");
    }
    CHECK_TREE(h, expected);
}

void FolderTreeTest::emptyFolderDeltaKeepsMembers_data()
{
    addFilteredColumn();
}

// Scenario 3: an empty delta for a folder with members (every refresh
// sends one, R9) never removes the folder.
void FolderTreeTest::emptyFolderDeltaKeepsMembers()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.delta(h.world.tab("folder-f"), {});
    h.delta(h.world.tab("folder-g"), {});
    const QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "  #3, \"Beta\": B1 Sword",
                                              "#4, \"Folder G\"",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "#6, \"Delta\": D1 Sword",
                                              "#7, \"Echo\": E1 Shield"},
                                             filtered);
    CHECK_TREE(h, expected);
    h.snapshot();
    CHECK_TREE(h, expected);
}

void FolderTreeTest::tabMovesBetweenFolders_data()
{
    addFilteredColumn();
}

// Scenario 4: a tab moves into a folder, between folders, and out of one —
// each a metadata delta carrying the tab's contents, then a snapshot.
void FolderTreeTest::tabMovesBetweenFolders()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);

    // Into: Delta joins folder F.
    const ItemLocation deltaInF = makeTab("tab-d", "Delta", 5, "folder-f");
    h.delta(deltaInF, {makeItem("d1", "D1", "Sword", deltaInF)});
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "  #6, \"Delta\": D1 Sword",
                                        "#4, \"Folder G\"",
                                        "  #5, \"Gamma\": C1 Sword",
                                        "#7, \"Echo\": E1 Shield"},
                                       filtered);
    CHECK_TREE(h, expected);

    // Between: Alpha moves from F to G.
    const ItemLocation alphaInG = makeTab("tab-a", "Alpha", 1, "folder-g");
    h.delta(alphaInG, {makeItem("a1", "A1", "Sword", alphaInG)});
    expected = withoutEcho({"#1, \"Folder F\"",
                            "  #3, \"Beta\": B1 Sword",
                            "  #6, \"Delta\": D1 Sword",
                            "#4, \"Folder G\"",
                            "  #2, \"Alpha\": A1 Sword",
                            "  #5, \"Gamma\": C1 Sword",
                            "#7, \"Echo\": E1 Shield"},
                           filtered);
    CHECK_TREE(h, expected);

    // Out: Gamma leaves G for the top level.
    const ItemLocation gammaLoose = makeTab("tab-c", "Gamma", 4);
    h.delta(gammaLoose, {makeItem("c1", "C1", "Sword", gammaLoose)});
    expected = withoutEcho({"#1, \"Folder F\"",
                            "  #3, \"Beta\": B1 Sword",
                            "  #6, \"Delta\": D1 Sword",
                            "#4, \"Folder G\"",
                            "  #2, \"Alpha\": A1 Sword",
                            "#5, \"Gamma\": C1 Sword",
                            "#7, \"Echo\": E1 Shield"},
                           filtered);
    CHECK_TREE(h, expected);

    // The snapshot publishes the same state: nothing moves.
    QSignalSpy moves(h.model, &QAbstractItemModel::rowsMoved);
    h.snapshot();
    QCOMPARE(moves.count(), 0);
    CHECK_TREE(h, expected);

    // A filtered folder that loses its last member to a move leaves too.
    const ItemLocation alphaLoose = makeTab("tab-a", "Alpha", 1);
    h.delta(alphaLoose, {makeItem("a1", "A1", "Sword", alphaLoose)});
    expected = withoutEcho({"#1, \"Folder F\"",
                            "  #3, \"Beta\": B1 Sword",
                            "  #6, \"Delta\": D1 Sword",
                            "#2, \"Alpha\": A1 Sword",
                            "#4, \"Folder G\"",
                            "#5, \"Gamma\": C1 Sword",
                            "#7, \"Echo\": E1 Shield"},
                           filtered);
    if (filtered) {
        expected.removeAll("#4, \"Folder G\"");
    }
    CHECK_TREE(h, expected);
}

void FolderTreeTest::folderIndexChangeMovesItsRun_data()
{
    addFilteredColumn();
}

// Scenario 5: a folder's index changes — it moves with its members.
void FolderTreeTest::folderIndexChangeMovesItsRun()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.delta(makeFolder("folder-f", "Folder F", 8), {});
    const QStringList expected = withoutEcho({"#4, \"Folder G\"",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "#6, \"Delta\": D1 Sword",
                                              "#7, \"Echo\": E1 Shield",
                                              "#9, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "  #3, \"Beta\": B1 Sword"},
                                             filtered);
    CHECK_TREE(h, expected);
    h.snapshot();
    CHECK_TREE(h, expected);

    // A member's index change reorders within its folder only.
    const ItemLocation alphaLate = makeTab("tab-a", "Alpha", 9, "folder-f");
    h.delta(alphaLate, {makeItem("a1", "A1", "Sword", alphaLate)});
    const QStringList reordered = withoutEcho({"#4, \"Folder G\"",
                                               "  #5, \"Gamma\": C1 Sword",
                                               "#6, \"Delta\": D1 Sword",
                                               "#7, \"Echo\": E1 Shield",
                                               "#9, \"Folder F\"",
                                               "  #3, \"Beta\": B1 Sword",
                                               "  #10, \"Alpha\": A1 Sword"},
                                              filtered);
    CHECK_TREE(h, reordered);
}

void FolderTreeTest::folderDeletedAtSnapshotKeepsMembers_data()
{
    addFilteredColumn();
}

// Scenario 6: folder G is deleted at the snapshot while its member
// survives (still naming G, or rebased out of it) — the member moves to the
// top level before the folder leaves.
void FolderTreeTest::folderDeletedAtSnapshotKeepsMembers()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.world.removeTab("folder-g");
    h.snapshot();
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "#5, \"Gamma\": C1 Sword",
                                        "#6, \"Delta\": D1 Sword",
                                        "#7, \"Echo\": E1 Shield"},
                                       filtered);
    CHECK_TREE(h, expected);

    // Folder F deleted while its members are rebased out of it.
    h.world.removeTab("folder-f");
    h.world.setTab(makeTab("tab-a", "Alpha", 1));
    h.world.setTab(makeTab("tab-b", "Beta", 2));
    h.snapshot();
    expected = withoutEcho({"#2, \"Alpha\": A1 Sword",
                            "#3, \"Beta\": B1 Sword",
                            "#5, \"Gamma\": C1 Sword",
                            "#6, \"Delta\": D1 Sword",
                            "#7, \"Echo\": E1 Shield"},
                           filtered);
    CHECK_TREE(h, expected);
}

void FolderTreeTest::memberDiscoveredBeforeFolder_data()
{
    addFilteredColumn();
}

// Scenario 7: a new member is discovered before its folder (an unknown
// folder id) — top-level until the folder's delta arrives, then adopted.
void FolderTreeTest::memberDiscoveredBeforeFolder()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    const ItemLocation hotel = makeTab("tab-h", "Hotel", 10, "folder-k");
    h.delta(hotel, {makeItem("h1", "H1", "Sword", hotel)});
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "#4, \"Folder G\"",
                                        "  #5, \"Gamma\": C1 Sword",
                                        "#6, \"Delta\": D1 Sword",
                                        "#7, \"Echo\": E1 Shield",
                                        "#11, \"Hotel\": H1 Sword"},
                                       filtered);
    CHECK_TREE(h, expected);

    h.delta(makeFolder("folder-k", "Folder K", 9), {});
    expected = withoutEcho({"#1, \"Folder F\"",
                            "  #2, \"Alpha\": A1 Sword",
                            "  #3, \"Beta\": B1 Sword",
                            "#4, \"Folder G\"",
                            "  #5, \"Gamma\": C1 Sword",
                            "#6, \"Delta\": D1 Sword",
                            "#7, \"Echo\": E1 Shield",
                            "#10, \"Folder K\"",
                            "  #11, \"Hotel\": H1 Sword"},
                           filtered);
    CHECK_TREE(h, expected);
    h.snapshot();
    CHECK_TREE(h, expected);
}

void FolderTreeTest::unknownFolderIdStaysTopLevel_data()
{
    addFilteredColumn();
}

// Scenario 8: a tab's folder id names no known folder — or names a tab
// that is not a folder — and the tab stays top-level.
void FolderTreeTest::unknownFolderIdStaysTopLevel()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    const ItemLocation deltaNowhere = makeTab("tab-d", "Delta", 5, "folder-nowhere");
    h.delta(deltaNowhere, {makeItem("d1", "D1", "Sword", deltaNowhere)});
    const ItemLocation echoInTab = makeTab("tab-e", "Echo", 6, "tab-d");
    h.delta(echoInTab, {makeItem("e1", "E1", "Shield", echoInTab)});
    const QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "  #3, \"Beta\": B1 Sword",
                                              "#4, \"Folder G\"",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "#6, \"Delta\": D1 Sword",
                                              "#7, \"Echo\": E1 Shield"},
                                             filtered);
    CHECK_TREE(h, expected);
    h.snapshot();
    CHECK_TREE(h, expected);
}

void FolderTreeTest::viewModeRoundTripAfterDeltas_data()
{
    addFilteredColumn();
}

// Scenario 9: deltas applied in By-Tab, then By-Item (where they maintain
// the flat bucket only), then back to By-Tab — the rebuilt tree is the
// tree a refilter builds.
void FolderTreeTest::viewModeRoundTripAfterDeltas()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    auto *viewCombo = h.fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(viewCombo);

    const ItemLocation deltaInG = makeTab("tab-d", "Delta", 5, "folder-g");
    h.delta(deltaInG, {makeItem("d2", "D2", "Sword", deltaInG)});

    viewCombo->setCurrentIndex(1);
    emit viewCombo->activated(1);
    QCOMPARE(h.model->rowCount(), 1); // the flat bucket
    const ItemLocation betaLoose = makeTab("tab-b", "Beta", 2);
    h.delta(betaLoose, {makeItem("b2", "B2", "Sword", betaLoose)});
    h.delta(makeFolder("folder-g", "Folder G", 3), {});
    viewCombo->setCurrentIndex(0);
    emit viewCombo->activated(0);
    h.resets->clear(); // the mode switch is a D6 boundary

    const QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "#3, \"Beta\": B2 Sword",
                                              "#4, \"Folder G\"",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "  #6, \"Delta\": D2 Sword",
                                              "#7, \"Echo\": E1 Shield"},
                                             filtered);
    CHECK_TREE(h, expected);
}

void FolderTreeTest::nestedBucketSortsAndResorts_data()
{
    addFilteredColumn();
}

// Scenario 10: a nested bucket is expanded, sorted, re-sorted, and takes a
// delta while expanded — order follows the sort, and only that bucket
// sorts.
void FolderTreeTest::nestedBucketSortsAndResorts()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    seed.items["tab-a"] = {makeItem("a1", "Zulu", "Sword", seed.tab("tab-a")),
                           makeItem("a2", "Alpha", "Sword", seed.tab("tab-a")),
                           makeItem("a3", "Mike", "Sword", seed.tab("tab-a"))};
    Harness h(seed, filtered);

    const auto childNames = [&h](const QModelIndex &header) {
        QStringList names;
        for (int row = 0; row < h.model->rowCount(header); ++row) {
            names.append(h.model->index(row, 0, header).data().toString());
        }
        return names;
    };

    h.tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    const QModelIndex folder = h.model->index(0, 0);
    h.tree->expand(folder);
    const QModelIndex alpha = h.model->index(0, 0, folder);
    QCOMPARE(alpha.data().toString(), "#2, \"Alpha\"");
    h.tree->expand(alpha);
    QCOMPARE(childNames(alpha), QStringList({"Alpha Sword", "Mike Sword", "Zulu Sword"}));

    h.tree->sortByColumn(0, Qt::DescendingOrder);
    QCOMPARE(childNames(h.model->index(0, 0, h.model->index(0, 0))),
             QStringList({"Zulu Sword", "Mike Sword", "Alpha Sword"}));
    h.tree->sortByColumn(0, Qt::AscendingOrder);
    QCOMPARE(childNames(h.model->index(0, 0, h.model->index(0, 0))),
             QStringList({"Alpha Sword", "Mike Sword", "Zulu Sword"}));

    // A delta into the expanded nested bucket merges into sorted order.
    h.delta(h.world.tab("tab-a"),
            {makeItem("a1", "Zulu", "Sword", h.world.tab("tab-a")),
             makeItem("a4", "Bravo", "Sword", h.world.tab("tab-a")),
             makeItem("a3", "Mike", "Sword", h.world.tab("tab-a"))});
    QCOMPARE(childNames(h.model->index(0, 0, h.model->index(0, 0))),
             QStringList({"Bravo Sword", "Mike Sword", "Zulu Sword"}));
    QCOMPARE(h.resets->count(), 0);

    const QStringList expected
        = withoutEcho({"#1, \"Folder F\"",
                       "  #2, \"Alpha\": Bravo Sword, Mike Sword, Zulu Sword",
                       "  #3, \"Beta\": B1 Sword",
                       "#4, \"Folder G\"",
                       "  #5, \"Gamma\": C1 Sword",
                       "#6, \"Delta\": D1 Sword",
                       "#7, \"Echo\": E1 Shield"},
                      filtered);
    CHECK_TREE(h, expected);
}

void FolderTreeTest::moveIntoAbsentFolder_data()
{
    addFilteredColumn();
}

// Scenario 4 with the folder entered absent: in a filtered search folder G
// is hidden (its member has no match); a matching loose tab moves into it —
// the presence rule inserts G first and the tab lands under it.
void FolderTreeTest::moveIntoAbsentFolder()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    seed.items["tab-c"] = {makeItem("c1", "C1", "Shield", seed.tab("tab-c"))};
    Harness h(seed, filtered);
    const ItemLocation deltaInG = makeTab("tab-d", "Delta", 5, "folder-g");
    h.delta(deltaInG, {makeItem("d1", "D1", "Sword", deltaInG)});
    QStringList expected = withoutEcho({"#1, \"Folder F\"",
                                        "  #2, \"Alpha\": A1 Sword",
                                        "  #3, \"Beta\": B1 Sword",
                                        "#4, \"Folder G\"",
                                        "  #5, \"Gamma\": C1 Shield",
                                        "  #6, \"Delta\": D1 Sword",
                                        "#7, \"Echo\": E1 Shield"},
                                       filtered);
    if (filtered) {
        expected.removeAll("  #5, \"Gamma\": C1 Shield");
    }
    CHECK_TREE(h, expected);
}

void FolderTreeTest::snapshotOnlyMetadataSettlesTree_data()
{
    addFilteredColumn();
}

// Membership and position changes the snapshot alone carries (a tabs-only
// refresh streams no deltas): Beta moves from F to G, Delta joins F, and
// G's index moves ahead of F. The reconciliation settles the tree with
// moves, never a reset.
void FolderTreeTest::snapshotOnlyMetadataSettlesTree()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.world.setTab(makeTab("tab-b", "Beta", 2, "folder-g"));
    h.world.setTab(makeTab("tab-d", "Delta", 5, "folder-f"));
    h.world.setTab(makeFolder("folder-g", "Folder G", 0));
    h.world.setTab(makeFolder("folder-f", "Folder F", 3));
    QSignalSpy moves(h.model, &QAbstractItemModel::rowsMoved);
    h.snapshot();
    QVERIFY(moves.count() > 0);
    const QStringList expected = withoutEcho({"#1, \"Folder G\"",
                                              "  #3, \"Beta\": B1 Sword",
                                              "  #5, \"Gamma\": C1 Sword",
                                              "#4, \"Folder F\"",
                                              "  #2, \"Alpha\": A1 Sword",
                                              "  #6, \"Delta\": D1 Sword",
                                              "#7, \"Echo\": E1 Shield"},
                                             filtered);
    CHECK_TREE(h, expected);
}

// A selected item in a nested bucket stays selected through scenario 4's
// cross-parent move and scenario 5's folder move (D8 via S0's result).
void FolderTreeTest::nestedSelectionSurvivesMoves()
{
    Harness h(baseWorld(), false);
    auto *locationLabel = h.fixture.window->findChild<QLabel *>("locationLabel");
    QVERIFY(locationLabel);

    const QModelIndex folder = h.model->index(0, 0);
    h.tree->expand(folder);
    const QModelIndex alpha = h.model->index(0, 0, folder);
    h.tree->expand(alpha);
    const QModelIndex item = h.model->index(0, 0, alpha);
    QCOMPARE(item.data().toString(), "A1 Sword");
    h.tree->selectionModel()->setCurrentIndex(item,
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    QTRY_COMPARE_WITH_TIMEOUT(locationLabel->text(), QString("#2, \"Alpha\""), 2000);

    const auto selectedNames = [&h] {
        QStringList names;
        for (const QModelIndex &index : h.tree->selectionModel()->selectedRows()) {
            names.append(index.data().toString());
        }
        return names;
    };

    // Scenario 5: the folder moves, carrying the selected item's tab.
    h.delta(makeFolder("folder-f", "Folder F", 8), {});
    QCOMPARE(selectedNames(), QStringList({"A1 Sword"}));
    QCOMPARE(h.tree->selectionModel()->currentIndex().data().toString(), "A1 Sword");
    QVERIFY(h.tree->isExpanded(h.tree->selectionModel()->currentIndex().parent()));

    // Scenario 4: the tab moves to another folder, then out — a metadata
    // delta without replacing the selected item object.
    const auto a1 = h.world.items["tab-a"];
    const ItemLocation alphaInG = makeTab("tab-a", "Alpha", 1, "folder-g");
    h.fixture.itemsManager->OnTabRefreshed(alphaInG, a1);
    QCOMPARE(selectedNames(), QStringList({"A1 Sword"}));
    QCOMPARE(h.tree->selectionModel()->currentIndex().parent().parent().data().toString(),
             "#4, \"Folder G\"");
    const ItemLocation alphaLoose = makeTab("tab-a", "Alpha", 1);
    h.fixture.itemsManager->OnTabRefreshed(alphaLoose, a1);
    QCOMPARE(selectedNames(), QStringList({"A1 Sword"}));
    QVERIFY(!h.tree->selectionModel()->currentIndex().parent().parent().isValid());

    // A replacement delta re-adopts the selection by stable id.
    const ItemLocation alphaBack = makeTab("tab-a", "Alpha", 1, "folder-g");
    h.fixture.itemsManager->OnTabRefreshed(alphaBack, {makeItem("a1", "A1", "Sword", alphaBack)});
    QCOMPARE(selectedNames(), QStringList({"A1 Sword"}));
    QCOMPARE(h.resets->count(), 0);
}

// An account with no folders keeps today's flat tree: every bucket is a
// top-level header whose children are items.
void FolderTreeTest::foldersUnaffectedAccountKeepsFlatTree()
{
    World world;
    world.setTab(makeTab("tab-a", "Alpha", 0));
    world.setTab(makeTab("tab-b", "Beta", 1));
    world.items["tab-a"] = {makeItem("a1", "A1", "Sword", world.tab("tab-a"))};
    Harness h(world, false);
    QCOMPARE(h.model->rowCount(), 2);
    for (int row = 0; row < 2; ++row) {
        const QModelIndex header = h.model->index(row, 0);
        QVERIFY(h.model->Locate(header).isBucket());
        QCOMPARE(h.model->Locate(header).bucket_row, row);
        QCOMPARE(h.model->BucketIndex(row), header);
        for (int child = 0; child < h.model->rowCount(header); ++child) {
            QVERIFY(h.model->Locate(h.model->index(child, 0, header)).isItem());
        }
    }
    CHECK_TREE(h, QStringList({"#1, \"Alpha\": A1 Sword", "#2, \"Beta\": "}));
}

void FolderTreeTest::folderStormConverges_data()
{
    addFilteredColumn();
}

// A randomized storm over every folder-shaped mutation the pipeline can
// deliver — content and empty deltas, membership moves, folder and tab
// index changes, folder and tab discovery (folder before or after its
// members, or never), snapshots that delete folders and tabs, expansion,
// sorts and mode switches — with the model tester attached throughout.
// Each snapshot boundary checks convergence against a fresh refilter.
void FolderTreeTest::folderStormConverges()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    Harness h(seed, filtered);
    auto *viewCombo = h.fixture.window->findChild<QComboBox *>("viewComboBox");
    QVERIFY(viewCombo);

    std::mt19937 rng(filtered ? 20261111u : 1111u);
    const auto pick = [&rng](int n) { return std::uniform_int_distribution<int>(0, n - 1)(rng); };
    int counter = 0;
    QStringList folder_ids({"folder-f", "folder-g"});
    const auto randomFolderId = [&]() -> QString {
        const int choice = pick(static_cast<int>(folder_ids.size()) + 2);
        if (choice < folder_ids.size()) {
            return folder_ids[choice];
        }
        return (choice == folder_ids.size()) ? QString() : QString("folder-unknown");
    };
    const auto tabIds = [&h] {
        QStringList ids;
        for (const auto &tab : h.world.tabs) {
            if (!tab.IsFolder()) {
                ids.append(tab.id());
            }
        }
        return ids;
    };
    const auto freshItems = [&](const ItemLocation &tab) {
        Items items;
        const int count = pick(3);
        for (int n = 0; n < count; ++n) {
            const QString id = QString("s%1").arg(counter++);
            items.push_back(makeItem(id, id, (pick(2) == 0) ? "Sword" : "Shield", tab));
        }
        return items;
    };

    // The delta path is checked mid-refresh too: a refilter of the same
    // published state (the deltas' items and anchors) must build the tree
    // the deltas maintained — the snapshot's settle would otherwise repair
    // a delta-path divergence before anything looked.
    int snapshots = 0;
    int probes = 0;
    for (int step = 0; step < 300; ++step) {
        const QStringList tabs = tabIds();
        const int action = pick(11);
        if (((action == 6) || (action == 10)) && (viewCombo->currentIndex() == 0)) {
            const QStringList applied = treeSignature(*h.tree);
            QCOMPARE(h.resets->count(), 0);
            QCOMPARE(h.refiltered(), applied);
            ++probes;
        }
        switch (action) {
        case 0:
        case 1: { // content or empty delta for a tab
            const ItemLocation &tab = h.world.tab(tabs[pick(static_cast<int>(tabs.size()))]);
            h.delta(tab, freshItems(tab));
            break;
        }
        case 2: { // membership move (into, out of, between, or unknown)
            const ItemLocation &old = h.world.tab(tabs[pick(static_cast<int>(tabs.size()))]);
            const ItemLocation moved = makeTab(old.id(),
                                               old.tab_label(),
                                               static_cast<unsigned>(old.tab_index()),
                                               randomFolderId());
            h.delta(moved, freshItems(moved));
            break;
        }
        case 3: { // index change of a tab or a folder
            const ItemLocation &old
                = h.world.tabs[static_cast<size_t>(pick(static_cast<int>(h.world.tabs.size())))];
            const unsigned index = static_cast<unsigned>(pick(30));
            if (old.IsFolder()) {
                h.delta(makeFolder(old.id(), old.tab_label(), index), {});
            } else {
                const ItemLocation moved = makeTab(old.id(),
                                                   old.tab_label(),
                                                   index,
                                                   old.folder_id());
                h.delta(moved, h.world.items[old.id()]);
            }
            break;
        }
        case 4: { // tab discovery, possibly naming a folder not yet known
            const QString id = QString("tab-n%1").arg(counter++);
            QString folder = randomFolderId();
            if (pick(3) == 0) {
                folder = QString("folder-n%1").arg(counter);
            }
            const ItemLocation tab = makeTab(id, id, static_cast<unsigned>(pick(30)), folder);
            h.delta(tab, freshItems(tab));
            break;
        }
        case 5: { // folder discovery — sometimes one members already name
            QString id = QString("folder-n%1").arg(counter++);
            for (const auto &tab : h.world.tabs) {
                if (!tab.folder_id().isEmpty() && !folder_ids.contains(tab.folder_id())
                    && (tab.folder_id() != "folder-unknown")) {
                    id = tab.folder_id();
                    break;
                }
            }
            folder_ids.append(id);
            h.delta(makeFolder(id, id, static_cast<unsigned>(pick(30))), {});
            break;
        }
        case 6: { // snapshot, deleting a folder or a tab now and then
            if ((pick(3) == 0) && (folder_ids.size() > 1)) {
                const QString id = folder_ids.takeAt(pick(static_cast<int>(folder_ids.size())));
                h.world.removeTab(id);
            }
            if ((pick(3) == 0) && (tabs.size() > 3)) {
                h.world.removeTab(tabs[pick(static_cast<int>(tabs.size()))]);
            }
            h.snapshot();
            ++snapshots;
            if (viewCombo->currentIndex() == 0) {
                const QStringList applied = treeSignature(*h.tree);
                QCOMPARE(h.resets->count(), 0);
                QCOMPARE(h.refiltered(), applied);
            }
            break;
        }
        case 7: { // expansion change on a random header
            const int bucket_count = h.model->rowCount();
            if (bucket_count > 0) {
                QModelIndex header = h.model->index(pick(bucket_count), 0);
                const QModelIndex child = h.model->index(0, 0, header);
                if (child.isValid() && h.model->Locate(child).isBucket() && (pick(2) == 0)) {
                    header = child;
                }
                if (h.tree->isExpanded(header)) {
                    h.tree->collapse(header);
                } else {
                    h.tree->expand(header);
                }
            }
            break;
        }
        case 8: { // sort click
            static constexpr int columns[] = {0, 1, 2, 4};
            h.tree->sortByColumn(columns[pick(4)],
                                 (pick(2) == 0) ? Qt::AscendingOrder : Qt::DescendingOrder);
            break;
        }
        case 9: { // view-mode switch through the user entry
            const int mode = (pick(4) == 0) ? 1 : 0;
            viewCombo->setCurrentIndex(mode);
            emit viewCombo->activated(mode);
            h.resets->clear(); // a mode switch is a D6 boundary
            break;
        }
        }
    }

    viewCombo->setCurrentIndex(0);
    emit viewCombo->activated(0);
    h.resets->clear();
    h.snapshot();
    QVERIFY2(snapshots >= 15, "snapshot boundary underexercised");
    QVERIFY2(probes >= 30, "mid-refresh convergence underexercised");
    const QStringList applied = treeSignature(*h.tree);
    QVERIFY(applied.size() > 3);
    QCOMPARE(h.resets->count(), 0);
    QCOMPARE(h.refiltered(), applied);
}

namespace {

    QModelIndex headerFor(const QAbstractItemModel &model, const QString &header)
    {
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex top = model.index(row, 0);
            if (top.data().toString().startsWith(header)) {
                return top;
            }
            for (int child = 0; child < model.rowCount(top); ++child) {
                const QModelIndex nested = model.index(child, 0, top);
                if (nested.data().toString().startsWith(header)) {
                    return nested;
                }
            }
        }
        return QModelIndex();
    }

    // Every bucket header's expansion, as "header: expanded" lines.
    QStringList expansionSignature(const QTreeView &view)
    {
        const auto *model = qobject_cast<const ItemsModel *>(view.model());
        QStringList lines;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex top = model->index(row, 0);
            lines.append(top.data().toString() + (view.isExpanded(top) ? " +" : " -"));
            for (int child = 0; child < model->rowCount(top); ++child) {
                const QModelIndex nested = model->index(child, 0, top);
                if (model->Locate(nested).isBucket()) {
                    lines.append("  " + nested.data().toString()
                                 + (view.isExpanded(nested) ? " +" : " -"));
                }
            }
        }
        return lines;
    }

    QTabBar *findSearchTabs(MainWindow &window)
    {
        for (auto *tabs : window.findChildren<QTabBar *>()) {
            if ((tabs->count() > 0) && (tabs->tabText(tabs->count() - 1) == "+")) {
                return tabs;
            }
        }
        return nullptr;
    }

} // namespace

void FolderTreeTest::foldersExpandByDefault_data()
{
    addFilteredColumn();
}

// D9 / R3 / A3: folders are expanded after a reset; tab buckets keep
// today's rule — collapsed in an unfiltered search, expanded in a filtered
// one — whether top-level or nested.
void FolderTreeTest::foldersExpandByDefault()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    const QString tab = filtered ? " +" : " -";
    QStringList expected({"#1, \"Folder F\" +",
                          "  #2, \"Alpha\"" + tab,
                          "  #3, \"Beta\"" + tab,
                          "#4, \"Folder G\" +",
                          "  #5, \"Gamma\"" + tab,
                          "#6, \"Delta\"" + tab});
    if (!filtered) {
        expected.append("#7, \"Echo\" -"); // a filtered search hides it
    }
    QCOMPARE(expansionSignature(*h.tree), expected);
}

void FolderTreeTest::collapsedFolderStaysCollapsed_data()
{
    addFilteredColumn();
}

// D9: each search keeps the folders the user collapsed; such a folder
// stays collapsed across resets and re-insertion, and expanding it again
// clears the mark. Another search keeps its own set.
void FolderTreeTest::collapsedFolderStaysCollapsed()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    h.tree->collapse(headerFor(*h.model, "#1, \"Folder F\""));

    h.refiltered();
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, "#1, \"Folder F\"")));
    QVERIFY(h.tree->isExpanded(headerFor(*h.model, "#4, \"Folder G\"")));

    // Re-insertion: the folder leaves and comes back — through its last
    // member in a filtered search, through the snapshot in an unfiltered
    // one.
    if (filtered) {
        h.delta(h.world.tab("tab-a"), {});
        h.delta(h.world.tab("tab-b"), {});
        QVERIFY(!headerFor(*h.model, "#1, \"Folder F\"").isValid());
        h.delta(h.world.tab("tab-a"), {makeItem("a2", "A2", "Sword", h.world.tab("tab-a"))});
    } else {
        const ItemLocation folder = h.world.tab("folder-f");
        h.world.removeTab("folder-f");
        h.snapshot();
        QVERIFY(!headerFor(*h.model, "#1, \"Folder F\"").isValid());
        h.world.setTab(folder);
        h.snapshot();
    }
    QVERIFY(headerFor(*h.model, "#1, \"Folder F\"").isValid());
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, "#1, \"Folder F\"")));

    // Another search has its own set.
    auto *tabs = findSearchTabs(*h.fixture.window);
    QVERIFY(tabs);
    tabs->setCurrentIndex(1);
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(h.tree->isExpanded(headerFor(*h.tree->model(), "#1, \"Folder F\"")));
    tabs->setCurrentIndex(0);
    QVERIFY(!h.tree->isExpanded(headerFor(*h.tree->model(), "#1, \"Folder F\"")));

    // Expanding it again clears the mark.
    h.tree->expand(headerFor(*h.tree->model(), "#1, \"Folder F\""));
    h.fixture.window->OnSearchFormChange();
    QVERIFY(h.tree->isExpanded(headerFor(*h.tree->model(), "#1, \"Folder F\"")));
}

void FolderTreeTest::insertedFolderExpands_data()
{
    addFilteredColumn();
}

// D9: a folder inserted by a delta or the snapshot is expanded, in either
// kind of search.
void FolderTreeTest::insertedFolderExpands()
{
    QFETCH(bool, filtered);
    Harness h(baseWorld(), filtered);
    const ItemLocation hotel = makeTab("tab-h", "Hotel", 10, "folder-k");
    h.delta(hotel, {makeItem("h1", "H1", "Sword", hotel)});
    h.delta(makeFolder("folder-k", "Folder K", 9), {});
    const QModelIndex folder = headerFor(*h.model, "#10, \"Folder K\"");
    QVERIFY(folder.isValid());
    QVERIFY(h.tree->isExpanded(folder));
    QCOMPARE(h.tree->isExpanded(headerFor(*h.model, "#11, \"Hotel\"")), filtered);

    // A folder the snapshot inserts.
    h.world.setTab(makeFolder("folder-l", "Folder L", 11));
    h.world.setTab(makeTab("tab-i", "India", 12, "folder-l"));
    h.world.items["tab-i"] = {makeItem("i1", "I1", "Sword", h.world.tab("tab-i"))};
    h.snapshot();
    QVERIFY(h.tree->isExpanded(headerFor(*h.model, "#12, \"Folder L\"")));
}

// D9: Expand All and Collapse All apply to every bucket header, folders
// and nested tabs included, and to no item row; Collapse All's folder
// collapses are user collapses (sticky), Expand All's expansions clear
// them.
void FolderTreeTest::expandAllAndCollapseAllCoverNestedHeaders()
{
    Harness h(baseWorld(), false);
    h.fixture.window->OnExpandAll();
    QCOMPARE(expansionSignature(*h.tree),
             QStringList({"#1, \"Folder F\" +",
                          "  #2, \"Alpha\" +",
                          "  #3, \"Beta\" +",
                          "#4, \"Folder G\" +",
                          "  #5, \"Gamma\" +",
                          "#6, \"Delta\" +",
                          "#7, \"Echo\" +"}));
    const QModelIndex alpha = headerFor(*h.model, "#2, \"Alpha\"");
    QVERIFY(!h.tree->isExpanded(h.model->index(0, 0, alpha)));

    h.fixture.window->OnCollapseAll();
    const QStringList collapsed({"#1, \"Folder F\" -",
                                 "  #2, \"Alpha\" -",
                                 "  #3, \"Beta\" -",
                                 "#4, \"Folder G\" -",
                                 "  #5, \"Gamma\" -",
                                 "#6, \"Delta\" -",
                                 "#7, \"Echo\" -"});
    QCOMPARE(expansionSignature(*h.tree), collapsed);
    h.fixture.window->OnSearchFormChange();
    QCOMPARE(expansionSignature(*h.tree), collapsed);

    h.fixture.window->OnExpandAll();
    h.fixture.window->OnSearchFormChange();
    QCOMPARE(expansionSignature(*h.tree),
             QStringList({"#1, \"Folder F\" +",
                          "  #2, \"Alpha\" +",
                          "  #3, \"Beta\" +",
                          "#4, \"Folder G\" +",
                          "  #5, \"Gamma\" +",
                          "#6, \"Delta\" +",
                          "#7, \"Echo\" +"}));
}

// D9: a refresh that replaces the selected item re-adopts the selection
// (M3 R1-3) without expanding what the user collapsed around it. Making a
// row current has the view expand every collapsed ancestor of it, and a
// folder expanded that way would leave the user-collapsed set for good.
void FolderTreeTest::collapsedRowsSurviveReselectionOnRefresh_data()
{
    QTest::addColumn<QString>("collapse");
    QTest::newRow("folder") << "#1, \"Folder F\"";
    QTest::newRow("tab") << "#2, \"Alpha\"";
}

void FolderTreeTest::collapsedRowsSurviveReselectionOnRefresh()
{
    QFETCH(QString, collapse);
    Harness h(baseWorld(), false);
    // The view only scrolls to (and expands towards) a new current row
    // while it is visible.
    h.fixture.window->show();
    QVERIFY(QTest::qWaitForWindowExposed(h.fixture.window.get()));

    h.tree->expand(headerFor(*h.model, "#2, \"Alpha\""));
    const QModelIndex item = h.model->index(0, 0, headerFor(*h.model, "#2, \"Alpha\""));
    QVERIFY(item.isValid());
    h.tree->selectionModel()->setCurrentIndex(item,
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    h.tree->collapse(headerFor(*h.model, collapse));
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, collapse)));

    // The tab's refresh replaces the selected item's object under the same
    // id: the old row leaves and the selection is re-adopted on the new one.
    const ItemLocation tab = h.world.tab("tab-a");
    h.delta(tab, {makeItem("a1", "A1", "Sword", tab)});
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, collapse)));
    const QModelIndex adopted = h.model->index(0, 0, headerFor(*h.model, "#2, \"Alpha\""));
    QVERIFY(adopted.isValid());
    QCOMPARE(h.tree->currentIndex(), adopted);
    QVERIFY(h.tree->selectionModel()->isSelected(adopted));

    h.snapshot();
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, collapse)));
    h.refiltered();
    QVERIFY(!h.tree->isExpanded(headerFor(*h.model, collapse)));
}

// D9: expanding or collapsing a folder sorts nothing, builds no keys and
// evicts none; a tab expanded inside a collapsed folder stays
// materialized.
void FolderTreeTest::folderExpansionMaterializesNothing()
{
    World seed = baseWorld();
    seed.items["tab-a"] = {makeItem("a1", "Zulu", "Sword", seed.tab("tab-a")),
                           makeItem("a2", "Alpha", "Sword", seed.tab("tab-a"))};
    Harness h(seed, false);
    h.tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    h.tree->expand(headerFor(*h.model, "#2, \"Alpha\""));

    auto &probes = ModelProbes::instance();
    probes.reset();
    probes.enabled = true;
    const std::int64_t live = probes.live_key_bytes;
    QVERIFY(live > 0);
    const QModelIndex folder = headerFor(*h.model, "#1, \"Folder F\"");
    h.tree->collapse(folder);
    QCOMPARE(probes.live_key_bytes, live);
    h.tree->expand(folder);
    QCOMPARE(probes.bucket_sorts, 0);
    QCOMPARE(probes.key_builds, 0);
    QCOMPARE(probes.live_key_bytes, live);
    QVERIFY(h.tree->isExpanded(headerFor(*h.model, "#2, \"Alpha\"")));
    probes.enabled = false;
}

// D9: a nested tab's expansion is saved and restored across a user
// refilter like a top-level tab's.
void FolderTreeTest::nestedTabExpansionSurvivesRefilter()
{
    Harness h(baseWorld(), false);
    h.tree->expand(headerFor(*h.model, "#2, \"Alpha\""));
    h.tree->expand(headerFor(*h.model, "#6, \"Delta\""));
    h.fixture.window->OnSearchFormChange();
    QCOMPARE(expansionSignature(*h.tree),
             QStringList({"#1, \"Folder F\" +",
                          "  #2, \"Alpha\" +",
                          "  #3, \"Beta\" -",
                          "#4, \"Folder G\" +",
                          "  #5, \"Gamma\" -",
                          "#6, \"Delta\" +",
                          "#7, \"Echo\" -"}));
}

// D9: selecting a folder header shows its header text in the details pane.
void FolderTreeTest::folderHeaderSelectionShowsHeaderText()
{
    Harness h(baseWorld(), false);
    auto *nameLabel = h.fixture.window->findChild<QLabel *>("nameLabel");
    QVERIFY(nameLabel);
    h.tree->selectionModel()->setCurrentIndex(headerFor(*h.model, "#4, \"Folder G\""),
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    QCOMPARE(nameLabel->text(), QString("#4, \"Folder G\""));
}

// D9: the scroll anchor resolves a nested tab's item through the
// conversions — the same item is back on top after a user refilter.
void FolderTreeTest::nestedScrollAnchorSurvivesRefilter()
{
    World seed = baseWorld();
    Items many;
    for (int n = 0; n < 60; ++n) {
        many.push_back(makeItem(QString("b%1").arg(n),
                                QString("Item %1").arg(n, 2, 10, QChar('0')),
                                "Sword",
                                seed.tab("tab-b")));
    }
    seed.items["tab-b"] = many;
    Harness h(seed, false);
    h.fixture.window->resize(900, 500);
    h.fixture.window->show();
    h.tree->header()->setSortIndicator(0, Qt::AscendingOrder);
    const QModelIndex beta = headerFor(*h.model, "#3, \"Beta\"");
    h.tree->expand(beta);
    QTRY_VERIFY_WITH_TIMEOUT(h.tree->verticalScrollBar()->maximum() > 0, 2000);

    const QModelIndex anchor = h.model->index(30, 0, headerFor(*h.model, "#3, \"Beta\""));
    QCOMPARE(anchor.data().toString(), "Item 30 Sword");
    h.tree->scrollTo(anchor, QAbstractItemView::PositionAtTop);
    QCOMPARE(h.tree->indexAt(QPoint(0, 0)).data().toString(), "Item 30 Sword");

    h.fixture.window->OnSearchFormChange();
    QCOMPARE(h.tree->indexAt(QPoint(0, 0)).data().toString(), "Item 30 Sword");
}

namespace {

    QVariant checkState(const QModelIndex &index)
    {
        return index.data(Qt::CheckStateRole);
    }

    void selectRows(QTreeView &view, const QModelIndexList &rows)
    {
        view.selectionModel()->clearSelection();
        for (const QModelIndex &row : rows) {
            view.selectionModel()->select(row,
                                          QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        if (!rows.isEmpty()) {
            view.selectionModel()->setCurrentIndex(rows.front(), QItemSelectionModel::NoUpdate);
        }
    }

    QStringList locationIds(const std::vector<ItemLocation> &locations)
    {
        QStringList ids;
        for (const auto &location : locations) {
            ids.append(location.id());
        }
        ids.sort();
        return ids;
    }

} // namespace

void FolderTreeTest::folderCheckStateDerivesFromVisibleMembers_data()
{
    addFilteredColumn();
}

// D10 / R5: a folder's check state is derived from its members present in
// the current search — all refreshing, none, or mixed — and never from a
// stored flag on the folder id. A folder with no present members shows no
// checkbox and is not user-checkable.
void FolderTreeTest::folderCheckStateDerivesFromVisibleMembers()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    // Beta has no match, so a filtered search shows only Alpha under F.
    seed.items["tab-b"] = {makeItem("b1", "B1", "Shield", seed.tab("tab-b"))};
    seed.setTab(makeFolder("folder-h", "Folder H", 7));
    Harness h(seed, filtered);
    auto &buyouts = *h.fixture.buyoutFixture.manager;

    // A stored flag on the folder id is never read.
    buyouts.SetRefreshChecked(h.world.tab("folder-f"), false);
    const QModelIndex folder = headerFor(*h.model, "#1, \"Folder F\"");
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Checked));
    QVERIFY(h.model->flags(folder) & Qt::ItemIsUserCheckable);

    buyouts.SetRefreshChecked(h.world.tab("tab-b"), false);
    // R5: a filtered search does not show Beta, so it does not count.
    QCOMPARE(checkState(folder).toInt(),
             static_cast<int>(filtered ? Qt::Checked : Qt::PartiallyChecked));

    buyouts.SetRefreshChecked(h.world.tab("tab-a"), false);
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Unchecked));

    // No present members: no checkbox (an unfiltered search shows the
    // empty folder H; a filtered one hides it).
    const QModelIndex empty = headerFor(*h.model, "#8, \"Folder H\"");
    QCOMPARE(empty.isValid(), !filtered);
    if (!filtered) {
        QVERIFY(!checkState(empty).isValid());
        QVERIFY(!(h.model->flags(empty) & Qt::ItemIsUserCheckable));
    }
}

// D10: checking or unchecking a folder writes that value to each present
// member that is not refresh-locked — any value other than unchecked
// means checked — then repaints the members and the folder. The folder id
// is never written.
void FolderTreeTest::folderClickCascadesToVisibleMembers()
{
    Harness h(baseWorld(), false);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    buyouts.SetRefreshChecked(h.world.tab("folder-f"), false);
    const QModelIndex folder = headerFor(*h.model, "#1, \"Folder F\"");
    const QModelIndex alpha = headerFor(*h.model, "#2, \"Alpha\"");
    const QModelIndex beta = headerFor(*h.model, "#3, \"Beta\"");

    QSignalSpy changed(h.model, &QAbstractItemModel::dataChanged);
    QVERIFY(h.model->setData(folder, Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("tab-a")));
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("tab-b")));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-c"))); // another folder's member
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Unchecked));
    QCOMPARE(checkState(alpha).toInt(), static_cast<int>(Qt::Unchecked));
    // The members and the folder were repainted.
    QSet<QPersistentModelIndex> repainted;
    for (const auto &arguments : changed) {
        const QModelIndex top_left = arguments.at(0).value<QModelIndex>();
        const QModelIndex bottom_right = arguments.at(1).value<QModelIndex>();
        for (int row = top_left.row(); row <= bottom_right.row(); ++row) {
            repainted.insert(h.model->index(row, 0, top_left.parent()));
        }
    }
    QVERIFY(repainted.contains(folder));
    QVERIFY(repainted.contains(alpha));
    QVERIFY(repainted.contains(beta));

    // Partially checked (the S0 click value is Checked; any non-unchecked
    // value checks).
    QVERIFY(h.model->setData(folder, Qt::PartiallyChecked, Qt::CheckStateRole));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-a")));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-b")));
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Checked));
    // Never written to the folder id.
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("folder-f")));
}

// D10: a refresh-locked member counts as refreshing and is never written;
// unchecking a folder that holds one leaves the folder partially checked
// (the accepted glyph overlap).
void FolderTreeTest::lockedMemberKeepsFolderPartial()
{
    Harness h(baseWorld(), false);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    buyouts.SetRefreshLocked(h.world.tab("tab-a"));
    const QModelIndex folder = headerFor(*h.model, "#1, \"Folder F\"");
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Checked));

    QVERIFY(h.model->setData(folder, Qt::Unchecked, Qt::CheckStateRole));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-a")));
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("tab-b")));
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::PartiallyChecked));
    QCOMPARE(checkState(headerFor(*h.model, "#2, \"Alpha\"")).toInt(),
             static_cast<int>(Qt::PartiallyChecked)); // the locked glyph

    QVERIFY(h.model->setData(folder, Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::Checked));
}

// D10: changing a nested tab's checkbox repaints its folder header.
void FolderTreeTest::nestedTabCheckRepaintsFolder()
{
    Harness h(baseWorld(), false);
    const QModelIndex folder = headerFor(*h.model, "#1, \"Folder F\"");
    const QModelIndex alpha = headerFor(*h.model, "#2, \"Alpha\"");
    QSignalSpy changed(h.model, &QAbstractItemModel::dataChanged);
    QVERIFY(h.model->setData(alpha, Qt::Unchecked, Qt::CheckStateRole));
    bool folder_repainted = false;
    for (const auto &arguments : changed) {
        const QModelIndex top_left = arguments.at(0).value<QModelIndex>();
        const QModelIndex bottom_right = arguments.at(1).value<QModelIndex>();
        folder_repainted = folder_repainted
                           || ((top_left.parent() == folder.parent())
                               && (top_left.row() <= folder.row())
                               && (bottom_right.row() >= folder.row()));
    }
    QVERIFY(folder_repainted);
    QCOMPARE(checkState(folder).toInt(), static_cast<int>(Qt::PartiallyChecked));
}

// D10: Check/Uncheck Selected on a folder row behave as a click; Check
// All/Uncheck All write every bucket except folders.
void FolderTreeTest::checkSelectedAndCheckAllOnFolders()
{
    Harness h(baseWorld(), false);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    buyouts.SetRefreshChecked(h.world.tab("folder-g"), false);

    selectRows(*h.tree, {headerFor(*h.model, "#1, \"Folder F\"")});
    h.fixture.window->OnUncheckSelected();
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("tab-a")));
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("tab-b")));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("folder-f"))); // never written
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-c")));
    h.fixture.window->OnCheckSelected();
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-a")));
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("tab-b")));

    h.fixture.window->OnUncheckAll();
    for (const QString &id : {"tab-a", "tab-b", "tab-c", "tab-d", "tab-e"}) {
        QVERIFY2(!buyouts.GetRefreshChecked(h.world.tab(id)), qPrintable(id));
    }
    QVERIFY(buyouts.GetRefreshChecked(h.world.tab("folder-f")));
    h.fixture.window->OnCheckAll();
    for (const QString &id : {"tab-a", "tab-b", "tab-c", "tab-d", "tab-e"}) {
        QVERIFY2(buyouts.GetRefreshChecked(h.world.tab(id)), qPrintable(id));
    }
    QVERIFY(!buyouts.GetRefreshChecked(h.world.tab("folder-g"))); // never written
}

// D10: refreshCheckStates repaints every header row's check state, nested
// headers included.
void FolderTreeTest::refreshCheckStatesCoversNestedHeaders()
{
    Harness h(baseWorld(), false);
    QSignalSpy changed(h.model, &QAbstractItemModel::dataChanged);
    h.model->refreshCheckStates();
    QSet<QPersistentModelIndex> repainted;
    for (const auto &arguments : changed) {
        const QModelIndex top_left = arguments.at(0).value<QModelIndex>();
        const QModelIndex bottom_right = arguments.at(1).value<QModelIndex>();
        QVERIFY(qvariant_cast<QList<int>>(arguments.at(2)).contains(Qt::CheckStateRole));
        for (int row = top_left.row(); row <= bottom_right.row(); ++row) {
            repainted.insert(h.model->index(row, 0, top_left.parent()));
        }
    }
    for (const QString &header : {"#1, \"Folder F\"",
                                  "#2, \"Alpha\"",
                                  "#3, \"Beta\"",
                                  "#4, \"Folder G\"",
                                  "#5, \"Gamma\"",
                                  "#6, \"Delta\"",
                                  "#7, \"Echo\""}) {
        QVERIFY2(repainted.contains(headerFor(*h.model, header)), qPrintable(header));
    }
}

void FolderTreeTest::refreshSelectedOnFolderSendsCheckedMembers_data()
{
    addFilteredColumn();
}

// R6 / A1 / R9: Refresh Selected on a folder row sends the folder and its
// members present in the current search whose refresh state is checked.
void FolderTreeTest::refreshSelectedOnFolderSendsCheckedMembers()
{
    QFETCH(bool, filtered);
    World seed = baseWorld();
    seed.setTab(makeTab("tab-i", "India", 3, "folder-f"));
    seed.items["tab-i"] = {makeItem("i1", "I1", "Shield", seed.tab("tab-i"))};
    Harness h(seed, filtered);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    buyouts.SetRefreshChecked(h.world.tab("tab-b"), false);

    std::vector<std::pair<Util::TabSelection, std::vector<ItemLocation>>> updates;
    QObject::connect(h.fixture.itemsManager.get(),
                     &ItemsManager::UpdateSignal,
                     h.fixture.itemsManager.get(),
                     [&updates](Util::TabSelection type, const std::vector<ItemLocation> &tabs) {
                         updates.emplace_back(type, tabs);
                     });
    selectRows(*h.tree, {headerFor(*h.model, "#1, \"Folder F\"")});
    h.fixture.window->OnRefreshSelected();
    QCOMPARE(updates.size(), size_t(1));
    QVERIFY(updates.front().first == Util::TabSelection::Selected);
    const auto &sent = updates.front().second;
    // India is a checked member only where it is present (unfiltered).
    QCOMPARE(locationIds(sent),
             filtered ? QStringList({"folder-f", "tab-a"})
                      : QStringList({"folder-f", "tab-a", "tab-i"}));
}

// D11 / R7: with a folder header current the buyout widgets are disabled,
// whatever an earlier version stored against the folder id — which is
// neither rendered nor removed.
void FolderTreeTest::folderHeaderDisablesBuyoutWidgets()
{
    Harness h(baseWorld(), false);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    buyouts.SetTab(h.world.tab("folder-g"), makeChaosBuyout(5));
    auto *type = h.fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = h.fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    auto *value = h.fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(type && currency && value);

    // A tab first: its widgets are live.
    h.tree->selectionModel()->setCurrentIndex(headerFor(*h.model, "#6, \"Delta\""),
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    QVERIFY(type->isEnabled());

    const QModelIndex folder = headerFor(*h.model, "#4, \"Folder G\"");
    h.tree->selectionModel()->setCurrentIndex(folder,
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    QVERIFY(!type->isEnabled());
    QVERIFY(!currency->isEnabled());
    QVERIFY(!value->isEnabled());
    QCOMPARE(value->text(), QString());
    // The stored tab buyout stays in the store and is not rendered.
    QVERIFY(buyouts.GetTab(h.world.tab("folder-g")).IsActive());
    QCOMPARE(folder.data().toString(), QString("#4, \"Folder G\""));
}

// D11: a buyout edit over a multi-selection skips folder rows; nothing
// cascades to members.
void FolderTreeTest::buyoutEditSkipsFolderRows()
{
    Harness h(baseWorld(), false);
    auto &buyouts = *h.fixture.buyoutFixture.manager;
    selectRows(*h.tree,
               {headerFor(*h.model, "#6, \"Delta\""), headerFor(*h.model, "#1, \"Folder F\"")});
    auto *type = h.fixture.window->findChild<QComboBox *>("buyoutTypeComboBox");
    auto *currency = h.fixture.window->findChild<QComboBox *>("buyoutCurrencyComboBox");
    auto *value = h.fixture.window->findChild<QLineEdit *>("buyoutValueLineEdit");
    QVERIFY(type && currency && value);
    type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    currency->setCurrentIndex(Currency::CURRENCY_CHAOS_ORB);
    value->setText("7");
    h.fixture.window->OnBuyoutChange();

    QVERIFY(buyouts.GetTab(h.world.tab("tab-d")).IsActive());
    QVERIFY(!buyouts.GetTab(h.world.tab("folder-f")).IsActive());
    QVERIFY(!buyouts.GetTab(h.world.tab("tab-a")).IsActive());
    QVERIFY(!buyouts.GetTab(h.world.tab("tab-b")).IsActive());
}

QTEST_MAIN(FolderTreeTest)

#include "tst_foldertree.moc"
