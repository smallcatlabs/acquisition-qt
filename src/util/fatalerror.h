// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QString>

#include <cstdio>

// For broken invariants only: reports the message and aborts. A problem
// the user can fix (missing SSL, a configuration error) exits cleanly instead.
[[noreturn]] void FatalError(const QString &message);

// What FatalError reports before aborting: the message at critical level in
// the log, and to `out` whatever the log level is. Separate so it can be tested.
void ReportFatalError(const QString &message, std::FILE *out);
