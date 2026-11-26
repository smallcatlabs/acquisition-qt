// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "poe/types/item.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PublicStashChange

    struct PublicStashChange
    {
        QString id;                         // string; a unique 64 digit hexadecimal string
        bool public_;                       // bool; if false then optional properties will be null
        std::optional<QString> accountName; // ?string
        std::optional<QString> stash;       // ?string; the name of the stash
        QString stashType;                  // string
        std::optional<QString> league;      // ?string; the league's name
        std::vector<poe::Item> items;       // array of Item
    };

} // namespace poe
