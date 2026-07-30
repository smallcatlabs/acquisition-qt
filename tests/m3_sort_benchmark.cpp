// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// SPIKE-ONLY (spike/m3-sort-profile, R3-4): the M3 sort-profiling harness.
// Never merged; the written result (docs/design/m3-sort-profile-result.md)
// is the deliverable. Run by hand, offscreen, in a Release build:
//
//   ./m3_sort_benchmark --preset 100k
//   ./m3_sort_benchmark --preset 1m
//
// Three measurement groups:
//
//  1. LIVE refilter split on the production path: MainWindow::OnItemsRefreshed
//     -> FilterItems (begin-reset / filter loop / bucket ordering / end-reset)
//     -> setSortingEnabled(true) -> ItemsModel::sort -> Search::Sort, with
//     per-section probes from src/util/spikeprofile.h and live comparator
//     invocation counts.
//
//  2. MICRO comparator attribution on copies of the published collection:
//     per-column bucket-sort cost and comparison counts, and per-call
//     component costs of the Column::lt comparator (value(), toString, the
//     two regexes, PrettyName, tuple build+compare).
//
//  3. LEVER bounds (prototype quality, no decisions): precomputed sort keys
//     (build once, cheap compare) and sorting only a handful of "expanded"
//     buckets, both on the same bucket shapes the live path sorts, plus the
//     By-Item single flat bucket both ways.

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QRegularExpression>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <tuple>
#include <vector>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "bucket.h"
#include "buyoutmanager.h"
#include "column.h"
#include "item.h"
#include "itemlocation.h"
#include "mainwindowfixture.h"
#include "spikedataset.h"
#include "util/spikeprofile.h"

namespace {

    double toMs(int64_t ns)
    {
        return static_cast<double>(ns) / 1e6;
    }

