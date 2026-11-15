// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

// The refusals of a live run (#378): a run against the real API from a test
// program, on a data directory in which the user has logged in. Each function
// returns why the run may not go ahead, or an empty string when it may.

// The directories no live run may use or be inside: the platform's
// application-data locations, where a user's real Acquisition data lives.
QStringList PlatformDataRoots();

// Checked before anything is opened. `dir` must exist, must not be one of
// `forbidden_roots` or inside one (compared after resolving symbolic links,
// ignoring case), and must hold the settings of a login as
// `expected_account`.
QString LiveDirectoryRefusal(const QDir &dir,
                             const QString &expected_account,
                             const QStringList &forbidden_roots);

// Checked after login and before the first API request. Nothing a live run
// does may post to the forum, so a session with a shop thread or with shop
// auto-update on is refused.
QString LiveSessionRefusal(const QString &session_account,
                           const QString &expected_account,
                           const QStringList &shop_threads,
                           bool shop_auto_update);

inline QStringList PlatformDataRoots()
{
    QStringList roots = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    // The application's own locations are named after the running program,
    // so take the directories that hold them.
    for (const auto location :
         {QStandardPaths::AppLocalDataLocation, QStandardPaths::AppDataLocation}) {
        const QString path = QStandardPaths::writableLocation(location);
        if (!path.isEmpty()) {
            roots.append(QFileInfo(path).absolutePath());
        }
    }
    roots.removeDuplicates();
    return roots;
}

namespace liveguards_detail {

    // A path as given and with its symbolic links resolved (when it exists).
    inline QStringList Spellings(const QString &path)
    {
        QStringList spellings{QDir::cleanPath(QFileInfo(path).absoluteFilePath())};
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.isEmpty()) {
            spellings.append(canonical);
        }
        return spellings;
    }

    inline bool IsOrIsInside(const QString &path, const QString &root)
    {
        // "appdata-live" is beside "appdata", not inside it.
        return (path.compare(root, Qt::CaseInsensitive) == 0)
               || path.startsWith(root + '/', Qt::CaseInsensitive);
    }

} // namespace liveguards_detail

inline QString LiveDirectoryRefusal(const QDir &dir,
                                    const QString &expected_account,
                                    const QStringList &forbidden_roots)
{
    if (expected_account.isEmpty()) {
        return "no account was named for this live run";
    }
    if (!dir.exists()) {
        return "the live directory does not exist: " + dir.absolutePath();
    }
    for (const QString &root : forbidden_roots) {
        for (const QString &root_spelling : liveguards_detail::Spellings(root)) {
            for (const QString &dir_spelling : liveguards_detail::Spellings(dir.absolutePath())) {
                if (liveguards_detail::IsOrIsInside(dir_spelling, root_spelling)) {
                    return "a live run never uses the platform's application-data location, and "
                           + dir.absolutePath() + " is " + root + " or inside it";
                }
            }
        }
    }
    const QString settings_file = dir.filePath("settings.ini");
    if (!QFile::exists(settings_file)) {
        return "nobody has logged in to " + dir.absolutePath() + " (it has no settings.ini)";
    }
    const QSettings settings(settings_file, QSettings::IniFormat);
    if (settings.value("account").toString() != expected_account) {
        return dir.absolutePath() + " is not logged in as the account named for this live run";
    }
    return {};
}

inline QString LiveSessionRefusal(const QString &session_account,
                                  const QString &expected_account,
                                  const QStringList &shop_threads,
                                  bool shop_auto_update)
{
    if (expected_account.isEmpty() || (session_account != expected_account)) {
        return "the session is not for the account named for this live run";
    }
    if (!shop_threads.isEmpty()) {
        return "this data directory has a forum shop thread; a live run must not be able to "
               "post to the forum";
    }
    if (shop_auto_update) {
        return "this data directory has shop auto-update on; a live run must not be able to "
               "post to the forum";
    }
    return {};
}
