// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include "poe/types/league.h"

#include <vector>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#accountleagues

    struct AccountLeagues
    {
        std::vector<poe::League> leagues;
    };

} // namespace poe
