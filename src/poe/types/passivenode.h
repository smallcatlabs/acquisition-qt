// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <QString>

#include <glaze/glaze.hpp>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PassiveNode

    struct PassiveNode
    {
        struct VariantEffect
        {
            unsigned effect;                                  // uint; effect hash
            std::vector<QString> stats;                       // array of string; stat descriptions
            std::optional<std::vector<QString>> reminderText; // ?array of string
        };

        struct Connection
        {
            // NOTE: documented as uint; modeled as a string like skill below and
            // the out/in node identifiers (Tom's ruling, 2026-11-12). Read from
            // either a JSON string or a JSON number (see the glz::meta below),
            // so neither shape fails a character fetch. Unverified until a 3.30
            // reply has been seen.
            QString to;     // uint; skill hash
            unsigned orbit; // uint; the orbit of this node around a point. 0 means a straight line
            std::optional<float> orbitX; // ?float; omitted if the orbit is 0
            std::optional<float> orbitY; // ?float; omitted if the orbit is 0
        };

        struct ClusterJewel
        {
            std::optional<unsigned> size;  // ?uint
            std::optional<unsigned> index; // ?uint
            // NOTE: TBD: actually a string.
            std::optional<QString> proxy; // ?uint; the proxy node identifier
            // NOTE: TBD: actually a string.
            std::optional<QString> parent; // ?uint; the parent node identifier
        };

        // NOTE: TODO: uint in docs, but string in result?
        std::optional<QString> skill;  // ?uint; skill hash
        std::optional<QString> name;   // ?string
        std::optional<QString> icon;   // ?string
        std::optional<bool> isNotable; // ?bool; always true if present
        std::optional<bool> isMastery; // ?bool; always true if present
        // ?string; active mastery or tattoo background image
        std::optional<QString> activeEffectImage;
        std::optional<bool> isTattoo;      // ?bool; always true if present
        std::optional<bool> isJewelSocket; // ?bool; always true if present
        // ?object; cluster jewel information
        std::optional<poe::PassiveNode::ClusterJewel> expansionJewel;
        std::optional<bool> isMultipleChoiceVariant; // ?bool; always true if present
        // ?array of object
        std::optional<std::vector<poe::PassiveNode::VariantEffect>> variantEffects;
        std::optional<std::vector<QString>> stats;        // ?array of string; stat descriptions
        std::optional<std::vector<QString>> reminderText; // ?array of string
        std::optional<QString> group;       // ?string; the key value to look up in the groups table
        std::optional<unsigned> orbit;      // ?uint; the orbit this node occupies within it's group
        std::optional<unsigned> orbitIndex; // ?uint; the index of this node in the group's orbit
        std::vector<QString> out; // array of string; node identifiers of nodes this one connects to
        std::vector<QString> in; // array of string; node identifiers of nodes connected to this one
        // ?array of object; edges from this node, including curves outside the group
        std::optional<std::vector<poe::PassiveNode::Connection>> connections;
    };

} // namespace poe

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
// see glz::meta<poe::Item> in poe/types/item.h). The comments are the reference's.
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
