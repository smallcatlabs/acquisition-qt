// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

#include <glaze/glaze.hpp>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PassiveGroup

    struct PassiveGroup
    {
        float x;                      // float
        float y;                      // float
        std::vector<unsigned> orbits; // array of uint
        std::optional<QString> proxy; // ?string; identifier of the placeholder node
        // array of string; the node identifiers associated with this group
        std::vector<QString> nodes;
    };

} // namespace poe

// Fields of the reference that nothing reads, skipped whatever their shape (#422;
// see glz::meta<poe::Item> in poe/types/item.h). The comments are the reference's.
template<>
struct glz::meta<poe::PassiveGroup>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "isProxy", glz::skip{} // ?bool; always true if present
    );
    // clang-format on
};
