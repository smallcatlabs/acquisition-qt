// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#include "util/fatalerror.h"

#include <cstdlib>

#include "util/spdlog_qt.h" // IWYU pragma: keep

[[noreturn]] void FatalError(const QString &message)
{
    // A broken invariant: nothing more may run, so no dialog and no nested
    // event loop that would deliver timers and replies to objects in a bad
    // state. The critical level flushes the log, and the abort produces a
    // crash report when crash reporting is on.
    spdlog::critical(message);
    spdlog::critical("Aborting acquisition after a fatal error.");
    std::abort();
}
