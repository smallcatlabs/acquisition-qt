// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QHashFunctions> // Needed to avoid obscure errors in std::unordered_map with QString keys.
#include <QString>

#include <optional>
#include <unordered_map>

#include "poe/types/passivegroup.h"
#include "poe/types/passivenode.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-ItemJewelData

    struct ItemJewelData
    {
        struct Subgraph
        {
            // dictionary of PassiveGroup; the key is the string value of the group id
            std::unordered_map<QString, poe::PassiveGroup> groups;
            // dictionary of PassiveNode; the key is the string value of the node identifier
            std::unordered_map<QString, poe::PassiveNode> nodes;
        };

        QString type;                        // string
        std::optional<unsigned> radius;      // ?uint
        std::optional<unsigned> radiusMin;   // ?uint
        std::optional<QString> radiusVisual; // ?string
        // ?object; only present on cluster jewels
        std::optional<poe::ItemJewelData::Subgraph> subgraph;
    };

} // namespace poe