    int64_t median(std::vector<int64_t> values)
    {
        if (values.empty()) {
            return 0;
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    // Keep results alive so the optimizer cannot delete the measured work.
    volatile int64_t g_sink = 0;

    struct EventActivityFilter : QObject
    {
        bool active = false;
        bool eventFilter(QObject *, QEvent *) override
        {
            active = true;
            return false;
        }
    };

    [[nodiscard]] bool drainUntilIdle()
    {
        constexpr int kMaxPasses = 100000;
        EventActivityFilter filter;
        QCoreApplication::instance()->installEventFilter(&filter);
        bool idle = false;
        for (int pass = 0; pass < kMaxPasses; ++pass) {
            filter.active = false;
            QEventLoop loop;
            loop.processEvents(QEventLoop::AllEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (!filter.active) {
                idle = true;
                break;
            }
        }
        QCoreApplication::instance()->removeEventFilter(&filter);
        return idle;
    }

    // --- replicated Column::multivalue (kept in sync with src/column.cpp by
    // hand; spike-only). The real one is private, and the components below
    // need to be timed separately anyway.

    const QRegularExpression &sortDoubleMatch()
    {
        static const QRegularExpression re("^\\+?([\\d.]+)%?$");
        return re;
    }

    const QRegularExpression &sortTwoValues()
    {
        static const QRegularExpression re("^(\\d+)([-/])(\\d+)$");
        return re;
    }

    struct SortKey
    {
        double d1 = 0.0;
        QString s1;
        double d2 = 0.0;
        QString s2;
        // Item tie-break, matching Item::operator<'s effective order
        // (PrettyName, then uid; the hash term is self-comparing dead code —
        // see the findings register).
        QString pretty;
        QString uid;

        bool operator<(const SortKey &other) const
        {
            return std::tie(d1, s1, d2, s2, pretty, uid)
                   < std::tie(other.d1, other.s1, other.d2, other.s2, other.pretty, other.uid);
        }
    };

    SortKey makeKey(const Column &column, const Item &item)
    {
        SortKey key;
        const QString str = column.value(item).toString();
        QRegularExpressionMatch match;
        if (str.contains(sortDoubleMatch(), &match)) {
            key.d1 = match.captured(1).toDouble();
        } else if (str.contains(sortTwoValues(), &match)) {
            if (match.captured(2).startsWith("-")) {
                key.d1 = 0.5 * (match.captured(1).toDouble() + match.captured(3).toDouble());
            } else {
                key.s1 = item.PrettyName();
                key.d2 = match.captured(1).toDouble();
            }
        } else {
            key.s1 = str;
            key.s2 = item.PrettyName();
        }
        key.pretty = item.PrettyName();
        key.uid = item.id();
        return key;
    }

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);

    QCommandLineParser parser;
    const QCommandLineOption preset_option("preset",
                                           "Dataset preset: 100k or 1m.",
                                           "preset",
                                           "100k");
    const QCommandLineOption reps_option("reps", "Measured live refilters.", "reps", "");
    parser.addOption(preset_option);
    parser.addOption(reps_option);
    parser.process(app);

    SpikeDataset::Config config;
    int reps = 5;
    if (parser.value(preset_option) == "1m") {
        config.tab_count = 2600;
        config.mean_items_per_tab = 400;
        config.quad_share = 0.8;
        reps = 3;
    }
    if (!parser.value(reps_option).isEmpty()) {
        reps = parser.value(reps_option).toInt();
    }

    // Logging as in a production session (info); MainWindowFixture requires
    // the "main" hub logger.
    auto main_logger = std::make_shared<spdlog::logger>("main");
    main_logger->sinks().push_back(std::make_shared<spdlog::sinks::dist_sink_mt>());
    spdlog::register_logger(main_logger);
    spdlog::set_level(spdlog::level::info);

    std::printf("M3 sort profile: building dataset (tabs=%d mean=%d quad=%.2f seed=%u)...\n",
                config.tab_count,
                config.mean_items_per_tab,
                config.quad_share,
                config.seed);
    SpikeDataset dataset(config);
    const Items all_items = dataset.allItems();
    const std::vector<ItemLocation> tabs = dataset.locations();
    std::printf("  %d tabs, %lld items\n",
                dataset.tabCount(),
                static_cast<long long>(all_items.size()));

    MainWindowFixture fx;
    fx.window->SetDeltaThrottleInterval(3600 * 1000);
    fx.window->show();

    // --- populate: publish the collection through the production snapshot
    // path (buyout passes + the initial refilter/sort ride along, untimed).
    QElapsedTimer populate_timer;
    populate_timer.start();
    fx.itemsManager->OnItemsRefreshed(all_items, tabs, true);
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after populate\n");
        return 1;
    }
    std::printf("  populate (snapshot publish + initial refilter/sort): %.1f s\n",
                populate_timer.elapsed() / 1000.0);

    // =====================================================================
    // 1. LIVE refilter split (the production path, spike probes)
    // =====================================================================

    struct LiveSample
    {
        int64_t whole = 0;
        int64_t filter_total = 0;
        int64_t filter_begin_reset = 0;
        int64_t filter_loop = 0;
        int64_t filter_bucket_order = 0;
        int64_t filter_end_reset = 0;
        int64_t sort_enable = 0;
        int64_t model_sort_total = 0;
        int64_t sort_layout_about = 0;
        int64_t sort_snapshot = 0;
        int64_t sort_search_sort = 0;
        int64_t sort_remap = 0;
        int64_t sort_layout_changed = 0;
        int64_t restore_expansion = 0;
        int64_t restore_rest = 0;
        int64_t mvr_total = 0;
        uint64_t lt_base = 0;
        uint64_t item_lt = 0;
        uint64_t mv_string = 0;
        uint64_t mv_double = 0;
        uint64_t mv_range = 0;
        int buckets = 0;
        int64_t top5_bucket_ns = 0;
        int64_t top5_items = 0;
        int64_t bucket_items = 0;
    };
    std::vector<LiveSample> live;

    for (int rep = 0; rep < reps + 1; ++rep) {
        spike_profile::reset();
        const int64_t t0 = spike_profile::now_ns();
        fx.window->OnItemsRefreshed();
        const int64_t t1 = spike_profile::now_ns();
        if (!drainUntilIdle()) {
            std::fprintf(stderr, "event loop never settled after a live refilter\n");
            return 1;
        }
        if (rep == 0) {
            continue; // warmup
        }
        LiveSample s;
        s.whole = t1 - t0;
        s.filter_total = spike_profile::filter_total_ns;
        s.filter_begin_reset = spike_profile::filter_begin_reset_ns;
        s.filter_loop = spike_profile::filter_loop_ns;
        s.filter_bucket_order = spike_profile::filter_bucket_order_ns;
        s.filter_end_reset = spike_profile::filter_end_reset_ns;
        s.sort_enable = spike_profile::mvr_sort_enable_ns;
        s.model_sort_total = spike_profile::model_sort_total_ns;
        s.sort_layout_about = spike_profile::sort_layout_about_ns;
        s.sort_snapshot = spike_profile::sort_snapshot_ns;
        s.sort_search_sort = spike_profile::sort_search_sort_ns;
        s.sort_remap = spike_profile::sort_remap_ns;
        s.sort_layout_changed = spike_profile::sort_layout_changed_ns;
        s.restore_expansion = spike_profile::mvr_restore_expansion_ns;
        s.restore_rest = spike_profile::mvr_restore_rest_ns;
        s.mvr_total = spike_profile::mvr_total_ns;
        s.lt_base = spike_profile::lt_calls_base;
        s.item_lt = spike_profile::item_lt_calls;
        s.mv_string = spike_profile::mv_path_string;
        s.mv_double = spike_profile::mv_path_double;
        s.mv_range = spike_profile::mv_path_range;
        s.buckets = static_cast<int>(spike_profile::bucket_sort_samples.size());
        auto samples = spike_profile::bucket_sort_samples;
        for (const auto &[count, ns] : samples) {
            s.bucket_items += count;
        }
        std::sort(samples.begin(), samples.end(), [](const auto &a, const auto &b) {
            return a.first > b.first;
        });
        for (size_t i = 0; i < samples.size() && i < 5; ++i) {
            s.top5_items += samples[i].first;
            s.top5_bucket_ns += samples[i].second;
        }
        live.push_back(s);
    }

    const auto liveMedian = [&](int64_t LiveSample::*field) {
        std::vector<int64_t> values;
        for (const auto &s : live) {
            values.push_back(s.*field);
        }
        return median(values);
    };

    std::printf("\n=== 1. LIVE full-refilter split (%d samples, median) ===\n",
                static_cast<int>(live.size()));
    std::printf("  whole OnItemsRefreshed        %9.3f ms\n", toMs(liveMedian(&LiveSample::whole)));
    std::printf("  ModelViewRefresh total        %9.3f ms\n",
                toMs(liveMedian(&LiveSample::mvr_total)));
    std::printf("  FilterItems total             %9.3f ms\n",
                toMs(liveMedian(&LiveSample::filter_total)));
    std::printf("    begin reset                 %9.3f ms\n",
                toMs(liveMedian(&LiveSample::filter_begin_reset)));
    std::printf("    filter loop + bucketing     %9.3f ms\n",
                toMs(liveMedian(&LiveSample::filter_loop)));
    std::printf("    bucket display ordering     %9.3f ms\n",
                toMs(liveMedian(&LiveSample::filter_bucket_order)));
    std::printf("    end reset (view rebuild)    %9.3f ms\n",
                toMs(liveMedian(&LiveSample::filter_end_reset)));
    std::printf("  setSortingEnabled(true)       %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_enable)));
    std::printf("    ItemsModel::sort total      %9.3f ms\n",
                toMs(liveMedian(&LiveSample::model_sort_total)));
    std::printf("      layoutAboutToBeChanged    %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_layout_about)));
    std::printf("      persistent-idx snapshot   %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_snapshot)));
    std::printf("      Search::Sort (buckets)    %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_search_sort)));
    std::printf("      remap persistent idx      %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_remap)));
    std::printf("      layoutChanged emit        %9.3f ms\n",
                toMs(liveMedian(&LiveSample::sort_layout_changed)));
    std::printf("  RestoreViewExpansion          %9.3f ms\n",
                toMs(liveMedian(&LiveSample::restore_expansion)));
    std::printf("  Reselect + RestoreScroll      %9.3f ms\n",
                toMs(liveMedian(&LiveSample::restore_rest)));
    if (!live.empty()) {
        const LiveSample &s = live.front();
        std::printf("  comparator calls: base lt %llu (item tie-break %llu; %.1f%%)\n",
                    static_cast<unsigned long long>(s.lt_base),
                    static_cast<unsigned long long>(s.item_lt),
                    s.lt_base ? 100.0 * static_cast<double>(s.item_lt)
                                    / static_cast<double>(s.lt_base)
                              : 0.0);
        std::printf("  multivalue paths: string %llu, double %llu, range %llu\n",
                    static_cast<unsigned long long>(s.mv_string),
                    static_cast<unsigned long long>(s.mv_double),
                    static_cast<unsigned long long>(s.mv_range));
        std::printf("  buckets sorted: %d (%lld items); top-5 buckets: %lld items, %9.3f ms\n",
                    s.buckets,
                    static_cast<long long>(s.bucket_items),
                    static_cast<long long>(s.top5_items),
                    toMs(s.top5_bucket_ns));
    }

    // =====================================================================
    // 2. MICRO comparator attribution (copies of the published collection)
    // =====================================================================

    // Rebuild the live ByTab bucket shapes from the published flat vector,
    // grouped by tab id in first-seen order (the same grouping FilterItems
    // produces for an unfiltered search, modulo display order — irrelevant
    // to sort cost).
    const Items &published = fx.itemsManager->items();
    std::vector<std::pair<QString, Items>> tab_groups;
    std::map<QString, size_t> group_index;
    for (const auto &item : published) {
        const QString id = item->location().id();
        auto it = group_index.find(id);
        if (it == group_index.end()) {
            group_index.emplace(id, tab_groups.size());
            tab_groups.emplace_back(id, Items{});
            tab_groups.back().second.push_back(item);
        } else {
            tab_groups[it->second].second.push_back(item);
        }
    }
    std::printf("\n=== 2. MICRO comparator attribution (%zu tab buckets, %lld items) ===\n",
                tab_groups.size(),
                static_cast<long long>(published.size()));

    BuyoutManager &bo = *fx.buyoutFixture.manager;
    const auto sortAllBuckets = [&](const Column &column) -> std::pair<int64_t, uint64_t> {
        // Fresh copies each run; descending order, as the model's default.
        std::vector<Items> copies;
        copies.reserve(tab_groups.size());
        for (const auto &[id, items] : tab_groups) {
            copies.push_back(items);
        }
        spike_profile::reset();
        const int64_t t0 = spike_profile::now_ns();
        for (auto &items : copies) {
            std::sort(items.begin(),
                      items.end(),
                      [&](const std::shared_ptr<Item> &lhs, const std::shared_ptr<Item> &rhs) {
                          return column.lt(rhs.get(), lhs.get());
                      });
        }
        const int64_t elapsed = spike_profile::now_ns() - t0;
        g_sink += static_cast<int64_t>(copies.front().size());
        const uint64_t calls = spike_profile::lt_calls_base + spike_profile::lt_calls_price
                               + spike_profile::lt_calls_date;
        return {elapsed, calls};
    };

    struct NamedColumn
    {
        const char *label;
        std::unique_ptr<Column> column;
    };
    std::vector<NamedColumn> columns;
    columns.push_back({"Name (default sort)", std::make_unique<NameColumn>()});
    columns.push_back({"Price", std::make_unique<PriceColumn>(bo)});
    columns.push_back({"Date (Last Update)", std::make_unique<DateColumn>(bo)});
    columns.push_back({"Property Q (Quality)", std::make_unique<PropertyColumn>("Q", "Quality")});
    columns.push_back({"ilvl", std::make_unique<ItemlevelColumn>()});

    std::printf("  per-column bucket sort (all buckets, descending):\n");
    for (const auto &nc : columns) {
        const auto [ns, calls] = sortAllBuckets(*nc.column);
        std::printf("    %-22s %9.3f ms   %10llu compares   %7.1f ns/compare\n",
                    nc.label,
                    toMs(ns),
                    static_cast<unsigned long long>(calls),
                    calls ? static_cast<double>(ns) / static_cast<double>(calls) : 0.0);
    }

    // Component costs per call, on a fixed random pair sample.
    const size_t pair_count = std::min<size_t>(1000000, published.size() * 4);
    std::mt19937 pair_rng(20260730);
    std::uniform_int_distribution<size_t> pick(0, published.size() - 1);
    std::vector<std::pair<const Item *, const Item *>> pairs;
    pairs.reserve(pair_count);
    for (size_t n = 0; n < pair_count; ++n) {
        pairs.emplace_back(published[pick(pair_rng)].get(), published[pick(pair_rng)].get());
    }
    const NameColumn name_column;

    const auto perCall = [&](const char *label, auto &&body) {
        const int64_t t0 = spike_profile::now_ns();
        for (const auto &[lhs, rhs] : pairs) {
            body(lhs, rhs);
        }
        const int64_t elapsed = spike_profile::now_ns() - t0;
        std::printf("    %-38s %7.1f ns/call  (%9.3f ms / %zu)\n",
                    label,
                    static_cast<double>(elapsed) / static_cast<double>(pairs.size()),
                    toMs(elapsed),
                    pairs.size());
    };

    std::printf("  comparator component costs (NameColumn, random pairs):\n");
    perCall("full Column::lt(a,b)", [&](const Item *a, const Item *b) {
        g_sink += name_column.lt(a, b) ? 1 : 0;
    });
    perCall("2x value() [QVariant of PrettyName]", [&](const Item *a, const Item *b) {
        g_sink += name_column.value(*a).isValid() ? 1 : 0;
        g_sink += name_column.value(*b).isValid() ? 1 : 0;
    });
    perCall("2x value().toString()", [&](const Item *a, const Item *b) {
        g_sink += name_column.value(*a).toString().size();
        g_sink += name_column.value(*b).toString().size();
    });
    perCall("2x toString + both regexes", [&](const Item *a, const Item *b) {
        for (const Item *item : {a, b}) {
            const QString str = name_column.value(*item).toString();
            QRegularExpressionMatch match;
            if (!str.contains(sortDoubleMatch(), &match)
                && !str.contains(sortTwoValues(), &match)) {
                g_sink += str.size();
            }
        }
    });
    perCall("2x PrettyName() alone", [&](const Item *a, const Item *b) {
        g_sink += a->PrettyName().size();
        g_sink += b->PrettyName().size();
    });
    perCall("2x replicated multivalue (key build)", [&](const Item *a, const Item *b) {
        g_sink += makeKey(name_column, *a).s1.size();
        g_sink += makeKey(name_column, *b).s1.size();
    });
    perCall("Item::operator< (tie-break) alone", [&](const Item *a, const Item *b) {
        g_sink += (*a < *b) ? 1 : 0;
    });

    // =====================================================================
    // 3. LEVER bounds (prototype quality; no decisions)
    // =====================================================================

    std::printf("\n=== 3. LEVER bounds ===\n");

    // Lever A: precomputed sort keys — build once per item, sort with cheap
    // tuple comparisons (QString relational compares remain; no QVariant,
    // regex, or allocation per comparison).
    {
        std::vector<std::vector<std::pair<SortKey, const Item *>>> keyed;
        keyed.reserve(tab_groups.size());
        const int64_t t0 = spike_profile::now_ns();
        for (const auto &[id, items] : tab_groups) {
            std::vector<std::pair<SortKey, const Item *>> bucket;
            bucket.reserve(items.size());
            for (const auto &item : items) {
                bucket.emplace_back(makeKey(name_column, *item), item.get());
            }
            keyed.push_back(std::move(bucket));
        }
        const int64_t build_ns = spike_profile::now_ns() - t0;
        uint64_t compares = 0;
        const int64_t t1 = spike_profile::now_ns();
        for (auto &bucket : keyed) {
            std::sort(bucket.begin(), bucket.end(), [&](const auto &lhs, const auto &rhs) {
                ++compares;
                return rhs.first < lhs.first; // descending
            });
        }
        const int64_t sort_ns = spike_profile::now_ns() - t1;
        g_sink += static_cast<int64_t>(keyed.front().size());
        std::printf("  A. precomputed keys (Name, all buckets):\n");
        std::printf("     key build (once per item)  %9.3f ms\n", toMs(build_ns));
        std::printf("     sort with cheap compare    %9.3f ms   %10llu compares   %7.1f "
                    "ns/compare\n",
                    toMs(sort_ns),
                    static_cast<unsigned long long>(compares),
                    compares ? static_cast<double>(sort_ns) / static_cast<double>(compares) : 0.0);
    }

    // Lever B: sort only "expanded" buckets — the K largest stand in for a
    // worst-case expanded set (collapsed buckets defer their sort).
    {
        std::vector<Items> by_size;
        by_size.reserve(tab_groups.size());
        for (const auto &[id, items] : tab_groups) {
            by_size.push_back(items);
        }
        std::sort(by_size.begin(), by_size.end(), [](const Items &a, const Items &b) {
            return a.size() > b.size();
        });
        for (const size_t k : {size_t{5}, size_t{20}}) {
            std::vector<Items> copies(by_size.begin(),
                                      by_size.begin()
                                          + std::min(k, by_size.size()));
            int64_t item_count = 0;
            for (const auto &items : copies) {
                item_count += static_cast<int64_t>(items.size());
            }
            const int64_t t0 = spike_profile::now_ns();
            for (auto &items : copies) {
                std::sort(items.begin(),
                          items.end(),
                          [&](const std::shared_ptr<Item> &lhs, const std::shared_ptr<Item> &rhs) {
                              return name_column.lt(rhs.get(), lhs.get());
                          });
            }
            const int64_t ns = spike_profile::now_ns() - t0;
            g_sink += static_cast<int64_t>(copies.front().size());
            std::printf("  B. sort only %2zu largest buckets (%lld items): %9.3f ms\n",
                        k,
                        static_cast<long long>(item_count),
                        toMs(ns));
        }
    }

    // The By-Item view: one flat bucket holding everything — the worst case
    // for both levers, sorted with the real comparator and with keys.
    {
        Items flat = published;
        const int64_t t0 = spike_profile::now_ns();
        std::sort(flat.begin(),
                  flat.end(),
                  [&](const std::shared_ptr<Item> &lhs, const std::shared_ptr<Item> &rhs) {
                      return name_column.lt(rhs.get(), lhs.get());
                  });
        const int64_t baseline_ns = spike_profile::now_ns() - t0;
        g_sink += static_cast<int64_t>(flat.size());

        std::vector<std::pair<SortKey, const Item *>> keyed;
        keyed.reserve(published.size());
        const int64_t t1 = spike_profile::now_ns();
        for (const auto &item : published) {
            keyed.emplace_back(makeKey(name_column, *item), item.get());
        }
        const int64_t build_ns = spike_profile::now_ns() - t1;
        const int64_t t2 = spike_profile::now_ns();
        std::sort(keyed.begin(), keyed.end(), [](const auto &lhs, const auto &rhs) {
            return rhs.first < lhs.first;
        });
        const int64_t sort_ns = spike_profile::now_ns() - t2;
        g_sink += static_cast<int64_t>(keyed.size());
        std::printf("  By-Item flat bucket (%lld items):\n",
                    static_cast<long long>(published.size()));
        std::printf("     real comparator            %9.3f ms\n", toMs(baseline_ns));
        std::printf("     precomputed keys           %9.3f ms build + %9.3f ms sort\n",
                    toMs(build_ns),
                    toMs(sort_ns));
    }

    std::printf("\ndone (sink %lld)\n", static_cast<long long>(g_sink));
    return 0;
}
