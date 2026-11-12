// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include "poe/types/character.h"

#include <optional>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#characters-get

    struct AccountCharacter
    {
        std::optional<poe::Character> character;
    };

} // namespace poe
