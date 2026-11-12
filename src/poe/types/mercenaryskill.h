// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <optional>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-MercenarySkill

    struct MercenarySkill
    {
        struct Support
        {
            unsigned hash; // uint
            QString name;  // string
            unsigned tier; // uint
        };

        unsigned hash;                                                     // uint
        QString icon;                                                      // string
        QString name;                                                      // string
        std::optional<std::vector<poe::MercenarySkill::Support>> supports; // ?array of object
    };

} // namespace poe