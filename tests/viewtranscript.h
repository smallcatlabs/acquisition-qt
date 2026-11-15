// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <algorithm>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QRegularExpression>
#include <QStringList>
#include <QTest>
#include <QTreeView>
#include <QWidget>

// A view transcript (#375): what a QTreeView shows, as text, so a window
// test can compare the whole tree before and after an operation instead of
// the one thing it is about. Read only through the view's public API.
//
// One line per visible row, in view order:
//
//     [-] Alpha Tab
//           Chaos Orb | ~b/o 5 chaos
//   *>      Tabula Rasa | ~b/o 5 chaos
//     [+] Beta Tab
//
// '*' marks a selected row and '>' the current row; then two spaces per
// level of nesting; then "[-]" for an expanded row with children, "[+]" for
// a collapsed one, four spaces for a row without children; then the texts.
// Decisions: columns print in visual order (what the user sees), hidden
// columns are left out; a row is selected or current by its column 0; an
// empty text prints as nothing, and empty texts at the end of a row are
// dropped, so a tab header is its title alone. `columns`, when given, is how
// many of the visible columns to print, counted from the left: a test that
// writes its expected lines out names the few it is about.
inline void appendViewTranscript(
    const QTreeView &view, const QModelIndex &parent, int depth, int columns, QStringList &lines)
{
    const QAbstractItemModel &model = *view.model();
    const QItemSelectionModel *selection = view.selectionModel();
    const QModelIndex current = view.currentIndex();
    const QHeaderView &header = *view.header();
    for (int row = 0; row < model.rowCount(parent); ++row) {
        if (view.isRowHidden(row, parent)) {
            continue;
        }
        const QModelIndex index = model.index(row, 0, parent);
        const bool expanded = model.hasChildren(index) && view.isExpanded(index);
        QString line;
        line += (selection && selection->isSelected(index)) ? '*' : ' ';
        line += (current.isValid() && (current.siblingAtColumn(0) == index)) ? '>' : ' ';
        line += QString(2 * depth, ' ');
        line += !model.hasChildren(index) ? QStringLiteral("    ")
                : expanded                ? QStringLiteral("[-] ")
                                          : QStringLiteral("[+] ");
        QStringList texts;
        for (int visual = 0; visual < header.count(); ++visual) {
            const int column = header.logicalIndex(visual);
            if (!view.isColumnHidden(column) && ((columns < 0) || (texts.size() < columns))) {
                texts << model.index(row, column, parent).data(Qt::DisplayRole).toString();
            }
        }
        while (!texts.isEmpty() && texts.back().isEmpty()) {
            texts.removeLast();
        }
        lines << line + texts.join(QStringLiteral(" | "));
        if (expanded) {
            appendViewTranscript(view, index, depth + 1, columns, lines);
        }
    }
}

inline QStringList viewTranscript(const QTreeView &view, int columns = -1)
{
    QStringList lines;
    if (view.model()) {
        appendViewTranscript(view, view.rootIndex(), 0, columns, lines);
    }
    return lines;
}

// The lines that differ between two transcripts, as "- before" and
// "+ after", leaving out those that contain one of the `allowed` texts. A
// test states what its operation may change, and checks
//     QVERIFY2(changes.isEmpty(), qPrintable(changes.join('\n')));
// The lines are matched by a longest common subsequence, so a row that
// arrives or leaves is one change and does not shift every line after it.
inline QStringList viewTranscriptChanges(const QStringList &before,
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

// Saves what `widget` shows as a PNG when the environment variable
// ACQ_TEST_SCREENSHOT_DIR names a directory, and does nothing otherwise. The
// file is named after the test executable, the test function, its data row
// and `step`, for example tst_mainwindow-someTest-before.png.
inline void saveTestScreenshot(QWidget &widget, const QString &step)
{
    const QString dir = qEnvironmentVariable("ACQ_TEST_SCREENSHOT_DIR");
    if (dir.isEmpty()) {
        return;
    }
    QStringList parts{QCoreApplication::applicationName(), QTest::currentTestFunction()};
    if (QTest::currentDataTag()) {
        parts << QString(QTest::currentDataTag());
    }
    parts << step;
    QString name = parts.join('-');
    name.replace(QRegularExpression("[^A-Za-z0-9_.-]"), QStringLiteral("_"));
    if (!QDir().mkpath(dir) || !widget.grab().save(QDir(dir).filePath(name + ".png"))) {
        qWarning("saveTestScreenshot: could not save %s in %s", qPrintable(name), qPrintable(dir));
    }
}

// Saves a screenshot named "failed" when the test function fails while this
// is in scope. Declare it after whatever owns the widget, so the widget is
// still alive when a failed check returns from the test function.
struct ScreenshotOnFailure
{
    QWidget &widget;
    ~ScreenshotOnFailure()
    {
        if (QTest::currentTestFailed()) {
            saveTestScreenshot(widget, QStringLiteral("failed"));
        }
    }
};
