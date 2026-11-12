// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <vector>

#include <QString>

#include <glaze/glaze.hpp>

#include "poe/types/item.h"

namespace poe {

    struct WebCharacter
    {
        QString name;
        QString realm;
        QString class_;
        QString league;
        unsigned level;
        bool pinnable;
    };

    struct WebCharacterList
    {
        std::vector<poe::WebCharacter> characters;
    };

    struct WebCharacterStash
    {
        std::vector<poe::Item> items;
    };

} // namespace poe

template<>
struct glz::meta<poe::WebCharacter>
{
    using T = poe::WebCharacter;
    static constexpr auto modify = glz::object("class", &T::class_);
};
