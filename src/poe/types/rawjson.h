// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <string>

namespace poe {

    // A JSON value kept as the text it arrived in, for the parts of a reply
    // that are carried but never read (a stash tab's layout). It reads and
    // writes as glaze's raw_json does (poe/types/glaze_meta.h); it is a type
    // of its own so that a header with such a member does not include glaze.
    struct RawJson
    {
        std::string str;
    };

} // namespace poe
