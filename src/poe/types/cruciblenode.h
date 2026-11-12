// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-CrucibleNode

    struct CrucibleNode
    {
        std::optional<unsigned> skill;                    // ?uint; mod hash
        std::optional<unsigned> tier;                     // ?uint; mod tier
        std::optional<QString> icon;                      // ?string
        std::optional<bool> allocated;                    // ?bool; always true if present
        std::optional<bool> isNotable;                    // ?bool; always true if present
        std::optional<bool> isReward;                     // ?bool; always true if present
        std::optional<std::vector<QString>> stats;        // ?array of string; stat descriptions
        std::optional<std::vector<QString>> reminderText; // ?array of string
        std::optional<unsigned> orbit;                    // ?uint; the column this node occupies
        std::optional<unsigned> orbitIndex; // ?uint; the node's position within the column
        std::vector<QString> out; // array of string; node identifiers of nodes this one connects to
        std::vector<QString> in; // array of string; node identifiers of nodes connected to this one
    };

} // namespace poe
