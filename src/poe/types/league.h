// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include "poe/types/leaguerule.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-League

    struct League
    {
        struct Category
        {
            QString id; // string the league category, e.g.Affliction
            std::optional<bool>
                current; // ? bool set for the active challenge leagues; always true if present
        };

        QString id;                                        // string the league's name
        std::optional<QString> realm;                      // ? string pc, xbox, or sony
        std::optional<QString> name;                       // ? string the league's name
        std::optional<QString> description;                // ? string
        std::optional<poe::League::Category> category;     // ? object
        std::optional<std::vector<poe::LeagueRule>> rules; // ? array of LeagueRule
        std::optional<QString> registerAt;                 // ? string date time(ISO8601)
        std::optional<bool> event;                         // ? bool always true if present
        std::optional<QString> goal;                       // ?string
        std::optional<QString> url;        // ? string a url link to a Path of Exile forum thread
        std::optional<QString> startAt;    // ? string date time(ISO8601)
        std::optional<QString> endAt;      // ? string date time(ISO8601)
        std::optional<bool> timedEvent;    // ? bool always true if present
        std::optional<bool> scoreEvent;    // ? bool always true if present
        std::optional<bool> delveEvent;    // ? bool always true if present
        std::optional<bool> ancestorEvent; // ? bool always true if present
        std::optional<bool> leagueEvent;   // ? bool always true if present
    };

    using LeagueList = std::vector<std::unique_ptr<poe::League>>;

    struct LeagueListWrapper
    {
        std::unique_ptr<poe::LeagueList> leagues;
    };

} // namespace poe
