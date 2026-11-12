// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

class QDir;

// Takes the single-instance lock on a data directory, creating the directory
// if needed. Returns nullptr when another running process holds the lock;
// otherwise the lock, held until it is destroyed. A lock file left by a process
// on this computer that is no longer running is taken over; one written on
// another computer, or one that cannot be read, cannot be judged and is
// refused (DataDirLockRefusal names the file). When the lock cannot be taken for
// another reason (an unwritable directory), the returned lock is not held and
// start-up goes on as it did before there was a lock.
std::unique_ptr<QLockFile> LockDataDirectory(const QDir &dataDir);

// The message shown when LockDataDirectory refused: the directory, what holds
// the lock when the lock file says so, and the lock file's full path.
QString DataDirLockRefusal(const QDir &dataDir);
