// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "viewtranscript.h"

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDir>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QRegularExpression>
#include <QTest>
#include <QTreeView>
#include <QWidget>

namespace {

    void appendViewTranscript(const QTreeView &view,
                              const QModelIndex &parent,
                              int depth,
                              int columns,
                              QStringList &lines)
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

} // namespace

QStringList viewTranscript(const QTreeView &view, int columns)
{
    QStringList lines;
    if (view.model()) {
        appendViewTranscript(view, view.rootIndex(), 0, columns, lines);
    }
    return lines;
}

void saveTestScreenshot(QWidget &widget, const QString &step)
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

ScreenshotOnFailure::~ScreenshotOnFailure()
{
    if (QTest::currentTestFailed()) {
        saveTestScreenshot(widget, QStringLiteral("failed"));
    }
}
