// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "util/datadirlock.h"

#include <QDir>
#include <QString>

#include <chrono>

static QString lockFilePath(const QDir &dataDir)
{
    return dataDir.absoluteFilePath("acquisition.lock");
}

std::unique_ptr<QLockFile> LockDataDirectory(const QDir &dataDir)
{
    QDir().mkpath(dataDir.absolutePath());
    auto lock = std::make_unique<QLockFile>(lockFilePath(dataDir));
    // Held for the whole run, so as the Qt documentation advises for a
    // long-lived lock: never treat a lock as stale by age (only when its
    // process is gone), and do not wait for it.
    lock->setStaleLockTime(std::chrono::milliseconds::zero());
    if (!lock->tryLock() && (lock->error() == QLockFile::LockFailedError)) {
        return nullptr;
    }
    return lock;
}

QString DataDirLockRefusal(const QDir &dataDir)
{
    const QString lock_path = lockFilePath(dataDir);
    qint64 pid = 0;
    QString host;
    QString app;
    QString holder;
    if (QLockFile(lock_path).getLockInfo(&pid, &host, &app)) {
        holder = QString("It is held by process %1 (%2) on %3.")
                     .arg(QString::number(pid), app.isEmpty() ? QString("unknown") : app, host);
    } else {
        holder = QString("The lock file does not say what holds it.");
    }
    return QString("Acquisition is already running with this data directory:\n\n%1\n\n%2\n\n"
                   "Use that Acquisition, or start with --data-dir to use another directory. "
                   "If you are sure no Acquisition is using this directory, on this or any "
                   "other computer, delete the lock file and start again:\n\n%3")
        .arg(QDir::toNativeSeparators(dataDir.absolutePath()),
             holder,
             QDir::toNativeSeparators(lock_path));
}
