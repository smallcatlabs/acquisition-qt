// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QString>

#include <spdlog/spdlog.h>

namespace logging {

    void init(const QString &filename);

    // Parse a logging level name such as "debug" or "INFO", ignoring case.
    // A name that is not a level gives the fallback, with a warning.
    spdlog::level::level_enum parseLevel(const QString &name, spdlog::level::level_enum fallback);
}
