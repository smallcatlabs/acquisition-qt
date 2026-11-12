// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// M2-M2 measurement harness (items-pipeline M2, D3/R7-3; implementation
// sequence stage 4): measures the complete synchronous reply application on
// the REAL production reply path — worker handler → persistence → manager
// apply → scoped pricing → UI intersection/fan-out — on fixed recorded reply
// and removal shapes at the 100k and 1m scales, with per-component
// attribution. Not a test: run by hand, offscreen, in a Release build:
//
//   ./m2m2_benchmark --preset 100k
//   ./m2m2_benchmark --preset 1m
//
// Attribution method: the live path is segmented by timing probes connected
// around the production slots (direct connections run in connection order),
// and the replace/parse/pricing components are additionally measured as
// micro-benchmarks on identical data outside the timed windows. Live
// segments sum to the measured total minus an explicitly reported residual
// (event-loop dispatch, coroutine resume, and the fake's completion path).

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSettings>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "buyoutmanager.h"
#include "currencymanager.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "fakeapiclient.h"
#include "fetchsourcekey.h"
#include "imagecache.h"
#include "itemcategories.h"
#include "itemsmanager.h"
#include "itemsmanagerworker.h"
#include "poe/poeapiclient.h"
#include "ratelimit/ratelimiter.h"
#include "shop.h"
#include "sourcekeyeditems.h"
#include "spikedataset.h"
#include "testfixtures.h"
#include "ui/mainwindow.h"
#include "util/json_writers.h"
#include "util/networkmanager.h"
#include "workertestaccess.h"

namespace {

    constexpr const char *kRealm = "pc";
    constexpr const char *kLeague = "M2M2 League";
    constexpr const char *kAccount = "m2m2";

    // One clock for every probe; probes record nsecsElapsed() since start.
    QElapsedTimer g_clock;

    struct Probes
    {
        qint64 window_start = -1;
        qint64 persist_a = -1;
        qint64 persist_b = -1;
        qint64 delta_a = -1;
        qint64 delta_b = -1;
        qint64 ui_a = -1;
        qint64 ui_b = -1;
        qint64 reconcile_a = -1;
        qint64 reconcile_b = -1;
        qint64 ui_reconcile_a = -1;
        qint64 ui_reconcile_b = -1;
        qint64 window_end = -1;
    };

    // One measured reply's attribution, all in nanoseconds.
    struct Sample
    {
        qint64 whole = 0;             // resolve -> event loop quiescent
        qint64 pre = 0;               // resolve/dispatch until handler start
        qint64 persistence = 0;       // StashRepo::saveStash
        qint64 worker_replace = 0;    // worker erase + parse + append (live)
        qint64 manager_primary = 0;   // manager erase+append+pricing (live, minus UI)
        qint64 ui_primary = 0;        // MainWindow intersection/fan-out (primary delta)
        qint64 worker_between = 0;    // counters, status fan-out, child discovery, ghost erase
        qint64 manager_reconcile = 0; // manager expected-set erase (live, minus UI)
        qint64 ui_reconcile = 0;      // MainWindow ghost-intersection fan-out
        qint64 post = 0;              // LaunchContent (empty) + CheckUpdateFinished + drain-out
        qint64 residual = 0;          // whole minus the sum of the segments above
        // Micro-benchmarks on identical data, outside the live window. The
        // replace micros time the production bucket op (ReplaceSource) on
        // store mirrors — the source-keyed successor to the flat erase the
        // original measurement attributed (D3, post-M2-M2 remedy).
        qint64 micro_worker_replace = 0;
        qint64 micro_manager_replace = 0;
        qint64 micro_parse_append = 0;
        qint64 micro_pricing = 0;
        int reply_items = 0;
    };

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

    qint64 maxOf(const std::vector<qint64> &values)
    {
        qint64 result = 0;
        for (const qint64 v : values) {
            result = std::max(result, v);
        }
        return result;
    }

    void printBucket(const char *name, const std::vector<Sample> &samples, qint64 Sample::*field)
    {
        std::vector<qint64> values;
        values.reserve(samples.size());
        for (const auto &sample : samples) {
            values.push_back(sample.*field);
        }
        std::printf("  %-22s median %9.3f ms   max %9.3f ms\n",
                    name,
                    toMs(median(values)),
                    toMs(maxOf(values)));
    }

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    g_clock.start();

    QCommandLineParser parser;
    const QCommandLineOption preset_option("preset",
                                           "Dataset preset: smoke, 100k, or 1m.",
                                           "preset",
                                           "100k");
    const QCommandLineOption reps_option("reps", "Measured replacement replies.", "reps", "");
    parser.addOption(preset_option);
    parser.addOption(reps_option);
    parser.process(app);

