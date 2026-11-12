// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PvPMatch

    struct PvPMatch
    {
        QString id;                        // string; the match's name
        std::optional<QString> realm;      // ?string; pc, xbox, or sony
        std::optional<QString> startAt;    // ?string; date time (ISO8601)
        std::optional<QString> endAt;      // ?string; date time (ISO8601)
        std::optional<QString> url;        // ?string; a url link to a Path of Exile forum thread
        QString description;               // string
        bool glickoRatings;                // bool
        bool pvp;                          // bool; always true
        QString style;                     // string; Blitz, Swiss, or Arena
        std::optional<QString> registerAt; // ?string; date time (ISO8601)
        std::optional<bool> complete;      // ?bool; always true if present
        std::optional<bool> upcoming;      // ?bool; always true if present
        std::optional<bool> inProgress;    // ?bool; always true if present
    };

} // namespace poe
