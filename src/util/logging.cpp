// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "util/logging.h"

#include <sentry.h>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

#ifdef _WIN32
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include "util/spdlog_qt.h" // IWYU pragma: keep

constexpr int MAX_FILES = 20;

constexpr int MAX_LOG_SIZE = 10 * 1024 * 1024; // 10MB

#ifdef Q_OS_WIN
// Visual Studio debug output on Windows
using DEBUG_SINK = spdlog::sinks::msvc_sink_mt;
#else
// Debug console output sink for macOS/Linux)
using DEBUG_SINK = spdlog::sinks::stdout_color_sink_mt;
#endif

void logging::init(const QString &filename)
{
    // Create a debug sink for the c
    auto debug_sink = std::make_shared<DEBUG_SINK>();
    debug_sink->set_level(spdlog::level::trace);

    // Create a file sink for the log file.
    const auto path = filename.toStdString();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path,
                                                                            MAX_LOG_SIZE,
                                                                            MAX_FILES);
    file_sink->set_level(spdlog::level::trace);

    /* TBD:
    // Create a sink for sentry (but only capture errors)
    auto sentry_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [](const spdlog::details::log_msg &msg) {
            // Make absolutely sure we are only logging errors and fatal errors,
            // since there seems to be no way to filter sentry logs, e.g. to only
            // report the X latest log messages.
            if (msg.level >= spdlog::level::err) {
                std::string payload{msg.payload.data(), msg.payload.size()};
                sentry_log_error(payload.c_str());
            }
        });
    sentry_sink->set_level(spdlog::level::err);
    */

    // Create a permanent hub for sinks that come and go with UI objects
    // (LogPanel). The logger's own sink vector is not synchronized, so it
    // must never be mutated while other threads may be logging; the dist
    // sink's add_sink/remove_sink share the delivery mutex, which makes
    // attach and detach safe at any time (F42).
    auto ui_sink_hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    ui_sink_hub->set_level(spdlog::level::warn); // raise if a sub-sink needs lower

    // Create sinks vector
    std::vector<spdlog::sink_ptr> sinks = {
        debug_sink,
        file_sink,
        ui_sink_hub,
        // sentry_sink,
    };

    // Create logger with both sinks
    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());

    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}

spdlog::level::level_enum logging::parseLevel(const QString &name,
                                              spdlog::level::level_enum fallback)
{
    // spdlog::level::from_str() is case-sensitive and returns 'off' for any
    // name it does not know, which would silently disable the log.
    const std::string lowered = name.trimmed().toLower().toStdString();
    const auto level = spdlog::level::from_str(lowered);
    if (level == spdlog::level::off && lowered != "off") {
        spdlog::warn("Unknown logging level '{}'; using {}",
                     name,
                     spdlog::level::to_string_view(fallback));
        return fallback;
    }
    return level;
}
