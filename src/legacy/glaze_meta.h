// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

// The keys of the legacy types (legacy/) that are C++ keywords.
//
// Do not include this file. Include "util/json_glaze.h"; see
// poe/types/glaze_meta.h for why.

#include "legacy/legacycharacter.h"
#include "legacy/legacystash.h"
#include "util/glaze_qt.h"

template<>
struct glz::meta<LegacyCharacter>
{
    using T = LegacyCharacter;
    static constexpr auto modify = glz::object("class", &T::class_);
};

template<>
struct glz::meta<LegacyStash::Metadata>
{
    using T = LegacyStash::Metadata;
    static constexpr auto modify = glz::object("public", &T::public_);
};
