// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include "poe/types/stashtab.h"

#include <optional>

namespace libpoe {

    // https://www.pathofexile.com/developer/docs/reference#stashes-get

    struct AccountStashes
    {
        std::optional<poe::StashTab> stash;
    };

} // namespace libpoe
