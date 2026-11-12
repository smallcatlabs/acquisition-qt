// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#pragma once

#include <QImage>
#include <QPixmap>
#include <QString>

namespace Ui {
    class MainWindow;
}

class Item;

void UpdateItemTooltip(const Item &item, Ui::MainWindow *ui);
QPixmap GenerateItemIcon(const Item &item, const QImage &image);

// How an item's frame type is drawn in the item panel: the resource prefix of
// its header images (Left/Middle/Right.png are appended), the key of its
// separator image, and the colour of its name. Frame types without art of
// their own borrow the closest existing images.
struct ItemFrameStyle
{
    QString header_prefix;
    QString separator_key;
    QString color;
};
ItemFrameStyle ItemFrameStyleFor(int frame_type, bool singleline);

// The resource path of the Shaper or Elder background drawn behind an item's
// icon, or an empty string when there is none for its influence or size.
QString InfluenceBackgroundFor(const Item &item);
