// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <unordered_map>
#include <vector>

#include <QHashFunctions> // Needed to avoid obscure errors in std::unordered_map with QString keys.
#include <QString>

#include "poe/types/item.h"
#include "poe/types/mercenaryskill.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-LeagueAccount

    struct LeagueAccount
    {
        struct AtlasPassiveTree
        {
            QString name;                 // string
            std::vector<unsigned> hashes; // array of uint
            // dictionary of int; the key is the string value of the variant node's skill hash and
            // the value is the selected effect hash
            std::unordered_map<QString, int> variants;
        };

        struct Mercenaries
        {
            unsigned index; // uint
            QString name;
            unsigned level;
            QString build;
            unsigned build_hash;
            std::vector<poe::MercenarySkill> skills; // array of MercenarySkill
            std::vector<poe::Item> items;            // array of Item
        };

        std::vector<poe::LeagueAccount::AtlasPassiveTree> atlas_passive_trees; // array of object
        std::vector<poe::LeagueAccount::Mercenaries> mercenaries;              // array of object
    };

} // namespace poe
