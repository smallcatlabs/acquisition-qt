// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// M3 hold-point harness (items-pipeline M3, implementation sequence,
// working rule 3): runs the conditional budget rows for the stages that
// have one, against the real MainWindow/Search/ItemsModel path on the
// recorded spike presets. Not a test: run by hand, offscreen, in a
// Release build:
//
//   ./m3_holdpoint_benchmark --preset 100k
//   ./m3_holdpoint_benchmark --preset 1m
//
// S3 rows (this file's initial scope): worst-case unfiltered By-Tab
// refilter (default collapsed, sort share attributed); single-bucket
// expand with cold keys; broad-filter default-expanded refilter (R1-8's
// worst case, driven by the ilvl >= 2 filter, which matches ~99% of the
// dataset while still excluding items); collapsed-default and background
// resident-key memory. S4 row (reshaped in review round 1): delta
// application on the current search, By-Tab visible bucket — the
// budgeted shape is the R2-2 interleaved case, a synthetic child fetch
// source replaced inside the largest expanded bucket so removal runs
// scatter through retained sibling-source rows and the merge interleaves
// against them; the simple single-source full replacement is kept as an
// informational row. S5 rows: By-Item full refilter, clean By-Item
// reactivation (the R3-1 eager hydration), the D4 flat-bucket merge
// (same interleaved child source, By-Item active), and the worst-shape
// resident key memory — gauge plus process-level footprint delta.
// S6 rows (informational, added in S6 review round 1): the clean final
// snapshot's row reconciliation, By-Tab and By-Item — elapsed plus
// lifetime-peak delta, no budget (the spec accepts O(collection) once
// per refresh; these keep the every-refresh path measured).
//
// S7 runs this same accumulated set as the formal complete-table M1-M3
// gate (the spec's acceptance-criteria budget table, authoritative on
// the finished model); the broad-filter row additionally records a
// process-level footprint delta so BOTH candidate worst materialized
// shapes of the ≤ 300 MB row are judged at process level. The process
// exits non-zero when any budgeted row or binding memory check misses,
// or when a binding measurement is unavailable on the platform (S7
// review round 1); the ru_maxrss peak observations stay informational.
//
// Attribution follows the M2-M2 discipline: the live windows are timed
// end-to-end, sort/key work is attributed by the model probes (counts)
// and by micro-benchmarks on identical data outside the timed windows
// (filter loop, key build + sort at collection scale).

#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollBar>
#include <QTabBar>
#include <QTreeView>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/resource.h>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "bucket.h"
#include "column.h"
#include "filters/filterspec.h"
#include "filters/filterstate.h"
#include "mainwindowfixture.h"
#include "modelprobes.h"
#include "search.h"
#include "spikedataset.h"

namespace {

    double toMs(qint64 ns)
    {
        return static_cast<double>(ns) / 1e6;
    }

