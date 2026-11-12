// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "poe/types/itemproperty.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-GemPage

    struct GemPage
    {
        std::optional<QString> skillName;                         // ?string
        std::optional<QString> description;                       // ?string
        std::optional<std::vector<poe::ItemProperty>> properties; // ?array of ItemProperty
        std::optional<std::vector<QString>> stats;                // ?array of string
    };

} // namespace poe
