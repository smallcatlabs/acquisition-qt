// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <glaze/glaze.hpp>

#include "poe/types/account.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-LadderEntry

    struct LadderEntry
    {
        struct DelveInfo
        {
            std::optional<unsigned> default_; // ? uint
            std::optional<unsigned> solo;     // ? uint
        };

        struct Character
        {
            QString id;     // string a unique 64 digit hexadecimal string
            QString name;   // string
            unsigned level; // uint
            QString class_; // string
            std::optional<unsigned>
                time; // ? uint time taken to complete the league objective in seconds
            std::optional<unsigned> score; // ? uint count of league objective completions
            // NOTE: kept as the raw object; the reference gives it no shape.
            // ?object; the values of this depend on the league objective
            std::optional<glz::raw_json> progress;
            std::optional<unsigned> experience;               // ? uint
            std::optional<poe::LadderEntry::DelveInfo> depth; // ? object deepest Delve depth completed
        };

        unsigned rank;                         // uint
        std::optional<bool> dead;              // ? bool
        std::optional<bool> retired;           // ? bool
        std::optional<bool> ineligible;        // ? bool
        std::optional<bool> public_;           // ? bool
        poe::LadderEntry::Character character; // object
        std::optional<poe::Account> account;   // ? Account
    };

} // namespace poe

template<>
struct glz::meta<poe::LadderEntry::DelveInfo>
{
    using T = poe::LadderEntry::DelveInfo;
    static constexpr auto modify = glz::object("default", &T::default_);
};

template<>
struct glz::meta<poe::LadderEntry::Character>
{
    using T = poe::LadderEntry::Character;
    static constexpr auto modify = glz::object("class", &T::class_);
};

template<>
struct glz::meta<poe::LadderEntry>
{
    using T = poe::LadderEntry;
    static constexpr auto modify = glz::object("public", &T::public_);
};
