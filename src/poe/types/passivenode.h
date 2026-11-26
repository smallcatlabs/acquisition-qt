// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <QString>

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
            // either a JSON string or a JSON number (see poe/types/glaze_meta.h),
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
