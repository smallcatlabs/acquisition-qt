// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <utility>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-ItemMod

    struct ItemMod
    {
        // The flag fields live in a base class so the custom reader (poe/types/glaze_meta.h) can
        // delegate to glaze's ordinary object parsing without recursing into
        // itself. Nothing should name this type directly -- use Flags.
        struct FlagFields
        {
            std::optional<bool> fractured;  // ?bool; always true if present
            std::optional<bool> mutated;    // ?bool; always true if present
            std::optional<bool> crafted;    // ?bool; always true if present
            std::optional<bool> desecrated; // ?bool; PoE2 only; always true if present
            std::optional<bool> vestigial;  // ?bool; PoE1 only; always true if present
            std::optional<bool> rareonly;   // ?bool; PoE1 only; always true if present
        };

        struct Flags : FlagFields
        {
            // GGG sends "flags": [] for a mod with nothing set (see the reader
            // in poe/types/glaze_meta.h), which arrives here as an engaged Flags with every field
            // empty -- indistinguishable from a mod that carried no flags at
            // all. Callers should branch on this rather than on whether the
            // optional is engaged.
            //
            // Whether any flag that says what kind of mod this is is set.
            // Item::LoadModifiers does not call this: it routes each line by
            // its flags itself (fractured, mutated, crafted and vestigial
            // lines get sections of their own), and a line whose flags it has
            // no branch for, including a flag field added to FlagFields
            // later, stays in the section of the array it arrived in.
            //
            // rareonly (3.30) is deliberately left out: it says which items a
            // mod can roll on, not what kind of mod it is.
            bool any() const
            {
                return fractured.value_or(false) || mutated.value_or(false)
                       || crafted.value_or(false) || desecrated.value_or(false)
                       || vestigial.value_or(false);
            }
        };

        QString description; // string
        // NOTE: GGG sometimes sends an empty array; see the reader.
        std::optional<ItemMod::Flags> flags; // ?object
    };

} // namespace poe
