// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <utility>

#include <QString>

#include <glaze/glaze.hpp>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-ItemMod

    struct ItemMod
    {
        // The flag fields live in a base class so the custom reader below can
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
            // below), which arrives here as an engaged Flags with every field
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
        // NOTE: GGG sometimes sends an empty array; see the reader below.
        std::optional<ItemMod::Flags> flags; // ?object
    };

} // namespace poe

namespace glz {

    // GGG's backend emits "flags": [] rather than "flags": {} when a mod has no
    // flags set -- their serializer encodes an empty associative array as a
    // list. Glaze reads an object type strictly, so the bare struct rejects the
    // whole document. Accept both shapes: an empty array reads as a Flags with
    // nothing set, which is what it means.
    //
    // Confirmed with GGG's web developer at some point prior to 3.29.

    template<>
    struct from<JSON, poe::ItemMod::Flags>
    {
        template<auto Opts>
        static void op(poe::ItemMod::Flags &value, is_context auto &&ctx, auto &&it, auto &&end)
        {
            if (skip_ws<Opts>(ctx, it, end)) {
                return;
            }
            if (it != end && *it == '[') {
                ++it;
                if (skip_ws<Opts>(ctx, it, end)) {
                    return;
                }
                if (it == end || *it != ']') {
                    // Only the empty array is a known shape. A populated one
                    // means the wire format moved again; fail loudly rather
                    // than silently reading it as "no flags".
                    ctx.error = error_code::expected_bracket;
                    return;
                }
                ++it;
                value = {};
                return;
            }
            parse<JSON>::op<Opts>(static_cast<poe::ItemMod::FlagFields &>(value), ctx, it, end);
        }
    };

    // Specialized explicitly rather than left to reflection: items are
    // serialized through json::writeStash (test fixtures since F62 moved
    // the production cache to raw wire bytes), and Flags carries its
    // members in a base class.
    template<>
    struct to<JSON, poe::ItemMod::Flags>
    {
        template<auto Opts>
        static void op(const poe::ItemMod::Flags &value, auto &&...args)
        {
            serialize<JSON>::op<Opts>(static_cast<const poe::ItemMod::FlagFields &>(value),
                                      std::forward<decltype(args)>(args)...);
        }
    };

} // namespace glz
