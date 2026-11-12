// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "util/crashreporting.h"

#include <QSettings>

bool CrashReportingEnabled(const QSettings &settings)
{
    return settings.value("report_crashes", true).toBool();
}
