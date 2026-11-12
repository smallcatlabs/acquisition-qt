// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PassiveGroup

    struct PassiveGroup
    {
        float x;                      // float
        float y;                      // float
        std::vector<unsigned> orbits; // array of uint
        std::optional<bool> isProxy;  // ?bool; always true if present
        std::optional<QString> proxy; // ?string; identifier of the placeholder node
        // array of string; the node identifiers associated with this group
        std::vector<QString> nodes;
    };

} // namespace poe
