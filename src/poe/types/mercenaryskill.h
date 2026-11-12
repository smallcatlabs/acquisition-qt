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
            unsigned hash;
            QString name;
            unsigned tier;
        };

        unsigned hash;
        QString icon;
        QString name;
        std::optional<std::vector<poe::MercenarySkill::Support>> supports; // ?array of object
    };

} // namespace poe