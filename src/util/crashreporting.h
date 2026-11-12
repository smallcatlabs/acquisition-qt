// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

class QSettings;

// Whether crash reporting (Sentry and its crashpad handler) may start in this
// process, according to the 'report_crashes' setting. Reporting is on unless
// the user has turned it off; a missing key counts as on.
bool CrashReportingEnabled(const QSettings &settings);
