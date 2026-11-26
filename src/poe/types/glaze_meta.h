// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

// How the wire types (poe/types/) differ from the JSON they are read from and
// written to: the keys that are C++ keywords, the keys of the reference that
// nothing reads, and the values GGG sends in more than one shape. The structs
// themselves are plain data and their headers do not include glaze.
//
// Do not include this file. Include "util/json_glaze.h", the one way into
// glaze, which brings this with it: a file that used glaze on one of these
// types without these declarations would compile, and read and write other
// JSON (a "class_" key, a refused "flags": []). The glaze_boundary test holds
// that line.

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include <QString>

#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/itemfilter.h"
#include "poe/types/itemjeweldata.h"
#include "poe/types/itemmod.h"
#include "poe/types/ladderentry.h"
#include "poe/types/passivegroup.h"
#include "poe/types/passivenode.h"
#include "poe/types/publicstashchange.h"
#include "poe/types/pvpladderteammember.h"
#include "poe/types/rawjson.h"
#include "poe/types/stashtab.h"
#include "poe/types/website/webcharacter.h"
#include "poe/types/website/webstashtab.h"
#include "util/glaze_qt.h"

namespace glz {

    // poe::RawJson is glaze's raw_json under a name that needs no glaze.

    template<>
    struct from<JSON, poe::RawJson>
    {
        template<auto Opts>
        static void op(poe::RawJson &value, auto &&...args)
        {
            glz::raw_json raw;
            parse<JSON>::op<Opts>(raw, std::forward<decltype(args)>(args)...);
            value.str = std::move(raw.str);
        }
    };

    template<>
    struct to<JSON, poe::RawJson>
    {
        template<auto Opts>
        static void op(const poe::RawJson &value, auto &&...args)
        {
            serialize<JSON>::op<Opts>(glz::raw_json_view{value.str},
                                      std::forward<decltype(args)>(args)...);
        }
    };

} // namespace glz

// ----- poe::Item -----

// The fields of the reference that nothing reads are not modeled (#422): a
// known key in another shape than its member's would refuse the whole reply.
// Their keys are named here, so that a reply carrying one is read whatever
// the value's shape, and the strict read of the payload corpus
// (tst_payloads) still knows them. The comments are the reference's.
template<>
struct glz::meta<poe::Item>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "iconTierText", glz::skip{}, // ?string; usually roman numerals
        "gemSockets", glz::skip{}, // ?array of string; PoE2 only; string is always W
        "forum_note", glz::skip{}, // ?string; user-generated text
        "lockedToCharacter", glz::skip{}, // ?bool; always true if present
        "lockedToAccount", glz::skip{}, // ?bool; always true if present
        "sanctified", glz::skip{}, // ?bool; PoE2 only; always true if present
        "cisRaceReward", glz::skip{}, // ?bool; always true if present
        "seaRaceReward", glz::skip{}, // ?bool; always true if present
        "thRaceReward", glz::skip{}, // ?bool; always true if present
        "notableProperties", glz::skip{}, // ?array of ItemProperty
        "weaponRequirements", glz::skip{}, // ?array of ItemProperty; PoE2 only
        "supportGemRequirements", glz::skip{}, // ?array of ItemProperty; PoE2 only
        "tamedBeastProperties", glz::skip{}, // ?array of ItemProperty; PoE2 only
        "grantedSkills", glz::skip{}, // ?array of ItemProperty; PoE2 only
        "logbookMods", glz::skip{}, // ?array of object
        "runeMods", glz::skip{}, // ?array of string; PoE2 only
        "bondedMods", glz::skip{}, // ?array of string; PoE2 only
        "cosmeticMods", glz::skip{}, // ?array of string
        "gemTabs", glz::skip{}, // ?array of GemTab; PoE2 only
        "gemBackground", glz::skip{}, // ?string; PoE2 only
        "gemSkill", glz::skip{}, // ?string; PoE2 only
        "foreseeing", glz::skip{}, // ?bool; always true if present
        "ruthless", glz::skip{}, // ?bool; always true if present
        "extended", glz::skip{}, // ?object; only present in the Public Stash API
        "socketedIcon", glz::skip{}, // ?string; PoE2 only; an image URL to use when this item is in the socket of another item
        "flavourTextNote", glz::skip{}, // ?string; user-generated text
        "prophecyText", glz::skip{} // ?string
    );
    // clang-format on
};

// ----- poe::ItemMod -----

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

// ----- poe::ItemJewelData -----

// Fields of the reference that nothing reads, skipped whatever their shape (#422;
// see glz::meta<poe::Item> above). The comments are the reference's.
template<>
struct glz::meta<poe::ItemJewelData>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "radiusMin", glz::skip{}, // ?uint
        "radiusVisual", glz::skip{} // ?string
    );
    // clang-format on
};

// ----- poe::PassiveGroup -----

// Fields of the reference that nothing reads, skipped whatever their shape (#422;
// see glz::meta<poe::Item> above). The comments are the reference's.
template<>
struct glz::meta<poe::PassiveGroup>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "isProxy", glz::skip{} // ?bool; always true if present
    );
    // clang-format on
};

