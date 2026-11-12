// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include "poe/types/stashtab.h"

#include <vector>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#stashes-list

    struct AccountStashes
    {
        std::vector<poe::StashTab> stashes;
    };

} // namespace poe
