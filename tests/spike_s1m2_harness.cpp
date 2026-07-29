// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// SPIKE(S1-M2): interactive harness for the D9 cadence spike. Builds the
// real MainWindow offline (same substrate as tst_mainwindow), loads a
// deterministic large collection, streams simulated per-tab deltas at a
// configurable cadence, and exposes the throttle period S, churn, and
// failure/final-snapshot controls beside a live reset-latency readout.
//
// Non-production code on a throwaway branch (R3-4/R7-3). Never merge.

#include <QApplication>
#include <QCheckBox>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <random>

#include <spdlog/logger.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "mainwindowfixture.h"
#include "spikedataset.h"

namespace {

    struct StreamStats
    {
        int deltas = 0;
        int resets = 0;
        qint64 last_total_ms = 0;
        qint64 last_filter_ms = 0;
        qint64 last_restore_ms = 0;
        qint64 max_total_ms = 0;
        qint64 sum_total_ms = 0;
    };

    class SpikeControlPanel : public QWidget
    {
    public:
        SpikeControlPanel(MainWindowFixture &fixture, SpikeDataset &dataset)
            : m_fixture(fixture)
            , m_dataset(dataset)
            , m_pick(0, dataset.tabCount() - 1)
            , m_rng(dataset.tabCount() * 7919u + 17u)
        {
            setWindowTitle("S1-M2 Spike Controls");

            auto *layout = new QVBoxLayout(this);

            auto *config_box = new QGroupBox("D9 knobs", this);
            auto *config = new QFormLayout(config_box);

            m_s_spin = new QSpinBox(this);
            m_s_spin->setRange(1, 600);
            m_s_spin->setSuffix(" s");
            m_s_spin->setValue(m_fixture.window->SpikeThrottleInterval() / 1000);
            connect(m_s_spin, &QSpinBox::valueChanged, this, [this](int seconds) {
                m_fixture.window->SpikeSetThrottleInterval(seconds * 1000);
            });
            config->addRow("Throttle period S:", m_s_spin);

            m_interval_spin = new QSpinBox(this);
            m_interval_spin->setRange(1, 120);
            m_interval_spin->setSuffix(" s");
            m_interval_spin->setValue(20);
            connect(m_interval_spin, &QSpinBox::valueChanged, this, [this](int seconds) {
                m_stream.setInterval(seconds * 1000);
            });
            config->addRow("Delta arrival interval:", m_interval_spin);

            m_churn_spin = new QSpinBox(this);
            m_churn_spin->setRange(0, 100);
            m_churn_spin->setSuffix(" %");
            m_churn_spin->setValue(30);
            config->addRow("Per-tab churn:", m_churn_spin);

            layout->addWidget(config_box);

            auto *actions_box = new QGroupBox("Stream", this);
            auto *actions = new QVBoxLayout(actions_box);

            m_start_button = new QPushButton("Start refresh stream", this);
            connect(m_start_button, &QPushButton::clicked, this, [this]() {
                if (m_stream.isActive()) {
                    m_stream.stop();
                    m_start_button->setText("Resume refresh stream");
                } else {
                    m_stream.setInterval(m_interval_spin->value() * 1000);
                    m_stream.start();
                    m_start_button->setText("Pause refresh stream");
                }
            });
            actions->addWidget(m_start_button);

            auto *inject = new QPushButton("Inject one delta now", this);
            connect(inject, &QPushButton::clicked, this, [this]() { ApplyOneDelta(); });
            actions->addWidget(inject);

            auto *rename = new QPushButton("Rename a random tab (delta)", this);
            connect(rename, &QPushButton::clicked, this, [this]() {
                const int tab = m_pick(m_rng);
                ++m_renames;
                m_dataset.RenameTab(tab,
                                    QString("Renamed %1 (v%2)").arg(tab).arg(m_renames));
                m_fixture.buyoutFixture.manager->SetStashTabLocations(m_dataset.locations());
                // The rename arrives like any other fetch: a delta whose
                // items carry the new tab metadata.
                m_fixture.itemsManager->SpikeApplyTabDelta(m_dataset.location(tab),
                                                           m_dataset.tabItems(tab));
                ++m_stats.deltas;
                spdlog::info("SPIKE(S1-M2): renamed tab {} -> '{}'",
                             tab,
                             m_dataset.tabName(tab));
                UpdateLabels();
            });
            actions->addWidget(rename);

            auto *fail = new QPushButton("Simulate terminal failure (stop, no final emit)", this);
            connect(fail, &QPushButton::clicked, this, [this]() {
                m_stream.stop();
                m_start_button->setText("Resume refresh stream");
                // R5-3: the pending tick survives and fires; only the stream
                // of new deltas ends.
                spdlog::info("SPIKE(S1-M2): terminal failure simulated; pending tick "
                             "left alive ({} ms remaining)",
                             m_fixture.window->SpikeThrottleRemainingMs());
                UpdateLabels();
            });
            actions->addWidget(fail);

            auto *final_emit = new QPushButton("Final snapshot (full ItemsRefreshed path)", this);
            connect(final_emit, &QPushButton::clicked, this, [this]() {
                QElapsedTimer timer;
                timer.start();
                m_fixture.itemsManager->OnItemsRefreshed(m_dataset.allItems(),
                                                         m_dataset.locations(),
                                                         false);
                spdlog::info("SPIKE(S1-M2): final snapshot applied in {} ms", timer.elapsed());
                UpdateLabels();
            });
            actions->addWidget(final_emit);

            layout->addWidget(actions_box);

            auto *stats_box = new QGroupBox("Latency + state", this);
            auto *stats = new QFormLayout(stats_box);
            m_collection_label = new QLabel(this);
            m_deltas_label = new QLabel(this);
            m_pending_label = new QLabel(this);
            m_resets_label = new QLabel(this);
            m_latency_label = new QLabel(this);
            stats->addRow("Collection:", m_collection_label);
            stats->addRow("Deltas applied:", m_deltas_label);
            stats->addRow("Pending tick:", m_pending_label);
            stats->addRow("Throttled resets:", m_resets_label);
            stats->addRow("Reset latency:", m_latency_label);
            layout->addWidget(stats_box);

            connect(&m_stream, &QTimer::timeout, this, [this]() { ApplyOneDelta(); });

            connect(m_fixture.window.get(),
                    &MainWindow::SpikeResetApplied,
                    this,
                    [this](const MainWindow::SpikeResetRecord &record) {
                        ++m_stats.resets;
                        m_stats.last_total_ms = record.total_ms;
                        m_stats.last_filter_ms = record.filter_ms;
                        m_stats.last_restore_ms = record.restore_ms;
                        m_stats.max_total_ms = std::max(m_stats.max_total_ms, record.total_ms);
                        m_stats.sum_total_ms += record.total_ms;
                        UpdateLabels();
                    });

            m_refresh_labels.setInterval(500);
            connect(&m_refresh_labels, &QTimer::timeout, this, [this]() { UpdateLabels(); });
            m_refresh_labels.start();

            UpdateLabels();
        }

