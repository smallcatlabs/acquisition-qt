// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <QString>

#include <glaze/glaze.hpp>

#include "poe/types/account.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PvPLadderTeamMember

    struct PvPLadderTeamMember
    {
        struct Character
        {
            QString id;                    // string; a unique 64 digit hexadecimal string
            QString name;                  // string
            unsigned level;                // uint
            QString class_;                // string
            std::optional<QString> league; // ?string
            std::optional<unsigned> score; // ?uint; count of league objective completions
        };

        poe::Account account;                          // Account
        poe::PvPLadderTeamMember::Character character; // object
        std::optional<bool> public_;                   // ?bool; always true if present
    };

} // namespace poe

template<>
struct glz::meta<poe::PvPLadderTeamMember::Character>
{
    using T = poe::PvPLadderTeamMember::Character;
    static constexpr auto modify = glz::object("class", &T::class_);
};

template<>
struct glz::meta<poe::PvPLadderTeamMember>
{
    using T = poe::PvPLadderTeamMember;
    static constexpr auto modify = glz::object("public", &T::public_);
};
