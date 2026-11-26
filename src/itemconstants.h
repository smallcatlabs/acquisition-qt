// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QObject>
#include <QString>

#include <map>

namespace ItemEnums {
    Q_NAMESPACE

    enum FrameType {
        FRAME_TYPE_NORMAL = 0,
        FRAME_TYPE_MAGIC = 1,
        FRAME_TYPE_RARE = 2,
        FRAME_TYPE_UNIQUE = 3,
        FRAME_TYPE_GEM = 4,
        FRAME_TYPE_CURRENCY = 5,
        FRAME_TYPE_DIVINATION_CARD = 6,
        FRAME_TYPE_QUEST_ITEM = 7,
        FRAME_TYPE_PROPHECY = 8,
        FRAME_TYPE_FOIL = 9,
        FRAME_TYPE_SUPPORTER_FOIL = 10,
        FRAME_TYPE_NECROPOLIS = 11,
        FRAME_TYPE_GOLD = 12,
        FRAME_TYPE_BREACH_SKILL = 13
    };
    Q_ENUM_NS(FrameType)

    enum ElementalDamageType {
        ED_FIRE = 4,
        ED_COLD = 5,
        ED_LIGHTNING = 6,
    };
    Q_ENUM_NS(ElementalDamageType)

} // namespace ItemEnums

using FrameType = ItemEnums::FrameType;
using ElementalDamageType = ItemEnums::ElementalDamageType;
constexpr int PIXELS_PER_SLOT = 47;
constexpr int INVENTORY_SLOTS = 12;
constexpr int PIXELS_PER_MINIMAP_SLOT = 10;
constexpr int MINIMAP_SIZE = INVENTORY_SLOTS * PIXELS_PER_MINIMAP_SLOT;

struct position
{
    double x;
    double y;
};

const std::map<QString, position> &POS_MAP();
