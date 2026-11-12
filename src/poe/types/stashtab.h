// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

#include <glaze/glaze.hpp>

#include "poe/types/item.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-StashTab

    struct StashTab
    {
        struct Metadata
        {
            std::optional<bool> public_; // ?bool; always true if present
            std::optional<bool> folder;  // ?bool; always true if present
            // NOTE: WARNING: The colour field can sometimes be 2 or 4 characters (which means it
            // needs to be zero-padded)
            std::optional<QString> colour; // ?string; 6 digit hex colour
            // NOTE: TODO: layout and items are not in GGG's reference.
            std::optional<glz::raw_json> layout;
            std::optional<unsigned> items;
            std::optional<glz::raw_json> map; // ?object; various game specific properties
        };

        QString id;                       // string; a 10 digit hexadecimal string
        std::optional<QString> parent;    // ?string; a 10 digit hexadecimal string
        std::optional<QString> folder;    // ?string; a 10 digit hexadecimal string
        QString name;                     // string
        QString type;                     // string
        std::optional<unsigned> index;    // ?uint
        poe::StashTab::Metadata metadata; // object
        std::optional<std::vector<poe::StashTab>> children; // ?array of StashTab
        std::optional<std::vector<poe::Item>> items;        // ?array of Item

        inline bool operator<(const StashTab &other) const
        {
            const unsigned a = index.value_or(0);
            const unsigned b = other.index.value_or(0);
            return a < b;
        }
    };

    struct StashListWrapper
    {
        std::vector<poe::StashTab> stashes;
    };

    // What a stash fetch returns across the typed boundary (F62): the parsed
    // payload plus the exact wire bytes of the reply's "stash" sub-object it
    // was parsed from. The bytes are what the datastore persists — the cache
    // must be a faithful record of the API, and a re-serialization of the
    // parsed struct silently drops every field Acquisition does not model.
    // Both members are unconditional: a reply whose sub-object is missing or
    // null is a facade Parse error (M2 D5/R2-4), never a success, so a
    // payload that exists always holds a stash and the bytes it was parsed
    // from. Not a wire type and never serialized; the wire shape lives with
    // the reader (json_readers.cpp).
    struct StashPayload
    {
        poe::StashTab stash;
        QByteArray bytes;
    };

}; // namespace poe

template<>
struct glz::meta<poe::StashTab::Metadata>
{
    using T = poe::StashTab::Metadata;
    static constexpr auto modify = glz::object("public", &T::public_);
};
