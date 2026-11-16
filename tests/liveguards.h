// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
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

// The refresh and network code a live run must not be testing: the `src/`
// entries of the `paths:` lists in the text of
// .github/workflows/sanitizers.yml, in that file's pattern syntax.
QStringList LiveNetworkPatterns(const QString &workflow_text);

// Whether `changed` (paths from the top of the repository) holds a file one
// of `patterns` matches. Refuses too when there is nothing to compare with.
QString LiveChangeRefusal(const QStringList &changed, const QStringList &patterns);

// The files in which the work tree at `source` differs from the commit where
// its branch left origin/master: committed, staged, unstaged and untracked.
// `top` is the top of that work tree. `error` says why they cannot be told.
struct LiveChanges
{
    QString top;
    QStringList files;
    QString error;
};
LiveChanges LiveChangesSinceMaster(const QDir &source);

// Checked before anything is opened: a live run is never made on a branch
// that changes the refresh or network code (AGENTS.md, "Test"). `source` is
// the source tree the program was built from. Whatever cannot be told is
// refused.
QString LiveBranchRefusal(const QDir &source);

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

inline QStringList LiveNetworkPatterns(const QString &workflow_text)
{
    static const QRegularExpression list_start(R"(^\s*paths:\s*$)");
    static const QRegularExpression list_item(R"(^\s*-\s+['"]?([^'"\s]+)['"]?\s*$)");
    static const QRegularExpression comment(R"((^|\s)#.*$)");
    QStringList patterns;
    bool in_paths = false;
    for (QString line : workflow_text.split('\n')) {
        // A comment or an empty line inside a list does not end it.
        line.remove(comment);
        if (line.trimmed().isEmpty()) {
            continue;
        }
        if (list_start.match(line).hasMatch()) {
            in_paths = true;
            continue;
        }
        const auto item = list_item.match(line);
        in_paths = in_paths && item.hasMatch();
        if (in_paths && item.captured(1).startsWith("src/")) {
            patterns.append(item.captured(1));
        }
    }
    return patterns;
}

inline QString LiveChangeRefusal(const QStringList &changed, const QStringList &patterns)
{
    if (patterns.isEmpty()) {
        return "the list of refresh and network code is empty";
    }
    QStringList found;
    for (const QString &pattern : patterns) {
        // Only what the workflow uses today is read: `**` is any run of
        // characters and `*` any run without a '/'. Anything else would be
        // taken as written, match nothing, and let every change through.
        static const QRegularExpression unread(R"([?\[\]{}!])");
        if (pattern.contains(unread)) {
            return "the pattern '" + pattern + "' is in a syntax this check does not read";
        }
        QString expression = QRegularExpression::escape(pattern);
        expression.replace(R"(\*\*)", ".*").replace(R"(\*)", "[^/]*");
        const QRegularExpression matcher(QRegularExpression::anchoredPattern(expression));
        for (const QString &file : changed) {
            if (matcher.match(file).hasMatch() && !found.contains(file)) {
                found.append(file);
            }
        }
    }
    if (found.isEmpty()) {
        return {};
    }
    found.sort();
    return "this branch changes the refresh or network code (" + found.join(", ")
           + "), and a live run is never made on such a branch";
}

namespace liveguards_detail {

    // The output of `git <arguments>` in `dir`, or nothing and `error`.
    inline QString Git(const QString &dir, const QStringList &arguments, QString &error)
    {
        const QString git = QStandardPaths::findExecutable("git");
        if (git.isEmpty()) {
            error = "git was not found";
            return {};
        }
        QProcess process;
        process.setWorkingDirectory(dir);
        process.start(git, arguments);
        if (!process.waitForFinished(30000) || (process.exitStatus() != QProcess::NormalExit)
            || (process.exitCode() != 0)) {
            process.kill();
            process.waitForFinished(1000);
            error = "git " + arguments.join(' ') + " failed in " + dir + ": "
                    + QString::fromUtf8(process.readAllStandardError()).simplified();
            return {};
        }
        return QString::fromUtf8(process.readAllStandardOutput());
    }

} // namespace liveguards_detail

inline LiveChanges LiveChangesSinceMaster(const QDir &source)
{
    using liveguards_detail::Git;
    LiveChanges changes;
    const QString dir = source.absolutePath();
    changes.top = Git(dir, {"rev-parse", "--show-toplevel"}, changes.error).trimmed();
    if (!changes.error.isEmpty()) {
        return changes;
    }
    // Where the branch left master, so that what master has done since is
    // not counted as the branch's.
    const QString base
        = Git(dir, {"merge-base", "HEAD", "refs/remotes/origin/master"}, changes.error).trimmed();
    if (!changes.error.isEmpty()) {
        changes.error = "cannot tell where this branch left origin/master (" + changes.error + ")";
        return changes;
    }
    // Committed, staged and unstaged. A moved file counts under both names.
    // A check leaves the repository as it found it: without the setting,
    // git notes in its index which files it found unchanged.
    const QString tracked = Git(
        changes.top,
        {"-c", "diff.autoRefreshIndex=false", "diff", "--name-only", "--no-renames", "-z", base},
        changes.error);
    if (!changes.error.isEmpty()) {
        return changes;
    }
    const QString untracked = Git(changes.top,
                                  {"ls-files", "--others", "--exclude-standard", "-z"},
                                  changes.error);
    if (!changes.error.isEmpty()) {
        return changes;
    }
    changes.files = (tracked + untracked).split(QChar(0), Qt::SkipEmptyParts);
    return changes;
}

inline QString LiveBranchRefusal(const QDir &source)
{
    const LiveChanges changes = LiveChangesSinceMaster(source);
    if (!changes.error.isEmpty()) {
        return "a live run is never made on a branch that changes the refresh or network code, "
               "and that cannot be told here: "
               + changes.error;
    }
    const QString workflow_name = ".github/workflows/sanitizers.yml";
    QFile workflow(QDir(changes.top).filePath(workflow_name));
    const QStringList patterns = workflow.open(QIODevice::ReadOnly)
                                     ? LiveNetworkPatterns(QString::fromUtf8(workflow.readAll()))
                                     : QStringList();
    if (patterns.isEmpty()) {
        return "the refresh and network code is listed in " + workflow_name + ", and "
               + workflow.fileName() + " lists none";
    }
    const QString refusal = LiveChangeRefusal(changes.files, patterns);
    // A stale origin/master makes master's own later changes look like the
    // branch's; say what the comparison was with.
    return refusal.isEmpty() ? refusal : refusal + " (compared with origin/master as last fetched)";
}
