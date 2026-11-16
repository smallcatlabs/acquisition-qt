// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QDir>
#include <QFile>
#include <QString>
#include <QTreeView>
#include <QWidget>

#include "viewtranscript.h"

// What a live run leaves for whoever started it (#378): `transcript.txt`,
// what the item tree shows with every tab opened, as a view transcript; and
// `window.png`, a picture of the window. They hold the user's tab, character
// and item names, so `out` is checked by LiveOutputRefusal (liveguards.h)
// before anything is run. Returns what went wrong, or nothing.
inline QString WriteLiveRecord(QWidget &window, QTreeView &tree, const QDir &out)
{
    tree.expandAll();
    QFile transcript(out.filePath("transcript.txt"));
    const QByteArray text = (viewTranscript(tree).join('\n') + '\n').toUtf8();
    if (!transcript.open(QIODevice::WriteOnly) || (transcript.write(text) != text.size())) {
        return "cannot write " + transcript.fileName();
    }
    const QString picture = out.filePath("window.png");
    if (!window.grab().save(picture)) {
        return "cannot write " + picture;
    }
    return {};
}