    // Fixed recorded shapes (R7-3): the same (config, seed) always produces
    // the same collection and churn sequence. The named presets are the
    // recorded M2-M2/spike shapes (SpikeDataset::Config::Preset).
    const auto preset = SpikeDataset::Config::Preset(parser.value(preset_option));
    if (!preset) {
        std::fprintf(stderr, "unknown preset: %s\n", qPrintable(parser.value(preset_option)));
        return 1;
    }
    const SpikeDataset::Config config = *preset;
    int reps = 40;
    int removal_reps = 10;
    if (parser.value(preset_option) == "1m") {
        reps = 12;
        removal_reps = 6;
    }
    if (!parser.value(reps_option).isEmpty()) {
        reps = parser.value(reps_option).toInt();
    }

    // Logging as in a production session (info); the "main" logger is the
    // hub the UI sinks attach through.
    auto main_logger = std::make_shared<spdlog::logger>("main");
    main_logger->sinks().push_back(std::make_shared<spdlog::sinks::dist_sink_mt>());
    spdlog::register_logger(main_logger);
    spdlog::set_level(spdlog::level::info);

    InitItemClasses(R"json({"TestClass":{"name":"Weapons"}})json");
    InitItemBaseTypes(
        R"json({"Metadata/Items/TestSword":{"item_class":"TestClass","name":"Test Sword","release_state":"released"}})json");

    std::printf("M2-M2 harness: building dataset (tabs=%d mean=%d quad=%.2f seed=%u)...\n",
                config.tab_count,
                config.mean_items_per_tab,
                config.quad_share,
                config.seed);
    SpikeDataset dataset(config);
    std::printf("  %d tabs, %lld items\n",
                dataset.tabCount(),
                static_cast<long long>(dataset.totalItems()));

    // --- production objects, wired the way Application wires them --------

    BuyoutManagerFixture bm;
    QSettings settings(bm.tempDir.filePath("settings.ini"), QSettings::IniFormat);
    settings.setValue("account", kAccount);
    settings.setValue("realm", kRealm);
    settings.setValue("league", kLeague);
    settings.sync();

    NetworkManager network;
    RateLimiter limiter(network);
    FakePoeApiClient api(limiter);
    ItemsManager manager(settings, *bm.manager);
    CurrencyManager currency(settings, *bm.data, manager);
    const SessionIdentity identity = makeTestIdentity(kAccount, kRealm, kLeague);
    Shop shop(settings, identity, network, api, *bm.data, manager, *bm.manager);
    ImageCache image_cache(network, bm.tempDir.filePath("cache"));
    UserStore store(QDir(bm.tempDir.filePath("data")), kAccount);
    MainWindow window(settings,
                      identity,
                      network,
                      limiter,
                      manager,
                      *bm.manager,
                      *bm.repo,
                      store.stashes(),
                      store.characters(),
                      currency,
                      shop,
                      image_cache,
                      QDir(bm.tempDir.path()));
    // M3 (S4/S5) retired the D9 throttle: a delta now applies to the
    // current search's model synchronously inside this measured unit, so
    // the rows below include the model's bucket-scoped application — the
    // production path. (M2's runs excluded the then-asynchronous tick via
    // SetDeltaThrottleInterval, deleted with the timer.)

    ItemsManagerWorker worker(settings, identity, *bm.manager, api);
    WorkerTestAccess access(worker);

    Probes probes;
    Items last_delta;
    int refresh_count = 0;