    qint64 median(std::vector<qint64> values)
    {
        if (values.empty()) {
            return 0;
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    QLineEdit *findMinMaxMin(MainWindow &window, const QString &caption)
    {
        for (auto *label : window.findChildren<QLabel *>()) {
            if (label->text() != caption) {
                continue;
            }
            auto *group = label->parentWidget();
            if (!group) {
                continue;
            }
            const auto edits = group->findChildren<QLineEdit *>();
            if (!edits.isEmpty()) {
                return edits.front(); // min precedes max in the form layout
            }
        }
        return nullptr;
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

    void drainEvents()
    {
        for (int n = 0; n < 8; ++n) {
            QCoreApplication::processEvents(QEventLoop::AllEvents);
        }
    }

    // Process-level memory footprint (phys_footprint on macOS — the
    // number Activity Monitor calls "memory"). The S5/S7 resident-key
    // budget is stated at process level; the gauge is an estimate.
    std::int64_t processFootprintBytes()
    {
#ifdef __APPLE__
        task_vm_info_data_t info;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count)
            == KERN_SUCCESS) {
            return static_cast<std::int64_t>(info.phys_footprint);
        }
#endif
        return -1;
    }

    struct BudgetRow
    {
        const char *name;
        double measured_ms;
        double budget_ms; // < 0: informational, no budget at this preset
    };

    [[nodiscard]] bool printRows(const std::vector<BudgetRow> &rows)
    {
        bool missed = false;
        for (const auto &row : rows) {
            if (row.budget_ms < 0) {
                std::printf("  %-46s %10.3f ms   (informational)\n", row.name, row.measured_ms);
            } else {
                const bool pass = row.measured_ms <= row.budget_ms;
                missed = missed || !pass;
                std::printf("  %-46s %10.3f ms   budget %8.1f ms   %s\n",
                            row.name,
                            row.measured_ms,
                            row.budget_ms,
                            pass ? "PASS" : "MISS");
            }
        }
        return missed;
    }

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    QElapsedTimer clock;
    clock.start();

    QCommandLineParser parser;
    const QCommandLineOption preset_option("preset",
                                           "Dataset preset: smoke, 100k, or 1m.",
                                           "preset",
                                           "100k");
    parser.addOption(preset_option);
    parser.process(app);

    const QString preset_name = parser.value(preset_option);
    const auto preset = SpikeDataset::Config::Preset(preset_name);
    if (!preset) {
        std::fprintf(stderr, "unknown preset: %s\n", qPrintable(preset_name));
        return 1;
    }
    // The S3 budget table (items-pipeline-m3.md acceptance criteria);
    // smoke is functional-only.
    const bool is_100k = (preset_name == "100k");
    const bool is_1m = (preset_name == "1m");
    const double budget_refilter_ms = is_100k ? 60.0 : is_1m ? 500.0 : -1.0;
    const double budget_sort_share_ms = (is_100k || is_1m) ? 5.0 : -1.0;
    const double budget_expand_ms = (is_100k || is_1m) ? 10.0 : -1.0;
    const double budget_broad_ms = is_100k ? 150.0 : is_1m ? 1200.0 : -1.0;
    // The S4 row's budget is stated at 1m only; 100k is informational.
    const double budget_delta_ms = is_1m ? 5.0 : -1.0;

    auto main_logger = std::make_shared<spdlog::logger>("main");
    main_logger->sinks().push_back(std::make_shared<spdlog::sinks::dist_sink_mt>());
    spdlog::register_logger(main_logger);
    spdlog::set_level(spdlog::level::warn);

    // The dataset materializes Items, which consult the process-global
    // category tables the fixture would otherwise initialize later.
    InitializeMainWindowTestCategories();

    std::printf("M3 hold-point harness: building dataset preset %s...\n", qPrintable(preset_name));
    SpikeDataset dataset(*preset);
    Items all_items = dataset.allItems();
    std::vector<ItemLocation> locations;
    locations.reserve(static_cast<size_t>(dataset.tabCount()));
    for (int t = 0; t < dataset.tabCount(); ++t) {
        locations.push_back(dataset.location(t));
    }
    std::printf("  %d tabs, %zu items\n", dataset.tabCount(), all_items.size());

    MainWindowFixture fixture;
    auto *tree = fixture.window->findChild<QTreeView *>("treeView");
    auto *tabs = findSearchTabs(*fixture.window);
    auto *ilvl_min = findMinMaxMin(*fixture.window, "ilvl");
    if (!tree || !tabs || !ilvl_min) {
        std::fprintf(stderr, "fixture widgets not found\n");
        return 1;
    }

    // Initial population (production-faithful since S6: the cached-load
    // snapshot is initial_refresh=true and takes the reset path; the
    // non-initial reconciliation is measured by its own S6 rows below).
    qint64 t0 = clock.nsecsElapsed();
    fixture.itemsManager->OnItemsRefreshed(all_items, locations, true);
    drainEvents();
    std::printf("  initial publish + refilter: %.1f ms\n", toMs(clock.nsecsElapsed() - t0));

    auto &probes = ModelProbes::instance();
    std::vector<BudgetRow> rows;
    // The formal gate's memory rows are inline checks, not BudgetRows;
    // they accumulate here so a miss — or a required measurement that is
    // unavailable on this platform — fails the process, not just the eye.
    bool memory_missed = false;

    // --- Row 1: worst-case unfiltered By-Tab refilter (default collapsed) --
    {
        std::vector<qint64> samples;
        for (int rep = 0; rep < 5; ++rep) {
            t0 = clock.nsecsElapsed();
            fixture.window->OnSearchFormChange();
            samples.push_back(clock.nsecsElapsed() - t0);
            drainEvents();
        }
        rows.push_back(
            {"unfiltered By-Tab refilter (median of 5)", toMs(median(samples)), budget_refilter_ms});

        // Sort-share attribution: the collapsed-default refilter must sort
        // nothing and build no keys (D2) — the probes prove the share is
        // literally zero.
        probes.reset();
        probes.enabled = true;
        fixture.window->OnSearchFormChange();
        probes.enabled = false;
        std::printf("  [attribution] unfiltered refilter: bucket_sorts=%lld key_builds=%lld "
                    "keyed_compares=%lld\n",
                    static_cast<long long>(probes.bucket_sorts),
                    static_cast<long long>(probes.key_builds),
                    static_cast<long long>(probes.keyed_compares));
        rows.push_back({"  sort share (probe-attributed)",
                        (probes.bucket_sorts == 0 && probes.key_builds == 0) ? 0.0 : 1e9,
                        budget_sort_share_ms});
        drainEvents();

        // Memory row: the collapsed-default view holds no resident keys.
        memory_missed = memory_missed || (probes.live_key_bytes != 0);
        std::printf("  [memory] collapsed-default resident key bytes: %lld (row: == 0)%s\n",
                    static_cast<long long>(probes.live_key_bytes),
                    probes.live_key_bytes == 0 ? "  PASS" : "  MISS");

        // Filter-loop micro on identical data, outside the live window: a
        // bare Search's FilterItems is the loop + bucketing + index rebuild
        // without the view machinery.
        const FilterCatalog catalog = BuildFilterCatalog(*fixture.buyoutFixture.manager);
        Search bare(*fixture.buyoutFixture.manager, "micro", catalog);
        t0 = clock.nsecsElapsed();
        bare.FilterItems(all_items);
        std::printf("  [micro] bare FilterItems (no active filter): %.3f ms\n",
                    toMs(clock.nsecsElapsed() - t0));
    }

    // --- Row 2: single-bucket expand, cold keys ---------------------------
    {
        // The largest buckets are the honest worst case (quad tabs, up to
        // 576 items). Each rep refilters first so the expanded bucket's
        // keys are cold, then times the expand alone.
        std::vector<qint64> samples;
        int expanded_items = 0;
        for (int rep = 0; rep < 5; ++rep) {
            // Collapse everything first: a still-expanded bucket would be
            // saved and restored by the refilter (sorting it there), and
            // the timed expand() below would then be a signal-less no-op.
            for (int row = 0; row < tree->model()->rowCount(); ++row) {
                tree->collapse(tree->model()->index(row, 0));
            }
            fixture.window->OnSearchFormChange();
            drainEvents();
            auto *model = tree->model();
            int best_row = 0;
            int best_count = -1;
            for (int row = 0; row < model->rowCount(); ++row) {
                const int count = model->rowCount(model->index(row, 0));
                if (count > best_count) {
                    best_count = count;
                    best_row = row;
                }
            }
            expanded_items = best_count;
            const QModelIndex bucket = model->index(best_row, 0);
            t0 = clock.nsecsElapsed();
            tree->expand(bucket);
            samples.push_back(clock.nsecsElapsed() - t0);
            drainEvents();
        }
        std::printf("  [shape] expanded bucket item count: %d\n", expanded_items);
        rows.push_back({"single-bucket expand, cold keys (median of 5)",
                        toMs(median(samples)),
                        budget_expand_ms});
    }

    // --- Row 3: broad-filter default-expanded refilter (R1-8) -------------
    {
        ilvl_min->setText("2"); // matches ~85/86 of items; excludes some -> m_filtered
        // The ≤ 300 MB worst-shape memory row names two candidate shapes
        // (By-Item, or broad-filter fully-expanded By-Tab — R2-3); the
        // By-Item one is measured at process level below, so this shape
        // gets the same treatment: footprint delta across the FIRST entry
        // into the shape (later reps rebuild keys through the allocator's
        // recycled pages and would double-count the high-water mark).
        const std::int64_t broad_footprint_before = processFootprintBytes();
        std::vector<qint64> samples;
        std::int64_t broad_footprint_after = -1;
        for (int rep = 0; rep < 3; ++rep) {
            t0 = clock.nsecsElapsed();
            fixture.window->OnSearchFormChange();
            samples.push_back(clock.nsecsElapsed() - t0);
            if (rep == 0) {
                broad_footprint_after = processFootprintBytes();
            }
            drainEvents();
        }
        rows.push_back({"broad-filter default-expanded refilter (median)",
                        toMs(median(samples)),
                        budget_broad_ms});
        if ((broad_footprint_before >= 0) && (broad_footprint_after >= 0)) {
            const double delta_mb = static_cast<double>(broad_footprint_after
                                                        - broad_footprint_before)
                                    / (1024.0 * 1024.0);
            const bool pass = !is_1m || (delta_mb <= 300.0);
            memory_missed = memory_missed || !pass;
            std::printf("  [memory] broad-filter resident keys: gauge %.1f MB; process "
                        "footprint delta %.1f MB (budget 300 MB aggregate at 1m)%s\n",
                        static_cast<double>(probes.live_key_bytes) / (1024.0 * 1024.0),
                        delta_mb,
                        is_1m ? (pass ? "  PASS" : "  MISS") : "  (informational)");
        } else {
            // The 1m memory budget binds here; without the footprint API
            // the required measurement is unavailable — that fails the
            // gate rather than silently degrading to the gauge estimate.
            memory_missed = memory_missed || is_1m;
            std::printf("  [memory] broad-filter resident key bytes: %lld gauge only — no "
                        "process footprint API on this platform%s\n",
                        static_cast<long long>(probes.live_key_bytes),
                        is_1m ? "  MISS (required measurement unavailable)" : "");
        }

        // Attribution run + micros on identical data.
        probes.reset();
        probes.enabled = true;
        fixture.window->OnSearchFormChange();
        probes.enabled = false;
        std::printf("  [attribution] broad refilter: bucket_sorts=%lld key_builds=%lld "
                    "keyed_compares=%lld\n",
                    static_cast<long long>(probes.bucket_sorts),
                    static_cast<long long>(probes.key_builds),
                    static_cast<long long>(probes.keyed_compares));
        drainEvents();

        const FilterCatalog catalog = BuildFilterCatalog(*fixture.buyoutFixture.manager);
        Search bare(*fixture.buyoutFixture.manager, "micro", catalog);
        for (qsizetype n = 0; n < catalog.size(); ++n) {
            if (catalog[n].caption == "ilvl") {
                bare.setFilterState(n, MinMaxState{2.0, std::nullopt});
                break;
            }
        }
        t0 = clock.nsecsElapsed();
        bare.FilterItems(all_items);
        const qint64 micro_filter = clock.nsecsElapsed() - t0;

        Bucket flat{ItemLocation()};
        flat.AddItems(bare.items());
        NameColumn name_column;
        t0 = clock.nsecsElapsed();
        flat.Sort(name_column, Qt::DescendingOrder);
        const qint64 micro_sort = clock.nsecsElapsed() - t0;
        std::printf("  [micro] broad FilterItems: %.3f ms; key build + sort of the visible "
                    "result (one flat bucket): %.3f ms\n",
                    toMs(micro_filter),
                    toMs(micro_sort));

        ilvl_min->clear();
        fixture.window->OnSearchFormChange();
        drainEvents();
    }

    // --- Row 4: background resident-key memory (exactly 0) ----------------
    {
        // Materialize something in Search 1, then create/switch to Search 2:
        // deactivation must evict every resident vector (R2-4).
        auto *model = tree->model();
        tree->expand(model->index(0, 0));
        const std::int64_t while_active = probes.live_key_bytes;
        tabs->setCurrentIndex(1);
        drainEvents();
        memory_missed = memory_missed || (probes.live_key_bytes != 0);
        std::printf("  [memory] background resident key bytes: %lld after deactivation "
                    "(was %lld active) (row: == 0)%s\n",
                    static_cast<long long>(probes.live_key_bytes),
                    static_cast<long long>(while_active),
                    probes.live_key_bytes == 0 ? "  PASS" : "  MISS");
    }

    // --- Row 5 (S4): delta application, By-Tab visible bucket -------------
    // The child source and donor reply are shared with the S5 By-Item
    // merge rows below.
    ItemLocation child_source;
    Items child_arrivals;
    poe::StashTab donor_reply;
    {
        // Back on Search 1, clean state: the delta lands on the current
        // search's visible bucket.
        tabs->setCurrentIndex(0);
        fixture.window->OnSearchFormChange();
        drainEvents();
        auto *model = tree->model();

        // The largest tab is the honest bucket shape (a 576-item quad); a
        // second donor tab of similar size supplies a synthetic CHILD
        // fetch source drawn from the same name/base pools, so the bucket
        // aggregates two sources and the child's replacement is the R2-2
        // shape: removal runs scattered through retained sibling-source
        // rows, then a merge whose arrivals interleave against them.
        std::map<LocationInventory::Key, int> counts;
        for (const auto &item : all_items) {
            ++counts[LocationInventory::KeyFor(item->location())];
        }
        int best_tab = 0;
        int donor_tab = 0;
        int best_tab_count = -1;
        int donor_count = -1;
        for (int t = 0; t < dataset.tabCount(); ++t) {
            const int count = counts[LocationInventory::KeyFor(dataset.location(t))];
            if (count > best_tab_count) {
                donor_tab = best_tab;
                donor_count = best_tab_count;
                best_tab = t;
                best_tab_count = count;
            } else if (count > donor_count) {
                donor_tab = t;
                donor_count = count;
            }
        }
        const ItemLocation target = dataset.location(best_tab);
        const auto target_key = LocationInventory::KeyFor(target);
        tabs->setCurrentIndex(0);
        for (int row = 0; row < model->rowCount(); ++row) {
            if (model->index(row, 0).data().toString().startsWith(target.GetHeader())) {
                tree->expand(model->index(row, 0));
                break;
            }
        }
        drainEvents();

        // Informational: the simple shape — a full replacement of the
        // tab's own single source (empty retained vector, one removal
        // run, arrivals merged against nothing).
        Items own_replacement;
        own_replacement.reserve(static_cast<size_t>(best_tab_count));
        for (const auto &item : all_items) {
            if (LocationInventory::KeyFor(item->location()) == target_key) {
                own_replacement.push_back(item);
            }
        }
        {
            std::vector<qint64> samples;
            for (int rep = 0; rep < 5; ++rep) {
                t0 = clock.nsecsElapsed();
                fixture.window->OnTabRefreshed(target, own_replacement);
                samples.push_back(clock.nsecsElapsed() - t0);
                drainEvents();
            }
            rows.push_back({"  single-source full replacement", toMs(median(samples)), -1.0});
        }

        // The budgeted shape: the child source, seeded once (warmup),
        // then replaced per rep. Fresh ids keep the id index on its
        // unique fast path, as real arrivals would.
        child_source = target;
        child_source.setFetchId("bench-child-0001");
        donor_reply = dataset.MakeStashReply(donor_tab);
        const poe::StashTab &donor = donor_reply;
        Items &arrivals = child_arrivals;
        if (donor.items) {
            arrivals.reserve(donor.items->size());
            int serial = 0;
            for (poe::Item poe_item : *donor.items) {
                poe_item.id = QString("benchchild%1").arg(serial++, 16, 16, QChar('0'));
                arrivals.push_back(std::make_shared<Item>(poe_item, child_source));
            }
        }
        fixture.window->OnTabRefreshed(child_source, arrivals); // warmup: first insertion
        drainEvents();

        std::printf("  [shape] bucket: %d retained parent rows + %zu child arrivals per delta\n",
                    best_tab_count,
                    arrivals.size());
        std::vector<qint64> samples;
        for (int rep = 0; rep < 5; ++rep) {
            t0 = clock.nsecsElapsed();
            fixture.window->OnTabRefreshed(child_source, arrivals);
            samples.push_back(clock.nsecsElapsed() - t0);
            drainEvents();
        }
        rows.push_back({"S4 delta application (interleaved child merge)",
                        toMs(median(samples)),
                        budget_delta_ms});

        probes.reset();
        probes.enabled = true;
        fixture.window->OnTabRefreshed(child_source, arrivals);
        probes.enabled = false;
        std::printf("  [attribution] delta application: bucket_sorts=%lld key_builds=%lld "
                    "keyed_compares=%lld index_rebuilds=%lld refilters=%lld model_resets=%lld\n",
                    static_cast<long long>(probes.bucket_sorts),
                    static_cast<long long>(probes.key_builds),
                    static_cast<long long>(probes.keyed_compares),
                    static_cast<long long>(probes.index_rebuilds),
                    static_cast<long long>(probes.refilters),
                    static_cast<long long>(probes.model_resets));
        drainEvents();
    }

    // --- S5 rows: By-Item (D4 merge, R3-1 eager activation, memory) -------
    {
        const double budget_byitem_refilter_ms = is_100k ? 250.0 : is_1m ? 1500.0 : -1.0;
        const double budget_byitem_react_ms = is_100k ? 100.0 : is_1m ? 500.0 : -1.0;
        // The merge budget is stated at 1m only; 100k is informational.
        const double budget_byitem_merge_ms = is_1m ? 50.0 : -1.0;

        auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
        if (!viewCombo) {
            std::fprintf(stderr, "viewComboBox not found\n");
            return 1;
        }

        // Enter By-Item on the current search. Informational: the mode
        // switch itself (flat rebuild + keyed flat sort at the D6
        // boundary).
        const std::int64_t footprint_before = processFootprintBytes();
        t0 = clock.nsecsElapsed();
        viewCombo->setCurrentIndex(1);
        emit viewCombo->activated(1);
        const qint64 switch_ns = clock.nsecsElapsed() - t0;
        drainEvents();
        rows.push_back({"  mode switch into By-Item (build + sort)", toMs(switch_ns), -1.0});

        // Row: By-Item full refilter (user-initiated: filter loop + keyed
        // build + flat sort, reset and restore included).
        {
            std::vector<qint64> samples;
            for (int rep = 0; rep < 3; ++rep) {
                t0 = clock.nsecsElapsed();
                fixture.window->OnSearchFormChange();
                samples.push_back(clock.nsecsElapsed() - t0);
                drainEvents();
            }
            rows.push_back({"By-Item full refilter (median of 3)",
                            toMs(median(samples)),
                            budget_byitem_refilter_ms});
        }

        // Memory row: the worst materialized shape (the whole collection
        // resident in the flat bucket's key vector). The gauge is the
        // estimate; the ≤ 300 MB aggregate budget is judged at process
        // level (footprint delta across entering the shape).
        const std::int64_t footprint_after = processFootprintBytes();
        const double gauge_mb = static_cast<double>(probes.live_key_bytes) / (1024.0 * 1024.0);
        if ((footprint_before >= 0) && (footprint_after >= 0)) {
            const double delta_mb = static_cast<double>(footprint_after - footprint_before)
                                    / (1024.0 * 1024.0);
            const bool pass = !is_1m || (delta_mb <= 300.0);
            memory_missed = memory_missed || !pass;
            std::printf("  [memory] By-Item resident keys: gauge %.1f MB; process footprint "
                        "delta %.1f MB (budget 300 MB aggregate at 1m)%s\n",
                        gauge_mb,
                        delta_mb,
                        is_1m ? (pass ? "  PASS" : "  MISS") : "  (informational)");
        } else {
            memory_missed = memory_missed || is_1m;
            std::printf("  [memory] By-Item resident keys: gauge %.1f MB — no process "
                        "footprint API on this platform%s\n",
                        gauge_mb,
                        is_1m ? "  MISS (required measurement unavailable)" : "");
        }

        // Row: clean By-Item reactivation — deactivation evicts the flat
        // keys (R2-4); reactivation decides dirtiness first and hydrates
        // eagerly (R3-1). Timed end-to-end through the tab switch, the
        // user action that pays it.
        {
            tabs->setCurrentIndex(1);
            drainEvents();
            probes.reset();
            probes.enabled = true;
            t0 = clock.nsecsElapsed();
            tabs->setCurrentIndex(0);
            const qint64 react_ns = clock.nsecsElapsed() - t0;
            probes.enabled = false;
            drainEvents();
            rows.push_back({"clean By-Item reactivation (eager hydration)",
                            toMs(react_ns),
                            budget_byitem_react_ms});
            std::printf("  [attribution] reactivation: refilters=%lld key_builds=%lld "
                        "model_resets=%lld (clean: no refilter, one eager build)\n",
                        static_cast<long long>(probes.refilters),
                        static_cast<long long>(probes.key_builds),
                        static_cast<long long>(probes.model_resets));
        }

        // Row: the D4 flat-bucket merge — the same interleaved child
        // source as the S4 row, replaced per rep while By-Item is active:
        // erase runs scatter through the whole resident order and the 576
        // arrivals merge against the collection-sized retained vector.
        {
            fixture.window->OnTabRefreshed(child_source, child_arrivals); // warmup
            drainEvents();
            std::vector<qint64> samples;
            for (int rep = 0; rep < 5; ++rep) {
                t0 = clock.nsecsElapsed();
                fixture.window->OnTabRefreshed(child_source, child_arrivals);
                samples.push_back(clock.nsecsElapsed() - t0);
                drainEvents();
            }
            rows.push_back({"S5 By-Item merge (child-source replacement)",
                            toMs(median(samples)),
                            budget_byitem_merge_ms});

            probes.reset();
            probes.enabled = true;
            fixture.window->OnTabRefreshed(child_source, child_arrivals);
            probes.enabled = false;
            std::printf("  [attribution] By-Item merge: bucket_sorts=%lld key_builds=%lld "
                        "keyed_compares=%lld index_rebuilds=%lld refilters=%lld "
                        "model_resets=%lld\n",
                        static_cast<long long>(probes.bucket_sorts),
                        static_cast<long long>(probes.key_builds),
                        static_cast<long long>(probes.keyed_compares),
                        static_cast<long long>(probes.index_rebuilds),
                        static_cast<long long>(probes.refilters),
                        static_cast<long long>(probes.model_resets));
            drainEvents();

            // Attribution micro on identical data, outside the timed
            // window: the same flat delta against a bare Search whose
            // model has NO view attached — splits the bucket-vector
            // shuffle (per-run erase/insert on the collection-sized item
            // and key vectors) from the view-side per-batch row splice
            // (QTreeView's flat visible-row list pays its own memmove per
            // begin/end pair).
            {
                const FilterCatalog catalog = BuildFilterCatalog(*fixture.buyoutFixture.manager);
                Search bare(*fixture.buyoutFixture.manager, "micro", catalog);
                Items with_child = all_items;
                with_child.insert(with_child.end(), child_arrivals.begin(), child_arrivals.end());
                bare.FilterItems(with_child);
                // SetViewMode sorts the flat bucket and leaves its keys
                // resident (R3-1) — the same pre-delta state as the live
                // window's.
                bare.SetViewMode(Search::ViewMode::ByItem);
                t0 = clock.nsecsElapsed();
                bare.ApplyTabDelta(child_source, child_arrivals);
                std::printf("  [micro] bare flat delta, no view attached: %.3f ms "
                            "(remainder of the live row is view-side batch handling)\n",
                            toMs(clock.nsecsElapsed() - t0));
            }
        }

        // The production path (S5 review, P2): a delta reaches the window
        // through ItemsManager::OnTabRefreshed — source-keyed replacement,
        // inventory ingest, and the SCOPED PRICING PASS, which emits one
        // BuyoutsChanged batch when any buyout state changed. In By-Item
        // the batch's repaint scans the whole flat bucket, and with
        // Price/Date active the affected key entries rebuild and the flat
        // bucket fully re-sorts BEFORE the merge — so the window-direct
        // merge row above is a lower bound for pricing-carrying deltas.
        // Three end-to-end shapes; each priced rep carries a fresh price
        // so its 576 Sets are real state changes (Set no-ops otherwise).
        {
            auto pricedArrivals = [&](int rep) {
                Items out;
                if (donor_reply.items) {
                    out.reserve(donor_reply.items->size());
                    int serial = 0;
                    for (poe::Item poe_item : *donor_reply.items) {
                        poe_item.id = QString("benchchild%1").arg(serial++, 16, 16, QChar('0'));
                        poe_item.note = QString("~b/o %1 chaos").arg(rep + 1);
                        out.push_back(std::make_shared<Item>(poe_item, child_source));
                    }
                }
                return out;
            };

            // Shape 1: unpriced arrivals, Name active — the pricing pass
            // records nothing and emits no batch.
            {
                fixture.itemsManager->OnTabRefreshed(child_source, child_arrivals); // warmup
                drainEvents();
                std::vector<qint64> samples;
                for (int rep = 0; rep < 5; ++rep) {
                    t0 = clock.nsecsElapsed();
                    fixture.itemsManager->OnTabRefreshed(child_source, child_arrivals);
                    samples.push_back(clock.nsecsElapsed() - t0);
                    drainEvents();
                }
                rows.push_back(
                    {"  merge, manager path (unpriced, Name)", toMs(median(samples)), -1.0});
            }

            // Shape 2: unpriced arrivals, Name active, with the view's
            // flat row list materialized first (indexAt forces the lazy
            // layout) — the ON-SCREEN shape: every row-op batch splices
            // the view's own collection-sized viewItems list, work the
            // offscreen rows above never pay.
            {
                fixture.itemsManager->OnTabRefreshed(child_source, child_arrivals); // warmup
                drainEvents();
                std::vector<qint64> samples;
                for (int rep = 0; rep < 5; ++rep) {
                    tree->indexAt(QPoint(0, 0)); // materialize viewItems (untimed)
                    t0 = clock.nsecsElapsed();
                    fixture.itemsManager->OnTabRefreshed(child_source, child_arrivals);
                    samples.push_back(clock.nsecsElapsed() - t0);
                    drainEvents();
                }
                rows.push_back({"  merge, manager path (unpriced, Name, laid out)",
                                toMs(median(samples)),
                                -1.0});
            }

            // Shape 3: priced arrivals, Name active — one batch, the
            // By-Item repaint scans the flat bucket, nothing reorders.
            {
                std::vector<Items> reps;
                for (int rep = 0; rep < 5; ++rep) {
                    reps.push_back(pricedArrivals(rep));
                }
                std::vector<qint64> samples;
                for (int rep = 0; rep < 5; ++rep) {
                    t0 = clock.nsecsElapsed();
                    fixture.itemsManager->OnTabRefreshed(child_source, reps[rep]);
                    samples.push_back(clock.nsecsElapsed() - t0);
                    drainEvents();
                }
                rows.push_back(
                    {"  merge, manager path (priced, Name)", toMs(median(samples)), -1.0});
            }

            // Shape 4: priced arrivals, Price active — the batch rebuilds
            // the affected resident key entries and fully re-sorts the
            // flat bucket (R3-2), then the merge applies.
            {
                tree->header()->setSortIndicator(1, Qt::AscendingOrder); // Price; re-keys + sorts
                drainEvents();
                std::vector<Items> reps;
                for (int rep = 0; rep < 5; ++rep) {
                    reps.push_back(pricedArrivals(100 + rep));
                }
                std::vector<qint64> samples;
                for (int rep = 0; rep < 5; ++rep) {
                    t0 = clock.nsecsElapsed();
                    fixture.itemsManager->OnTabRefreshed(child_source, reps[rep]);
                    samples.push_back(clock.nsecsElapsed() - t0);
                    drainEvents();
                }
                rows.push_back(
                    {"  merge, manager path (priced, Price)", toMs(median(samples)), -1.0});

                probes.reset();
                probes.enabled = true;
                fixture.itemsManager->OnTabRefreshed(child_source, pricedArrivals(200));
                probes.enabled = false;
                std::printf("  [attribution] priced Price-active delta: model_updates=%lld "
                            "bucket_sorts=%lld key_builds=%lld keyed_compares=%lld "
                            "model_resets=%lld\n",
                            static_cast<long long>(probes.model_updates),
                            static_cast<long long>(probes.bucket_sorts),
                            static_cast<long long>(probes.key_builds),
                            static_cast<long long>(probes.keyed_compares),
                            static_cast<long long>(probes.model_resets));
                drainEvents();
                tree->header()->setSortIndicator(0, Qt::DescendingOrder); // restore Name
                drainEvents();
            }
        }

        // A′ gates (S5 remedy): the shown, laid-out spot check — the
        // unshown rows above never materialize the view's flat row list,
        // so they cannot adjudicate per-batch view overhead — and the
        // peak-footprint gate: the lifetime peak must not grow by a
        // key-vector-sized transient across the reps (the in-place key
        // rebuild). Run with QT_QPA_PLATFORM=cocoa for the fully
        // on-screen variant; the recorded environment stays offscreen.
        {
            fixture.window->resize(1200, 800);
            fixture.window->show();
            drainEvents();
            for (int n = 0; (n < 200) && (tree->verticalScrollBar()->maximum() == 0); ++n) {
                drainEvents();
            }
            struct rusage peak_before{};
            getrusage(RUSAGE_SELF, &peak_before);
            std::vector<qint64> samples;
            for (int rep = 0; rep < 5; ++rep) {
                t0 = clock.nsecsElapsed();
                fixture.window->OnTabRefreshed(child_source, child_arrivals);
                samples.push_back(clock.nsecsElapsed() - t0);
                drainEvents();
            }
            struct rusage peak_after{};
            getrusage(RUSAGE_SELF, &peak_after);
            rows.push_back({"S5 By-Item merge (window shown, laid out)",
                            toMs(median(samples)),
                            budget_byitem_merge_ms});
            std::printf("  [memory] lifetime peak (ru_maxrss, bytes on macOS) across shown-window "
                        "merge reps: +%.1f MB (gate: no key-vector-sized transient)\n",
                        static_cast<double>(peak_after.ru_maxrss - peak_before.ru_maxrss)
                            / (1024.0 * 1024.0));
            fixture.window->hide();
            drainEvents();
        }
    }

    // --- S6 rows (informational, S6 review round 1): the clean final
    // snapshot, both modes. No budget — the spec accepts O(collection)
    // once per refresh — but the path runs on EVERY refresh and its two
    // modes have different allocation profiles (By-Tab: state table +
    // per-bucket diff + order scan; By-Item: state table + flat A′
    // replace), so S7 records both. The window-direct call isolates the
    // reconciliation; the manager's snapshot pricing batch is measured
    // by the S5 manager-path rows. ru_maxrss deltas report any
    // collection-scale transient (the state table is the expected one).
    {
        auto *viewCombo = fixture.window->findChild<QComboBox *>("viewComboBox");
        if (!viewCombo) {
            std::fprintf(stderr, "viewComboBox not found\n");
            return 1;
        }
        const auto cleanSnapshotRow = [&](const char *name) {
            fixture.window->OnSearchFormChange(); // settle: model == published state
            drainEvents();
            struct rusage peak_before{};
            getrusage(RUSAGE_SELF, &peak_before);
            std::vector<qint64> samples;
            for (int rep = 0; rep < 5; ++rep) {
                t0 = clock.nsecsElapsed();
                fixture.window->OnItemsRefreshed(false);
                samples.push_back(clock.nsecsElapsed() - t0);
                drainEvents();
            }
            struct rusage peak_after{};
            getrusage(RUSAGE_SELF, &peak_after);
            rows.push_back({name, toMs(median(samples)), -1.0});
            probes.reset();
            probes.enabled = true;
            fixture.window->OnItemsRefreshed(false);
            probes.enabled = false;
            std::printf("  [attribution] %s: final_reconciliations=%lld refilters=%lld "
                        "model_resets=%lld bucket_sorts=%lld key_builds=%lld; lifetime peak "
                        "across reps +%.1f MB\n",
                        name,
                        static_cast<long long>(probes.final_reconciliations),
                        static_cast<long long>(probes.refilters),
                        static_cast<long long>(probes.model_resets),
                        static_cast<long long>(probes.bucket_sorts),
                        static_cast<long long>(probes.key_builds),
                        static_cast<double>(peak_after.ru_maxrss - peak_before.ru_maxrss)
                            / (1024.0 * 1024.0));
        };

        viewCombo->setCurrentIndex(0);
        emit viewCombo->activated(0);
        drainEvents();
        cleanSnapshotRow("S6 clean final snapshot, By-Tab (median of 5)");

        viewCombo->setCurrentIndex(1);
        emit viewCombo->activated(1);
        drainEvents();
        cleanSnapshotRow("S6 clean final snapshot, By-Item (median of 5)");
    }

    std::printf(
        "\n=== M3 hold-point result (S3-S7 rows): preset %s, %d tabs, %zu items, Qt %s ===\n",
        qPrintable(preset_name),
        dataset.tabCount(),
        all_items.size(),
        qVersion());
    const bool rows_missed = printRows(rows);
    const bool failed = rows_missed || memory_missed;
    std::printf("  S7 gate: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
