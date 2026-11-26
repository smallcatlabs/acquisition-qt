// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QStringList>

// The lines that differ between two transcripts (viewtranscript.h, or any
// other text a test prints line by line), as "- before" and "+ after",
// leaving out those that contain one of the `allowed` texts. A
// test states what its operation may change, and checks
//     QVERIFY2(changes.isEmpty(), qPrintable(changes.join('\n')));
// The lines are matched by a longest common subsequence, so a row that
// arrives or leaves is one change and does not shift every line after it.
QStringList viewTranscriptChanges(const QStringList &before,
                                  const QStringList &after,
                                  const QStringList &allowed);