    // Persistence lane, with probes around the production repo slot.
    QObject::connect(&worker, &ItemsManagerWorker::stashReceived, &worker, [&] {
        probes.persist_a = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker,
                     &ItemsManagerWorker::stashReceived,
                     &store.stashes(),
                     &StashRepo::saveStash);
    QObject::connect(&worker, &ItemsManagerWorker::stashReceived, &worker, [&] {
        probes.persist_b = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker,
                     &ItemsManagerWorker::stashListReceived,
                     &store.stashes(),
                     &StashRepo::saveStashList);
    QObject::connect(&worker,
                     &ItemsManagerWorker::stashListReplaced,
                     &store.stashes(),
                     &StashRepo::reconcileStashList);
    QObject::connect(&worker,
                     &ItemsManagerWorker::stashChildrenReplaced,
                     &store.stashes(),
                     &StashRepo::reconcileStashChildren);

    // Presentation lane: worker -> manager, with probes bracketing the
    // manager's application (which nests the UI consumption below).
    QObject::connect(&worker, &ItemsManagerWorker::TabRefreshed, &worker, [&] {
        probes.delta_a = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker,
                     &ItemsManagerWorker::TabRefreshed,
                     &manager,
                     &ItemsManager::OnTabRefreshed);
    QObject::connect(&worker, &ItemsManagerWorker::TabRefreshed, &worker, [&] {
        probes.delta_b = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker, &ItemsManagerWorker::ChildrenReconciled, &worker, [&] {
        probes.reconcile_a = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker,
                     &ItemsManagerWorker::ChildrenReconciled,
                     &manager,
                     &ItemsManager::OnChildrenReconciled);
    QObject::connect(&worker, &ItemsManagerWorker::ChildrenReconciled, &worker, [&] {
        probes.reconcile_b = g_clock.nsecsElapsed();
    });
    QObject::connect(&worker,
                     &ItemsManagerWorker::ItemsRefreshed,
                     &manager,
                     &ItemsManager::OnItemsRefreshed);
    QObject::connect(&worker,
                     &ItemsManagerWorker::StatusUpdate,
                     &manager,
                     &ItemsManager::OnStatusUpdate);

    // UI lane: manager -> window, probes bracketing the intersection and
    // fan-out work.
    QObject::connect(&manager,
                     &ItemsManager::TabRefreshed,
                     &manager,
                     [&](const ItemLocation &, const Items &items) {
                         probes.ui_a = g_clock.nsecsElapsed();
                         last_delta = items;
                     });
    QObject::connect(&manager, &ItemsManager::TabRefreshed, &window, &MainWindow::OnTabRefreshed);
    QObject::connect(&manager, &ItemsManager::TabRefreshed, &manager, [&] {
        probes.ui_b = g_clock.nsecsElapsed();
    });
    QObject::connect(&manager, &ItemsManager::ChildrenReconciled, &manager, [&] {
        probes.ui_reconcile_a = g_clock.nsecsElapsed();
    });
    QObject::connect(&manager,
                     &ItemsManager::ChildrenReconciled,
                     &window,
                     &MainWindow::OnChildrenReconciled);
    QObject::connect(&manager, &ItemsManager::ChildrenReconciled, &manager, [&] {
        probes.ui_reconcile_b = g_clock.nsecsElapsed();
    });
    QObject::connect(&manager,
                     &ItemsManager::ItemsRefreshed,
                     &window,
                     &MainWindow::OnItemsRefreshed);
    QObject::connect(&manager, &ItemsManager::ItemsRefreshed, &manager, [&] { ++refresh_count; });
    QObject::connect(&manager, &ItemsManager::StatusUpdate, &window, &MainWindow::OnStatusUpdate);

    // --- initial load (empty cache) --------------------------------------

    const std::jthread cache_load = StartCacheLoad(worker,
                                                   QDir(bm.tempDir.filePath("data")),
                                                   identity,
                                                   true);
    while (refresh_count < 1) {
        if (!drainUntilIdle()) {
            std::fprintf(stderr, "event loop never settled during the initial load\n");
            return 1;
        }
        QEventLoop loop;
        loop.processEvents(QEventLoop::WaitForMoreEvents, 50);
    }

    // --- update 1: populate through the real reply path -------------------

    std::vector<poe::StashTab> stash_list;
    stash_list.reserve(static_cast<size_t>(dataset.tabCount()));
    for (int t = 0; t < dataset.tabCount(); ++t) {
        stash_list.push_back(dataset.stashSpec(t));
    }

    QElapsedTimer populate_timer;
    populate_timer.start();
    worker.Update(TabSelection::All);
    api.resolveCharacterList(api.pendingCharacterList(kRealm), {});
    api.resolveStashList(api.pendingStashList(kRealm, kLeague), stash_list);
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after the stash list\n");
        return 1;
    }
    for (int t = 0; t < dataset.tabCount(); ++t) {
        poe::StashTab reply = dataset.MakeStashReply(t);
        QByteArray bytes = json::writeStash(reply);
        api.resolve(api.pendingStash(reply.id),
                    poe::StashPayload{.stash = std::move(reply), .bytes = std::move(bytes)});
        if ((t % 64) == 0 && !drainUntilIdle()) {
            std::fprintf(stderr, "event loop never settled during populate\n");
            return 1;
        }
    }
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after populate\n");
        return 1;
    }
    if (refresh_count != 2) {
        std::fprintf(stderr, "populate did not complete (refresh_count=%d)\n", refresh_count);
        return 1;
    }
    std::printf("  populate update: %.1f s, published %lld items\n",
                populate_timer.elapsed() / 1000.0,
                static_cast<long long>(manager.items().size()));

    // --- update 2: measured replies ---------------------------------------

    worker.Update(TabSelection::All);
    api.resolveCharacterList(api.pendingCharacterList(kRealm), {});
    api.resolveStashList(api.pendingStashList(kRealm, kLeague), stash_list);
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after update 2's list\n");
        return 1;
    }

    // Deterministic measured tabs: spread across the collection; the first
    // three are unmeasured warmup. Replacement shape: churn 0.3 (removals,
    // in-place modifications, arrivals). Removal shape: an emptied source.
    const int stride = std::max(1, dataset.tabCount() / (reps + removal_reps + 4));
    std::vector<int> measured_tabs;
    for (int k = 0; k < reps + removal_reps + 3; ++k) {
        measured_tabs.push_back((7 + k * stride) % dataset.tabCount());
    }

    std::vector<Sample> replacement;
    std::vector<Sample> removal;

    // Store mirrors for the replace micros: seeded from each side's
    // post-populate state and fed every measured reply's delta, so they
    // track the live stores tab-for-tab across the measured sequence.
    SourceKeyedItems worker_mirror;
    SourceKeyedItems manager_mirror;
    worker_mirror.ResetTo(access.items());
    manager_mirror.ResetTo(manager.items());

    for (size_t k = 0; k < measured_tabs.size(); ++k) {
        const int t = measured_tabs[k];
        const bool warmup = (k < 3);
        const bool removal_shape = (!warmup && (k >= static_cast<size_t>(reps) + 3));

        // Build the reply and its bytes OUTSIDE the timed window.
        poe::StashTab reply;
        if (removal_shape) {
            reply = dataset.stashSpec(t);
            reply.items = std::vector<poe::Item>{};
        } else {
            dataset.ChurnTab(t, 0.3);
            reply = dataset.MakeStashReply(t);
        }
        QByteArray bytes = json::writeStash(reply);
        const FetchSourceKey key{ItemLocationType::STASH, reply.id};

        Sample sample;
        sample.reply_items = reply.items ? static_cast<int>(reply.items->size()) : 0;

        // Micro-benchmarks on identical pre-delivery state: parse the delta
        // (untimed — the parse cost has its own micro below) and time the
        // production bucket op on each mirror.
        {
            Items micro_delta;
            if (reply.items) {
                const ItemLocation base_location = dataset.location(t);
                micro_delta.reserve(reply.items->size());
                for (const auto &poe_item : *reply.items) {
                    micro_delta.push_back(
                        std::make_shared<Item>(poe_item, base_location.getItemLocation(poe_item)));
                }
            }
            Items manager_delta = micro_delta;
            const qint64 t0 = g_clock.nsecsElapsed();
            worker_mirror.ReplaceSource(key, std::move(micro_delta));
            const qint64 t1 = g_clock.nsecsElapsed();
            manager_mirror.ReplaceSource(key, std::move(manager_delta));
            const qint64 t2 = g_clock.nsecsElapsed();
            if (!warmup) {
                sample.micro_worker_replace = t1 - t0;
                sample.micro_manager_replace = t2 - t1;
            }
        }

        // The measured window: resolve -> quiescent.
        probes = Probes{};
        probes.window_start = g_clock.nsecsElapsed();
        api.resolve(api.pendingStash(reply.id),
                    poe::StashPayload{.stash = reply, .bytes = std::move(bytes)});
        if (!drainUntilIdle()) {
            std::fprintf(stderr, "event loop never settled during a measured reply\n");
            return 1;
        }
        probes.window_end = g_clock.nsecsElapsed();

        if (warmup) {
            continue;
        }

        sample.whole = probes.window_end - probes.window_start;
        sample.pre = probes.persist_a - probes.window_start;
        sample.persistence = probes.persist_b - probes.persist_a;
        sample.worker_replace = probes.delta_a - probes.persist_b;
        sample.ui_primary = probes.ui_b - probes.ui_a;
        sample.manager_primary = (probes.delta_b - probes.delta_a) - sample.ui_primary;
        sample.worker_between = probes.reconcile_a - probes.delta_b;
        sample.ui_reconcile = probes.ui_reconcile_b - probes.ui_reconcile_a;
        sample.manager_reconcile = (probes.reconcile_b - probes.reconcile_a) - sample.ui_reconcile;
        sample.post = probes.window_end - probes.reconcile_b;
        sample.residual = sample.whole
                          - (sample.pre + sample.persistence + sample.worker_replace
                             + (probes.delta_b - probes.delta_a) + sample.worker_between
                             + (probes.reconcile_b - probes.reconcile_a) + sample.post);

        // Post-delivery micro-benchmarks on the delta that was applied.
        {
            const ItemLocation base_location = dataset.location(t);
            const auto &poe_items = reply.items ? *reply.items : std::vector<poe::Item>{};
            const qint64 t0 = g_clock.nsecsElapsed();
            Items parsed;
            parsed.reserve(poe_items.size());
            for (const auto &poe_item : poe_items) {
                const ItemLocation location = base_location.getItemLocation(poe_item);
                parsed.push_back(std::make_shared<Item>(poe_item, location));
            }
            sample.micro_parse_append = g_clock.nsecsElapsed() - t0;
        }
        {
            // Replicates ItemsManager::ApplyScopedPricing's three per-item
            // rules (D7) with the real BuyoutManager, on the applied delta.
            const qint64 t0 = g_clock.nsecsElapsed();
            for (const auto &item : last_delta) {
                const auto &note = item->note();
                if (!note.isEmpty()) {
                    const Buyout buyout = bm.manager->StringToBuyout(note);
                    if (buyout.IsActive() || bm.manager->Get(*item).IsGameSet()) {
                        bm.manager->Set(*item, buyout);
                    }
                }
                Buyout tab_bo = bm.manager->GetTab(item->location());
                if (bm.manager->Get(*item).IsInherited()) {
                    if (tab_bo.IsActive()) {
                        tab_bo.inherited = true;
                        tab_bo.last_update = QDateTime::currentDateTime();
                        bm.manager->Set(*item, tab_bo);
                    } else {
                        bm.manager->Set(*item, Buyout());
                    }
                }
                if (item->location().removeonly() == false) {
                    if (bm.manager->Get(*item).RequiresRefresh() || tab_bo.RequiresRefresh()) {
                        bm.manager->SetRefreshLocked(item->location());
                    }
                }
            }
            sample.micro_pricing = g_clock.nsecsElapsed() - t0;
        }

        (removal_shape ? removal : replacement).push_back(sample);
    }

    // Terminate update 2 without replaying the whole collection: fail one
    // remaining fetch (first-failure terminal; the published copy keeps the
    // applied deltas, D4/D6) and settle the abandoned siblings.
    for (int t = 0; t < dataset.tabCount(); ++t) {
        const QString id = dataset.stashSpec(t).id;
        if (api.hasPendingStash(id)) {
            api.reject(api.pendingStash(id), RateLimit::FetchError::Kind::Network);
            break;
        }
    }
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after the terminal failure\n");
        return 1;
    }
    api.settleStoppedStragglers();
    if (!drainUntilIdle()) {
        std::fprintf(stderr, "event loop never settled after settling stragglers\n");
        return 1;
    }

    // --- report ------------------------------------------------------------

    const auto report = [](const char *title, const std::vector<Sample> &samples) {
        if (samples.empty()) {
            return;
        }
        std::vector<qint64> items;
        for (const auto &sample : samples) {
            items.push_back(sample.reply_items);
        }
        std::printf("\n%s (%zu samples, median reply %lld items):\n",
                    title,
                    samples.size(),
                    static_cast<long long>(median(items)));
        printBucket("whole path", samples, &Sample::whole);
        printBucket("pre (dispatch)", samples, &Sample::pre);
        printBucket("persistence", samples, &Sample::persistence);
        printBucket("worker erase+parse", samples, &Sample::worker_replace);
        printBucket("manager apply", samples, &Sample::manager_primary);
        printBucket("ui primary", samples, &Sample::ui_primary);
        printBucket("worker between", samples, &Sample::worker_between);
        printBucket("manager reconcile", samples, &Sample::manager_reconcile);
        printBucket("ui reconcile", samples, &Sample::ui_reconcile);
        printBucket("post (finish)", samples, &Sample::post);
        printBucket("residual", samples, &Sample::residual);
        std::printf("  micro-benchmarks (identical data, outside the window):\n");
        printBucket("worker replace (micro)", samples, &Sample::micro_worker_replace);
        printBucket("manager replace (micro)", samples, &Sample::micro_manager_replace);
        printBucket("parse+append (micro)", samples, &Sample::micro_parse_append);
        printBucket("pricing (micro)", samples, &Sample::micro_pricing);
    };

    std::printf("\n=== M2-M2 result: preset %s, %d tabs, %lld published items, Qt %s ===\n",
                qPrintable(parser.value(preset_option)),
                dataset.tabCount(),
                static_cast<long long>(manager.items().size()),
                qVersion());
    report("Replacement replies (churn 0.3)", replacement);
    report("Removal replies (emptied source)", removal);

    return 0;
}
