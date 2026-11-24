// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "util/databackup.h"

#include <QDir>
#include <QFile>

#include "util/spdlog_qt.h" // IWYU pragma: keep

QString BackupDataOnNewVersion(const QDir &app_data_dir,
                               const QString &previous_version,
                               const QString &current_version)
{
    // Do nothing if the version is current.
    if (previous_version == current_version) {
        spdlog::debug("Application: skipping backup: version is current");
        return {};
    }

    const auto src_path = app_data_dir.filePath("data");
    QDir src{src_path};

    // Do nothing if there's no data directory.
    if (!src.exists()) {
        spdlog::debug("Application: skipping backup: directory does not exist: {}", src_path);
        return {};
    }

    // Do nothing if the data directory is empty. Without NoDotAndDotDot the
    // listing always holds "." and "..", so it is never empty.
    const QStringList entries = src.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        spdlog::debug("Application: skipping backup: directory is empty: {}", src_path);
        return {};
    }

    // Find a backup directory we can use.
    const auto dst_base = app_data_dir.filePath("data-backup-" + previous_version);
    auto dst_path = dst_base;
    int n{0};
    while (app_data_dir.exists(dst_path)) {
        dst_path = dst_base + QString("-%1").arg(++n);
        if (n > 20) {
            spdlog::error("Application: skipping backup: '{}' and its {} numbered copies "
                          "already exist",
                          dst_base,
                          n - 1);
            return {};
        }
    }

    QDir dst{dst_path};
    if (!dst.exists()) {
        spdlog::debug("Application: creating backup in '{}'", dst_path);
        dst.mkpath(dst_path);
    }

    spdlog::info("Application: backup up data from version '{}' into '{}'",
                 previous_version,
                 dst_path);

    for (const auto &filename : entries) {
        const QString a = src.filePath(filename);
        const QString b = dst.filePath(filename);
        spdlog::debug("Application: backing up {} to {}", a, b);
        QFile::copy(a, b);
    }
    spdlog::info("Your data is backed up into '{}'", dst_path);
    return dst_path;
}
