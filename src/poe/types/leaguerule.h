// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-LeagueRule

    struct LeagueRule
    {
        QString id;                         // string; examples: Hardcore, NoParties (SSF)
        QString name;                       // string
        std::optional<QString> description; // ?string
    };

} // namespace poe
