// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "poe/types/pvpladderteammember.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PvPLadderTeamEntry

    struct PvPLadderTeamEntry
    {
        unsigned rank;                  // uint
        std::optional<unsigned> rating; // ?uint; only present if the PvP Match uses Glicko ratings
        std::optional<unsigned> points; // ?uint
        std::optional<unsigned> games_played;               // ?uint
        std::optional<unsigned> cumulative_opponent_points; // ?uint
        std::optional<QString> last_game_time;              // ?string; date time (ISO8601)
        std::vector<poe::PvPLadderTeamMember> members;      // array of PvPLadderTeamMember
    };

} // namespace poe
