// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QStringList>

#include "transcriptchanges.h"

class QTreeView;
class QWidget;

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
QStringList viewTranscript(const QTreeView &view, int columns = -1);

// Saves what `widget` shows as a PNG when the environment variable
// ACQ_TEST_SCREENSHOT_DIR names a directory, and does nothing otherwise. The
// file is named after the test executable, the test function, its data row
// and `step`, for example tst_mainwindow-someTest-before.png.
void saveTestScreenshot(QWidget &widget, const QString &step);

// Saves a screenshot named "failed" when the test function fails while this
// is in scope. Declare it after whatever owns the widget, so the widget is
// still alive when a failed check returns from the test function.
struct ScreenshotOnFailure
{
    QWidget &widget;
    ~ScreenshotOnFailure();
};