    private:
        void ApplyOneDelta()
        {
            const int tab = m_pick(m_rng);
            const double churn = m_churn_spin->value() / 100.0;
            const Items delta = m_dataset.ChurnTab(tab, churn);
            m_fixture.itemsManager->SpikeApplyTabDelta(m_dataset.location(tab), delta);
            ++m_stats.deltas;
            UpdateLabels();
        }

        void UpdateLabels()
        {
            m_collection_label->setText(QString("%1 items in %2 tabs")
                                            .arg(m_dataset.totalItems())
                                            .arg(m_dataset.tabCount()));
            m_deltas_label->setText(QString::number(m_stats.deltas));
            if (m_fixture.window->SpikeThrottlePending()) {
                m_pending_label->setText(
                    QString("armed, fires in %1 s")
                        .arg(m_fixture.window->SpikeThrottleRemainingMs() / 1000.0, 0, 'f', 1));
            } else {
                m_pending_label->setText("idle");
            }
            m_resets_label->setText(QString::number(m_stats.resets));
            const qint64 avg = m_stats.resets ? (m_stats.sum_total_ms / m_stats.resets) : 0;
            m_latency_label->setText(QString("last %1 ms (filter %2 / restore %3), "
                                             "avg %4 ms, max %5 ms")
                                         .arg(m_stats.last_total_ms)
                                         .arg(m_stats.last_filter_ms)
                                         .arg(m_stats.last_restore_ms)
                                         .arg(avg)
                                         .arg(m_stats.max_total_ms));
        }

        MainWindowFixture &m_fixture;
        SpikeDataset &m_dataset;
        std::uniform_int_distribution<int> m_pick;
        std::mt19937 m_rng;
        QTimer m_stream;
        QTimer m_refresh_labels;
        StreamStats m_stats;
        int m_renames = 0;

        QSpinBox *m_s_spin = nullptr;
        QSpinBox *m_interval_spin = nullptr;
        QSpinBox *m_churn_spin = nullptr;
        QPushButton *m_start_button = nullptr;
        QLabel *m_collection_label = nullptr;
        QLabel *m_deltas_label = nullptr;
        QLabel *m_pending_label = nullptr;
        QLabel *m_resets_label = nullptr;
        QLabel *m_latency_label = nullptr;
    };

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("acquisition-spike-s1m2");

