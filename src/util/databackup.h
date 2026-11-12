// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

class QDir;
class QSettings;

// Back up the 'data' folder inside app_data_dir when the version recorded in
// the settings differs from current_version, and record current_version.
// legacy_version stands in for the recorded version when the settings have
// none (the 'version' setting was introduced in v0.16). Returns the path of
// the backup, or an empty string when none was made.
QString BackupDataOnNewVersion(const QDir &app_data_dir,
                               QSettings &settings,
                               const QString &legacy_version,
                               const QString &current_version);
