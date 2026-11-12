// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include "poe/types/character.h"

#include <vector>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#characters-list

    struct AccountCharacters
    {
        std::vector<poe::Character> characters;
    };

} // namespace poe