    QCommandLineParser parser;
    parser.setApplicationDescription("S1-M2 cadence spike harness (non-production)");
    parser.addHelpOption();
    const QCommandLineOption preset_option("preset",
                                           "Dataset preset: smoke, 100k, or 1m.",
                                           "preset",
                                           "100k");
    const QCommandLineOption tabs_option("tabs", "Tab count override.", "count");
    const QCommandLineOption mean_option("mean", "Mean items per tab override.", "count");
    const QCommandLineOption seed_option("seed", "Generator seed.", "seed");
    const QCommandLineOption auto_option("auto",
                                         "Unattended smoke run: stream deltas at 1 s with "
                                         "S = 5 s, then exit after 35 s.");
    parser.addOption(preset_option);
    parser.addOption(tabs_option);
    parser.addOption(mean_option);
    parser.addOption(seed_option);
    parser.addOption(auto_option);
    parser.process(app);

    SpikeDataset::Config config;
    const QString preset = parser.value(preset_option);
    if (preset == "smoke") {
        config.tab_count = 50;
        config.mean_items_per_tab = 20;
    } else if (preset == "1m") {
        config.tab_count = 2000;
        config.mean_items_per_tab = 500;
    } else {
        config.tab_count = 2000;
        config.mean_items_per_tab = 50;
    }
    if (parser.isSet(tabs_option)) {
        config.tab_count = parser.value(tabs_option).toInt();
    }
    if (parser.isSet(mean_option)) {
        config.mean_items_per_tab = parser.value(mean_option).toInt();
    }
    if (parser.isSet(seed_option)) {
        config.seed = parser.value(seed_option).toUInt();
    }

    // The fixture requires the "main" logger with a dist-sink hub (F42).
    auto main_logger = std::make_shared<spdlog::logger>("main");
    auto sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    sink_hub->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    main_logger->sinks().push_back(sink_hub);
    main_logger->set_level(spdlog::level::debug);
    spdlog::register_logger(main_logger);
    spdlog::set_default_logger(main_logger);

    spdlog::info("SPIKE(S1-M2): generating dataset ({} tabs, mean {} items/tab, seed {})",
                 config.tab_count,
                 config.mean_items_per_tab,
                 config.seed);
    QElapsedTimer timer;
    timer.start();
    SpikeDataset dataset(config);
    MainWindowFixture fixture;

    const Items all = dataset.allItems();
    spdlog::info("SPIKE(S1-M2): {} items materialized in {} ms", all.size(), timer.elapsed());

    timer.restart();
    fixture.itemsManager->OnItemsRefreshed(all, dataset.locations(), true);
    spdlog::info("SPIKE(S1-M2): initial refresh applied in {} ms", timer.elapsed());

    QObject::connect(fixture.itemsManager.get(),
                     &ItemsManager::TabRefreshed,
                     fixture.window.get(),
                     &MainWindow::OnTabRefreshed);

    fixture.window->show();

    SpikeControlPanel panel(fixture, dataset);
    panel.show();

    // Unattended verification of the full delta -> throttle -> reset ->
    // restore path (used by the offscreen smoke run; not for hand-driving).
    std::unique_ptr<QTimer> auto_stream;
    int auto_resets = 0;
    if (parser.isSet(auto_option)) {
        fixture.window->SpikeSetThrottleInterval(5'000);
        QObject::connect(fixture.window.get(),
                         &MainWindow::SpikeResetApplied,
                         [&auto_resets](const MainWindow::SpikeResetRecord &) { ++auto_resets; });
        auto_stream = std::make_unique<QTimer>();
        auto_stream->setInterval(1'000);
        std::mt19937 auto_rng(config.seed + 1);
        std::uniform_int_distribution<int> auto_pick(0, dataset.tabCount() - 1);
        QObject::connect(auto_stream.get(), &QTimer::timeout, [&, auto_pick, auto_rng]() mutable {
            const int tab = auto_pick(auto_rng);
            fixture.itemsManager->SpikeApplyTabDelta(dataset.location(tab),
                                                     dataset.ChurnTab(tab, 0.3));
        });
        auto_stream->start();
        QTimer::singleShot(35'000, &app, [&]() {
            spdlog::info("SPIKE(S1-M2): auto run finished with {} throttled resets", auto_resets);
            app.exit(auto_resets >= 2 ? 0 : 1);
        });
    }

    return app.exec();
}
