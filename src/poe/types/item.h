// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QHashFunctions> // Needed to avoid errors instantiating std::hash<QString> for std::unordered_map.
#include <QString>

#include <optional>
#include <unordered_map>
#include <vector>

#include <glaze/glaze.hpp>

#include "poe/types/cruciblenode.h"
#include "poe/types/frametype.h"
#include "poe/types/itemmod.h"
#include "poe/types/itemproperty.h"
#include "poe/types/itemsocket.h"
#include "poe/types/mercenaryskill.h"

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#type-Item

    struct Item
    {
        struct Influences
        {
            std::optional<bool> shaper;
            std::optional<bool> elder;
            std::optional<bool> crusader;
            std::optional<bool> redeemer;
            std::optional<bool> hunter;
            std::optional<bool> warlord;
        };

        struct Rewards
        {
            QString label; // string
            // NOTE: TODO: WARNING: reward values (int) were formatted as a strings at one point.
            // Has this been fixed with 3.29?
            // dictionary of int; the key is a string representing the type of reward. The value is
            // the amount
            std::unordered_map<QString, int> rewards;
        };

        struct UltimatumMods
        {
            QString type;  // string; text used to display ultimatum icons
            unsigned tier; // uint
        };

        struct IncubatingInfo
        {
            QString name;      // string
            unsigned level;    // uint; monster level required to progress
            unsigned progress; // uint
            unsigned total;    // uint
        };

        struct ScourgedInfo
        {
            unsigned tier;                    // uint; 1-3 for items, 1-10 for maps
            std::optional<unsigned> level;    // ?uint; monster level required to progress
            std::optional<unsigned> progress; // ?uint
            std::optional<unsigned> total;    // ?uint
        };

        struct EnshroudedInfo
        {
            std::optional<QString> unique; // ?string
            unsigned progress;             // uint
            unsigned total;                // uint
        };

        struct CrucibleInfo
        {
            QString layout; // string; URL to an image of the tree layout
            // dictionary of CrucibleNode; the key is the string value of the node index
            std::unordered_map<QString, CrucibleNode> nodes;
        };

        struct HybridInfo
        {
            std::optional<bool> isVaalGem;                            // ?bool
            QString baseTypeName;                                     // string
            std::optional<std::vector<poe::ItemProperty>> properties; // ?array of ItemProperty
            std::optional<std::vector<QString>> explicitMods;         // ?array of string
            std::optional<QString> secDescrText;                      // ?string
        };

        std::optional<QString> realm;          // ?string; always poe2 if present
        bool verified;                         // bool
        unsigned w;                            // uint
        unsigned h;                            // uint
        QString icon;                          // string
        std::optional<bool> support;           // ?bool; always true if present
        std::optional<int> stackSize;          // ?int
        std::optional<int> maxStackSize;       // ?int
        std::optional<QString> stackSizeText;  // ?string
        std::optional<QString> iconStackLevel; // ?string; describes the level of a stack of items
        std::optional<QString> league;         // ?string
        std::optional<QString> id;             // ?string; a unique 64 digit hexadecimal string
        std::optional<poe::Item::Influences> influences; // ?object
        std::optional<bool> elder;                       // ?bool; always true if present
        std::optional<bool> shaper;                      // ?bool; always true if present
        std::optional<bool> searing;                     // ?bool; always true if present
        std::optional<bool> tangled;                     // ?bool; always true if present
        std::optional<bool> memoryItem;                  // ?bool; always true if present
        // ?bool; PoE1: true on Foulborn Uniques, PoE2: true on all Vaal Uniques
        std::optional<bool> mutated;
        std::optional<bool> vestigial;         // ?bool; PoE1 only; always true if present
        std::optional<QString> builtInSupport; // ?string; PoE1 only; Supported by level 1 x
        // ?int; PoE1 only; used for items that always display their monster level
        std::optional<int> monsterLevel;
        std::optional<bool> abyssJewel;                      // ?bool; always true if present
        std::optional<bool> delve;                           // ?bool; always true if present
        std::optional<bool> fractured;                       // ?bool; always true if present
        std::optional<bool> synthesised;                     // ?bool; always true if present
        std::optional<std::vector<poe::ItemSocket>> sockets; // ?array of ItemSocket
        std::optional<std::vector<poe::Item>> socketedItems; // ?array of Item
        QString name;                                        // string
        QString typeLine;                                    // string
        QString baseType;                                    // string
        std::optional<QString> rarity; // ?string; Normal, Magic, Rare, or Unique
        bool identified;               // bool
        std::optional<int> itemLevel;  // ?int; used for items that always display their item level
        std::optional<int> unidentifiedTier;                        // ?int
        int ilvl;                                                   // int
        std::optional<QString> note;                                // ?string; user-generated text
        std::optional<bool> duplicated;                             // ?bool; always true if present
        std::optional<bool> split;                                  // ?bool; always true if present
        std::optional<bool> corrupted;                              // ?bool; always true if present
        std::optional<bool> doubleCorrupted;                        // ?bool; always true if present
        std::optional<bool> unmodifiable;                           // ?bool; always true if present
        std::optional<bool> unmodifiableExceptChaos;                // ?bool; always true if present
        std::optional<std::vector<poe::ItemProperty>> properties;   // ?array of ItemProperty
        std::optional<std::vector<poe::ItemProperty>> requirements; // ?array of ItemProperty
        // ?array of ItemProperty
        std::optional<std::vector<poe::ItemProperty>> additionalProperties;
        // ?array of ItemProperty
        std::optional<std::vector<poe::ItemProperty>> nextLevelRequirements;
        std::optional<std::vector<poe::Item::Rewards>> rewards;             // ?array of object
        std::optional<QString> secDescrText;                                // ?string
        std::optional<std::vector<QString>> utilityMods;                    // ?array of string
        std::optional<std::vector<QString>> enchantMods;                    // ?array of string
        std::optional<std::vector<QString>> scourgeMods;                    // ?array of string
        std::optional<std::vector<poe::ItemMod>> implicitMods;              // ?array of ItemMod
        std::optional<std::vector<poe::Item::UltimatumMods>> ultimatumMods; // ?array of object
        std::optional<std::vector<poe::ItemMod>> explicitMods;              // ?array of ItemMod
        std::optional<std::vector<poe::MercenarySkill>> mercenarySkills; // ?array of MercenarySkill
        // ?array of string; only allocated mods are included
        std::optional<std::vector<QString>> crucibleMods;
        std::optional<std::vector<QString>> veiledMods; // ?array of string; random video identifier
        std::optional<bool> veiled;                     // ?bool; always true if present
        std::optional<bool> desecrated;                 // ?bool; PoE2 only; always true if present
        std::optional<QString> descrText;               // ?string
        std::optional<std::vector<QString>> flavourText; // ?array of string
        std::optional<bool> isRelic;                     // ?bool; always true if present
        std::optional<int> foilVariation;                // ?int
        std::optional<bool> replica;                     // ?bool; always true if present
        // NOTE: marked deprecated in GGG's 3.30 reference, without a changelog entry.
        std::optional<poe::Item::IncubatingInfo> incubatedItem; // ?object
        std::optional<poe::Item::EnshroudedInfo> enshrouded;    // ?object
        std::optional<poe::Item::ScourgedInfo> scourged;        // ?object
        std::optional<poe::Item::CrucibleInfo> crucible;        // ?object
        std::optional<poe::FrameType> frameType; // ?uint as FrameType; deprecated; use frameTypeId
        QString frameTypeId;                     // string
        std::optional<QString> artFilename;      // ?string
        std::optional<poe::Item::HybridInfo> hybrid; // ?object
        std::optional<unsigned> x;                   // ?uint
        std::optional<unsigned> y;                   // ?uint
        std::optional<QString> inventoryId;          // ?string
        std::optional<unsigned> socket;              // ?uint
        std::optional<QString> colour;               // ?string; PoE1 only; S, D, I, or G

        inline bool operator<(const Item &other) const
        {
            const QString a = name.isEmpty() ? typeLine : name + " " + typeLine;
            const QString b = other.name.isEmpty() ? other.typeLine
                                                   : other.name + " " + other.typeLine;
            return a < b;
        }
    };

} // namespace poe

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