// ----- poe::PassiveNode -----

// connections[].to is documented as a number and modeled as a string; accept
// both, so a reply in the documented shape does not fail the whole character
// read. Written back as a string.
template<>
struct glz::meta<poe::PassiveNode::Connection>
{
    using T = poe::PassiveNode::Connection;
    static constexpr auto read_to = [](T &self, const std::variant<std::uint64_t, std::string> &to) {
        if (const auto *number = std::get_if<std::uint64_t>(&to)) {
            self.to = QString::number(*number);
        } else {
            self.to = QString::fromStdString(std::get<std::string>(to));
        }
    };
    static constexpr auto write_to = [](const T &self) -> const QString & { return self.to; };
    static constexpr auto modify = glz::object("to", glz::custom<read_to, write_to>);
};

// Fields of the reference that nothing reads, skipped whatever their shape (#422;
// see glz::meta<poe::Item> above). The comments are the reference's.
template<>
struct glz::meta<poe::PassiveNode>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "isKeystone", glz::skip{}, // ?bool; always true if present
        "inactiveIcon", glz::skip{}, // ?string; inactive mastery image
        "activeIcon", glz::skip{}, // ?string; active mastery image
        "masteryEffects", glz::skip{}, // ?array of object
        "isBlighted", glz::skip{}, // ?bool; always true if present
        "isProxy", glz::skip{}, // ?bool; always true if present
        "recipe", glz::skip{}, // ?array of string; components required for Blight crafting this node
        "grantedStrength", glz::skip{}, // ?uint; sum of stats on this node that grant strength
        "grantedDexterity", glz::skip{}, // ?uint; sum of stats on this node that grant dexterity
        "grantedIntelligence", glz::skip{}, // ?uint; sum of stats on this node that grant intelligence
        "ascendancyName", glz::skip{}, // ?string
        "isAscendancyStart", glz::skip{}, // ?bool; always true if present
        "isMultipleChoice", glz::skip{}, // ?bool; always true if present
        "isMultipleChoiceOption", glz::skip{}, // ?bool; always true if present
        "grantedPassivePoints", glz::skip{}, // ?uint
        "flavourText", glz::skip{}, // ?array of string
        "classStartIndex", glz::skip{} // ?uint
    );
    // clang-format on
};

// ----- poe::Character -----

// Fields of the reference that nothing reads, skipped whatever their shape (#422;
// see glz::meta<poe::Item> above). The comments are the reference's.
template<>
struct glz::meta<poe::Character>
{
    using T = poe::Character;
    // clang-format off
    static constexpr auto modify = glz::object(
        "class", &T::class_,
        "active_mercenary_index", glz::skip{} // ?int; PoE1 only
    );
    // clang-format on
};

template<>
struct glz::meta<poe::Character::Passives>
{
    // clang-format off
    static constexpr auto modify = glz::object(
        "alternate_ascendancy", glz::skip{} // ?string; PoE1 only; Bloodline class name
    );
    // clang-format on
};

// ----- poe::StashTab -----

template<>
struct glz::meta<poe::StashTab::Metadata>
{
    using T = poe::StashTab::Metadata;
    static constexpr auto modify = glz::object("public", &T::public_);
};

// ----- poe::ItemFilter -----

template<>
struct glz::meta<poe::ItemFilter>
{
    using T = poe::ItemFilter;
    static constexpr auto modify = glz::object("public", &T::public_);
};

// ----- poe::LadderEntry -----

template<>
struct glz::meta<poe::LadderEntry::DelveInfo>
{
    using T = poe::LadderEntry::DelveInfo;
    static constexpr auto modify = glz::object("default", &T::default_);
};

template<>
struct glz::meta<poe::LadderEntry::Character>
{
    using T = poe::LadderEntry::Character;
    static constexpr auto modify = glz::object("class", &T::class_);
};

template<>
struct glz::meta<poe::LadderEntry>
{
    using T = poe::LadderEntry;
    static constexpr auto modify = glz::object("public", &T::public_);
};

// ----- poe::PublicStashChange -----

template<>
struct glz::meta<poe::PublicStashChange>
{
    using T = poe::PublicStashChange;
    static constexpr auto modify = glz::object("public", &T::public_);
};

// ----- poe::PvPLadderTeamMember -----

template<>
struct glz::meta<poe::PvPLadderTeamMember::Character>
{
    using T = poe::PvPLadderTeamMember::Character;
    static constexpr auto modify = glz::object("class", &T::class_);
};

template<>
struct glz::meta<poe::PvPLadderTeamMember>
{
    using T = poe::PvPLadderTeamMember;
    static constexpr auto modify = glz::object("public", &T::public_);
};

// ----- poe::WebCharacter -----

template<>
struct glz::meta<poe::WebCharacter>
{
    using T = poe::WebCharacter;
    static constexpr auto modify = glz::object("class", &T::class_);
};
