// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QLockFile>

#include <memory>

class QDir;

// Takes the single-instance lock on a data directory, creating the directory
// if needed. Returns nullptr when another running process holds the lock;
// otherwise the lock, held until it is destroyed. A lock file left by a process
// that is no longer running is taken over. When the lock cannot be taken for
// another reason (an unwritable directory), the returned lock is not held and
// start-up goes on as it did before there was a lock.
std::unique_ptr<QLockFile> LockDataDirectory(const QDir &dataDir);
