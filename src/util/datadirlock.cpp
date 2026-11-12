// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "util/datadirlock.h"

#include <QDir>

#include <chrono>

std::unique_ptr<QLockFile> LockDataDirectory(const QDir &dataDir)
{
    QDir().mkpath(dataDir.absolutePath());
    auto lock = std::make_unique<QLockFile>(dataDir.filePath("acquisition.lock"));
    // Held for the whole run, so as the Qt documentation advises for a
    // long-lived lock: never treat a lock as stale by age (only when its
    // process is gone), and do not wait for it.
    lock->setStaleLockTime(std::chrono::milliseconds::zero());
    if (!lock->tryLock() && (lock->error() == QLockFile::LockFailedError)) {
        return nullptr;
    }
    return lock;
}
