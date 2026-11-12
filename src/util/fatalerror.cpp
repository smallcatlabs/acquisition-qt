// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#include "util/fatalerror.h"

#include <cstdio>
#include <cstdlib>

#include "util/spdlog_qt.h" // IWYU pragma: keep

void ReportFatalError(const QString &message, std::FILE *out)
{
    // The log level can be "off", which filters even critical messages, so
    // the message also goes to `out` unconditionally.
    std::fprintf(out, "Acquisition fatal error: %s\n", qUtf8Printable(message));
    std::fflush(out);
    spdlog::critical(message);
    spdlog::critical("Aborting acquisition after a fatal error.");
}

[[noreturn]] void FatalError(const QString &message)
{
    // A broken invariant: nothing more may run, so no dialog and no nested
    // event loop that would deliver timers and replies to objects in a bad
    // state. The critical level flushes the log, and the abort produces a
    // crash report when crash reporting is on.
    ReportFatalError(message, stderr);
    std::abort();
}
