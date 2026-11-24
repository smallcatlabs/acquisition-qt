// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

class QDir;
class QSettings;

// Back up the 'data' folder inside app_data_dir when previous_version, the
// version that last used that data, differs from current_version. Returns the
// path of the backup, or an empty string when none was made. Recording
// current_version afterwards is the caller's job (OpenGlobalDataStore), so
// that no outcome here can leave it unrecorded (#446).
QString BackupDataOnNewVersion(const QDir &app_data_dir,
                               const QString &previous_version,
                               const QString &current_version);
