// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "poe/types/displaymode.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-ItemProperty

    struct ItemProperty
    {
        QString name;                                 // string
        std::vector<std::tuple<QString, int>> values; // array of array
        std::optional<poe::DisplayMode> displayMode;  // ?uint as DisplayMode
        std::optional<double> progress;               // ?double; rounded to 2 decimal places
        std::optional<unsigned> type;                 // ?uint
        std::optional<QString> suffix;                // ?string
        std::optional<QString> icon;                  // ?string
    };

} // namespace poe
