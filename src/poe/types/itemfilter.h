// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>

#include <QString>

#include <glaze/glaze.hpp>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-ItemFilter

    struct ItemFilter
    {
        struct Validity
        {
            bool valid;                       // bool
            std::optional<QString> version;   // ?string; game version
            std::optional<QString> validated; // ?string; date time (ISO8601)
        };

        QString id;                    // string
        QString filter_name;           // string
        QString realm;                 // string
        QString description;           // string
        QString version;               // string
        QString type;                  // string; either Normal or Ruthless
        bool public_;                  // ?bool; always true if present
        std::optional<QString> filter; // ?string; not present when listing all filters
        // ?object; not present when listing all filters
        std::optional<poe::ItemFilter::Validity> validation;
    };

} // namespace poe

template<>
struct glz::meta<poe::ItemFilter>
{
    using T = poe::ItemFilter;
    static constexpr auto modify = glz::object("public", &T::public_);
};
