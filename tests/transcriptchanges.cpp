// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "transcriptchanges.h"

#include <algorithm>

#include <QList>

QStringList viewTranscriptChanges(const QStringList &before,
                                  const QStringList &after,
                                  const QStringList &allowed)
{
    const qsizetype n = before.size();
    const qsizetype m = after.size();
    // common[i][j]: the longest common subsequence of before[i..] and after[j..].
    QList<QList<int>> common(n + 1, QList<int>(m + 1, 0));
    for (qsizetype i = n - 1; i >= 0; --i) {
        for (qsizetype j = m - 1; j >= 0; --j) {
            common[i][j] = (before[i] == after[j]) ? common[i + 1][j + 1] + 1
                                                   : std::max(common[i + 1][j], common[i][j + 1]);
        }
    }
    const auto isAllowed = [&](const QString &line) {
        for (const QString &text : allowed) {
            if (line.contains(text)) {
                return true;
            }
        }
        return false;
    };
    QStringList changes;
    const auto note = [&](QChar sign, const QString &line) {
        if (!isAllowed(line)) {
            changes << QString(sign) + ' ' + line;
        }
    };
    qsizetype i = 0;
    qsizetype j = 0;
    while ((i < n) || (j < m)) {
        if ((i < n) && (j < m) && (before[i] == after[j])) {
            ++i;
            ++j;
        } else if ((i < n) && ((j == m) || (common[i + 1][j] >= common[i][j + 1]))) {
            note('-', before[i++]);
        } else {
            note('+', after[j++]);
        }
    }
    return changes;
}
