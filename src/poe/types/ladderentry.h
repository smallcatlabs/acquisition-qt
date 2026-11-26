// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include "poe/types/rawjson.h"

#include "poe/types/account.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-LadderEntry

    struct LadderEntry
    {
        struct DelveInfo
        {
            std::optional<unsigned> default_; // ?uint
            std::optional<unsigned> solo;     // ?uint
        };

        struct Character
        {
            QString id;     // string; a unique 64 digit hexadecimal string
            QString name;   // string
            unsigned level; // uint
            QString class_; // string
            // ?uint; time taken to complete the league objective in seconds
            std::optional<unsigned> time;
            std::optional<unsigned> score; // ?uint; count of league objective completions
            // NOTE: kept as the raw object; the reference gives it no shape.
            // ?object; the values of this depend on the league objective
            std::optional<poe::RawJson> progress;
            std::optional<unsigned> experience; // ?uint
            // ?object; deepest Delve depth completed
            std::optional<poe::LadderEntry::DelveInfo> depth;
        };

        unsigned rank;                         // uint
        std::optional<bool> dead;              // ?bool
        std::optional<bool> retired;           // ?bool
        std::optional<bool> ineligible;        // ?bool
        std::optional<bool> public_;           // ?bool
        poe::LadderEntry::Character character; // object
        std::optional<poe::Account> account;   // ?Account
    };

} // namespace poe
