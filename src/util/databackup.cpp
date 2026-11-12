// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "util/databackup.h"

#include <QDir>
#include <QFile>
#include <QSettings>

#include "util/spdlog_qt.h" // IWYU pragma: keep

QString BackupDataOnNewVersion(const QDir &app_data_dir,
                               QSettings &settings,
                               const QString &legacy_version,
                               const QString &current_version)
{
    auto version = settings.value("version").toString();
    if (version.isEmpty()) {
        version = legacy_version;
    }

    // Do nothing if the version is current.
    if (version == current_version) {
        spdlog::debug("Application: skipping backup: version is current");
        return {};
    }

    // From here on the version is recorded whether or not a backup is made;
    // otherwise every later start would try again.
    const auto record_version = [&] {
        spdlog::debug("Application: updating 'version' setting to {}", current_version);
        settings.setValue("version", current_version);
    };

    const auto src_path = app_data_dir.filePath("data");
    QDir src{src_path};

    // Do nothing if there's no data directory.
    if (!src.exists()) {
        spdlog::debug("Application: skipping backup: directory does not exist: {}", src_path);
        record_version();
        return {};
    }

    // Do nothing if the data directory is empty. Without NoDotAndDotDot the
    // listing always holds "." and "..", so it is never empty.
    const QStringList entries = src.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        spdlog::debug("Application: skipping backup: directory is empty: {}", src_path);
        record_version();
        return {};
    }

    // Find a backup directory we can use.
    const auto dst_base = app_data_dir.filePath("data-backup-" + version);
    auto dst_path = dst_base;
    int n{0};
    while (app_data_dir.exists(dst_path)) {
        dst_path = dst_base + QString("-%1").arg(++n);
        if (n > 20) {
            spdlog::error("Application: skipping backup: too many backups!");
            return {};
        }
    }

    QDir dst{dst_path};
    if (!dst.exists()) {
        spdlog::debug("Application: creating backup in '{}'", dst_path);
        dst.mkpath(dst_path);
    }

    spdlog::info("Application: backup up data from version '{}' into '{}'", version, dst_path);

    for (const auto &filename : entries) {
        const QString a = src.filePath(filename);
        const QString b = dst.filePath(filename);
        spdlog::debug("Application: backing up {} to {}", a, b);
        QFile::copy(a, b);
    }
    spdlog::info("Your data is backed up into '{}'", dst_path);

    record_version();
    return dst_path;
}
