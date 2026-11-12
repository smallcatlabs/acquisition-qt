// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-EventLadderEntry

    struct EventLadderEntry
    {
        struct PrivateLeague
        {
            QString name; // string
            QString url;  // string; a url link to a Path of Exile Private League
        };

        unsigned rank;                  // uint
        std::optional<bool> ineligible; // ?bool
        // ?uint; time taken to complete the league objective in seconds
        std::optional<unsigned> time;
        poe::EventLadderEntry::PrivateLeague private_league; // object
    };

} // namespace poe
