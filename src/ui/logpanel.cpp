// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/logpanel.h"

#include <algorithm>

#include <QFontDatabase>
#include <QObject>
#include <QPushButton>
#include <QStatusBar>
#include <QTextEdit>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/qt_sinks.h>

#include "ui/mainwindow.h"
#include "ui_mainwindow.h"

constexpr QColor ERROR_COLOR = QColor(255, 0, 0);
constexpr QColor WARNING_COLOR = QColor(174, 141, 28);

constexpr int MAX_LINES = 200;
constexpr bool DARK_COLORS = false;
constexpr bool IS_UTF8 = true;

LogPanel::LogPanel(MainWindow *window, Ui::MainWindow *ui)
    : QObject(window)
{
    // Create the UI widgets.
    m_status_button = new QPushButton(window);
    m_output = new QTextEdit(window);

    // Setup the output panel.
    m_output->hide();
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->insertPlainText("Errors and warnings will be printed here\n");
    m_output->setMaximumHeight(250); // TODO(xyz): remove magic numbers

    // Setup the status button.
    m_status_button->setFlat(false);
    window->statusBar()->addPermanentWidget(m_status_button);

    // Get the status label ready.
    UpdateStatusLabel();

    QObject::connect(m_status_button, &QPushButton::clicked, this, &LogPanel::TogglePanelVisibility);

    ui->mainLayout->addWidget(m_output);

    const auto logger = spdlog::get("main");
    if (!logger) {
        spdlog::warn("LogPanel: main logger is not registered; panel sinks are disabled");
        return;
    }

    // The panel's sinks attach through the dist-sink hub that logging::init
    // installed, never directly to the logger: the logger's sink vector is
    // not synchronized against logging threads, while the hub's
    // add_sink/remove_sink share the delivery mutex (F42).
    for (const auto &sink : logger->sinks()) {
        m_sink_hub = std::dynamic_pointer_cast<spdlog::sinks::dist_sink_mt>(sink);
        if (m_sink_hub) {
            break;
        }
    }
    if (!m_sink_hub) {
        spdlog::warn("LogPanel: the main logger has no sink hub; panel sinks are disabled");
        return;
    }

    // Create a sink for the text panel.
    m_panel_sink = std::make_shared<spdlog::sinks::qt_color_sink_mt>(m_output,
                                                                     MAX_LINES,
                                                                     DARK_COLORS,
                                                                     IS_UTF8);
    m_panel_sink->set_level(spdlog::level::warn);

    // Create a custom callback sink to change the status button label.
    m_callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [this](const spdlog::details::log_msg &msg) {
            if (msg.level >= spdlog::level::err) {
                m_num_errors.fetch_add(1, std::memory_order_relaxed);
            } else if (msg.level == spdlog::level::warn) {
                m_num_warnings.fetch_add(1, std::memory_order_relaxed);
            }
            // IMPORTANT: queue UI update onto LogPanel's thread (GUI thread)
            // Otherwise calls to spdlog from other threads (e.g. ItemsWorkerManager)
            // will cause exceptions.
            QMetaObject::invokeMethod(this, [this] { UpdateStatusLabel(); }, Qt::QueuedConnection);
        });

    m_callback_sink->set_level(spdlog::level::warn);

    m_sink_hub->add_sink(m_panel_sink);
    m_sink_hub->add_sink(m_callback_sink);
}

LogPanel::~LogPanel()
{
    DetachSinks();
}

void LogPanel::DetachSinks()
{
    if (!m_sink_hub) {
        return;
    }
    // remove_sink locks the hub's delivery mutex, so once it returns no
    // logging thread can still be inside the removed sink.
    if (m_panel_sink) {
        m_sink_hub->remove_sink(m_panel_sink);
        m_panel_sink.reset();
    }
    if (m_callback_sink) {
        m_sink_hub->remove_sink(m_callback_sink);
        m_callback_sink.reset();
    }
}

void LogPanel::UpdateStatusLabel()
{
    const unsigned int errors = m_num_errors.load(std::memory_order_relaxed);
    const unsigned int warnings = m_num_warnings.load(std::memory_order_relaxed);

    QString label = "Event Log";
    QString style = "";
    unsigned int k = 0;

    if (errors > 0) {
        label = "error(s)";
        style = "font-weight: bold; color: " + ERROR_COLOR.name();
        k = errors;
    } else if (warnings > 0) {
        label = "warning(s)";
        style = "font-weight: bold; color: " + WARNING_COLOR.name();
        k = warnings;
    }

    if (k > 0) {
        label = QString("%1 %2").arg(QString::number(k), label);
    }

    m_status_button->setStyleSheet(style);
    m_status_button->setText(label);
}

void LogPanel::TogglePanelVisibility()
{
    if (m_output->isVisible()) {
        m_output->hide();
    } else {
        // Since we'll be showing the panel to the user, we can
        // reset the message counters.
        m_output->show();
        m_num_warnings.store(0, std::memory_order_relaxed);
        m_num_errors.store(0, std::memory_order_relaxed);
        UpdateStatusLabel();
    }
}
