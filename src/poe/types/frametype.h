// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-FrameType

    enum class FrameType : unsigned {
        Normal = 0,         // Normal frame
        Magic = 1,          // Magic frame
        Rare = 2,           // Rare frame
        Unique = 3,         // Unique frame
        Gem = 4,            // Gem frame
        Currency = 5,       // Currency frame
        DivinationCard = 6, // Divination Card frame
        Quest = 7,          // Quest frame
        Prophecy = 8,       // Prophecy frame (legacy)
        Foil = 9,           // Foil frame
        SupporterFoil = 10, // Supporter Foil frame
        Necropolis = 11,    // Necropolis frame
        Gold = 12,          // Gold frame
        BreachSkill = 13    // Breach Skill frame
    };

} // namespace poe
