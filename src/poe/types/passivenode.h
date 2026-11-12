// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <optional>
#include <vector>

#include <QString>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-PassiveNode

    struct PassiveNode
    {
        struct Mastery
        {
            unsigned effect;                                  // uint effect hash
            std::vector<QString> stats;                       // array of string stat descriptions
            std::optional<std::vector<QString>> reminderText; // ? array of string
        };

        struct VariantEffect
        {
            unsigned effect;                                  // uint; effect hash
            std::vector<QString> stats;                       // array of string; stat descriptions
            std::optional<std::vector<QString>> reminderText; // ?array of string
        };

        struct Connection
        {
            // NOTE: documented as uint; modeled as a string like skill above
            // and the out/in node identifiers (Tom's ruling, 2026-11-12).
            QString to;     // uint; skill hash
            unsigned orbit; // uint; the orbit of this node around a point. 0 means a straight line
            std::optional<float> orbitX; // ?float; omitted if the orbit is 0
            std::optional<float> orbitY; // ?float; omitted if the orbit is 0
        };

        struct ClusterJewel
        {
            std::optional<unsigned> size;  // ? uint
            std::optional<unsigned> index; // ? uint
            std::optional<QString>
                proxy; // ? uint the proxy node identifier WARN: TBD: actually a string
            std::optional<QString>
                parent; // ? uint the parent node identifier WARN: TBD: actually a string
        };

        std::optional<QString> skill; // ? uint skill hash TODO: uint in docs, but string in result?
        std::optional<QString> name;  // ? string
        std::optional<QString> icon;  // ? string
        std::optional<bool> isKeystone;           // ? bool always true if present
        std::optional<bool> isNotable;            // ? bool always true if present
        std::optional<bool> isMastery;            // ? bool always true if present
        std::optional<QString> inactiveIcon;      // ? string inactive mastery image
        std::optional<QString> activeIcon;        // ? string active mastery image
        std::optional<QString> activeEffectImage; // ? string active mastery or tattoo background image
        std::optional<std::vector<poe::PassiveNode::Mastery>> masteryEffects; // ? array of object
        std::optional<bool> isBlighted;    // ? bool always true if present
        std::optional<bool> isTattoo;      // ? bool always true if present
        std::optional<bool> isProxy;       // ? bool always true if present
        std::optional<bool> isJewelSocket; // ? bool always true if present
        std::optional<poe::PassiveNode::ClusterJewel>
            expansionJewel; // ? object cluster jewel information
        std::optional<std::vector<QString>>
            recipe; // ? array of string components required for Blight crafting this node.
        // each string is one of ClearOil, SepiaOil, AmberOil, VerdantOil,
        // TealOil, AzureOil, IndigoOil, VioletOil, CrimsonOil,
        // BlackOil, OpalescentOil, SilverOil, GoldenOil, or PrismaticOil
        std::optional<unsigned> grantedStrength; // ? uint sum of stats on this node that grant strength
        std::optional<unsigned>
            grantedDexterity; // ? uint sum of stats on this node that grant dexterity
        std::optional<unsigned>
            grantedIntelligence; // ? uint sum of stats on this node that grant intelligence
        std::optional<QString> ascendancyName;       // ? string
        std::optional<bool> isAscendancyStart;       // ? bool always true if present
        std::optional<bool> isMultipleChoiceVariant; // ?bool; always true if present
        std::optional<std::vector<poe::PassiveNode::VariantEffect>> variantEffects; // ?array of object
        std::optional<bool> isMultipleChoice;             // ? bool always true if present
        std::optional<bool> isMultipleChoiceOption;       // ? bool always true if present
        std::optional<unsigned> grantedPassivePoints;     // ? uint
        std::optional<std::vector<QString>> stats;        // ? array of string stat descriptions
        std::optional<std::vector<QString>> reminderText; // ? array of string
        std::optional<std::vector<QString>> flavourText;  // ? array of string
        std::optional<unsigned> classStartIndex;          // ? uint
        std::optional<QString> group;       // ? string the key value to look up in the groups table
        std::optional<unsigned> orbit;      // ? uint the orbit this node occupies within it's group
        std::optional<unsigned> orbitIndex; // ? uint the index of this node in the group's orbit
        std::vector<QString> out; // array of string node identifiers of nodes this one connects to
        std::vector<QString> in;  // array of string node identifiers of nodes connected to this one
        // ?array of object; edges from this node, including curves outside the group
        std::optional<std::vector<poe::PassiveNode::Connection>> connections;
    };

} // namespace poe
