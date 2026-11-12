// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "filters/filterspec.h"

#include <utility>

#include "buyoutmanager.h"
#include "filters/filtermatchers.h"
#include "item.h"
#include "itemcategories.h"

const QStringList &RarityChoices()
{
    static const QStringList choices{kAnyFilterChoice,
                                     "Normal",
                                     "Magic",
                                     "Rare",
                                     "Unique",
                                     "Unique (Foil)",
                                     "Any Non-Unique"};
    return choices;
}

FilterCatalog::FilterCatalog(std::vector<FilterSpec> specs)
    : m_specs(std::move(specs))
{}

FilterCatalog BuildFilterCatalog(const BuyoutManager &buyoutManager)
{
    using enum RefreshMode;

    const auto boolean =
        [](const char *caption, FilterGroup group, std::function<bool(const Item &)> predicate) {
            return FilterSpec{caption, group, Immediate, BoolPayload{std::move(predicate)}};
        };
    const auto text =
        [](const char *caption, FilterGroup group, std::function<QString(const Item &)> value) {
            return FilterSpec{caption, group, Debounced, TextPayload{std::move(value)}};
        };
    const auto combo = [](const char *caption,
                          FilterGroup group,
                          ComboMatchKind matchKind,
                          std::function<QStringList()> choices) {
        return FilterSpec{caption,
                          group,
                          Debounced,
                          ComboPayload{matchKind, kAnyFilterChoice, std::move(choices)}};
    };
    const auto colors = [](const char *caption, ColorsMatchKind matchKind) {
        return FilterSpec{caption, FilterGroup::Sockets, Immediate, ColorsPayload{matchKind}};
    };
    const auto minMax = [](const char *caption,
                           FilterGroup group,
                           std::function<double(const Item &)> value,
                           std::function<bool(const Item &)> present) {
        return FilterSpec{caption,
                          group,
                          Debounced,
                          MinMaxPayload{std::move(value), std::move(present)}};
    };
    const auto simpleProperty =
        [&minMax](const char *property, const char *caption, FilterGroup group) {
            const QString propertyName = QString::fromUtf8(property);
            return minMax(
                caption,
                group,
                [propertyName](const Item &item) {
                    // Present but not a number reads as 0, as before.
                    const auto &numeric = item.numeric_properties();
                    const auto found = numeric.find(propertyName);
                    return found == numeric.end() ? 0.0 : found->second;
                },
                [propertyName](const Item &item) { return item.properties().count(propertyName); });
        };
    const auto defaultProperty = [&minMax](const char *property,
                                           const char *caption,
                                           FilterGroup group,
                                           double defaultValue) {
        const QString propertyName = QString::fromUtf8(property);
        return minMax(
            caption,
            group,
            [propertyName, defaultValue](const Item &item) {
                const auto &numeric = item.numeric_properties();
                const auto found = numeric.find(propertyName);
                return found == numeric.end() ? defaultValue : found->second;
            },
            [](const Item &) { return true; });
    };
    const auto requiredStat =
        [&minMax](const char *property, const char *caption, FilterGroup group) {
            const QString propertyName = QString::fromUtf8(property);
            return minMax(
                caption,
                group,
                [propertyName](const Item &item) {
                    const auto &requirements = item.requirements();
                    const auto found = requirements.find(propertyName);
                    return found == requirements.end() ? 0.0 : static_cast<double>(found->second);
                },
                [](const Item &) { return true; });
        };
    const auto itemMethod = [&minMax](const char *caption,
                                      FilterGroup group,
                                      std::function<double(const Item &)> value) {
        return minMax(caption, group, std::move(value), [](const Item &) { return true; });
    };

    std::vector<FilterSpec> specs;
    specs.reserve(38);
    specs.push_back(text("Tab", FilterGroup::TopForm, [](const Item &item) {
        return item.location().GetHeader();
    }));
    specs.push_back(
        text("Name", FilterGroup::TopForm, [](const Item &item) { return item.PrettyName(); }));
    specs.push_back(combo("Category", FilterGroup::TopForm, ComboMatchKind::CategoryContains, [] {
        // The sentinel belongs to the filter, not to the item data: it is a
        // choice the form offers, and the matcher never sees it.
        QStringList choices{kAnyFilterChoice};
        choices.append(GetItemCategories());
        return choices;
    }));
    specs.push_back(combo("Rarity", FilterGroup::TopForm, ComboMatchKind::Rarity, [] {
        return RarityChoices();
    }));
    specs.push_back(simpleProperty("Critical Strike Chance", "Crit.", FilterGroup::Offense));
    specs.push_back(
        itemMethod("DPS", FilterGroup::Offense, [](const Item &item) { return item.DPS(); }));
    specs.push_back(
        itemMethod("pDPS", FilterGroup::Offense, [](const Item &item) { return item.pDPS(); }));
    specs.push_back(
        itemMethod("eDPS", FilterGroup::Offense, [](const Item &item) { return item.eDPS(); }));
    specs.push_back(
        itemMethod("cDPS", FilterGroup::Offense, [](const Item &item) { return item.cDPS(); }));
    specs.push_back(simpleProperty("Attacks per Second", "APS", FilterGroup::Offense));
    specs.push_back(simpleProperty("Armour", "Armour", FilterGroup::Defense));
    specs.push_back(simpleProperty("Evasion Rating", "Evasion", FilterGroup::Defense));
    specs.push_back(simpleProperty("Energy Shield", "Shield", FilterGroup::Defense));
    specs.push_back(simpleProperty("Chance to Block", "Block", FilterGroup::Defense));
    specs.push_back(itemMethod("Sockets", FilterGroup::Sockets, [](const Item &item) {
        return item.sockets_cnt();
    }));
    specs.push_back(itemMethod("Links", FilterGroup::Sockets, [](const Item &item) {
        return item.links_cnt();
    }));
    specs.push_back(colors("Colors", ColorsMatchKind::Sockets));
    specs.push_back(colors("Linked", ColorsMatchKind::Links));
    specs.push_back(requiredStat("Level", "R. Level", FilterGroup::Requirements));
    specs.push_back(requiredStat("Str", "R. Str", FilterGroup::Requirements));
    specs.push_back(requiredStat("Dex", "R. Dex", FilterGroup::Requirements));
    specs.push_back(requiredStat("Int", "R. Int", FilterGroup::Requirements));
    specs.push_back(defaultProperty("Quality", "Quality", FilterGroup::Misc, 0.0));
    specs.push_back(simpleProperty("Level", "Level", FilterGroup::Misc));
    specs.push_back(simpleProperty("Map Tier", "Map Tier", FilterGroup::Misc));
    specs.push_back(
        itemMethod("ilvl", FilterGroup::Misc, [](const Item &item) { return item.ilvl(); }));
    specs.push_back(boolean("Alt. art", FilterGroup::MiscFlags, MatchesAltart));
    specs.push_back(boolean("Priced", FilterGroup::MiscFlags, [&buyoutManager](const Item &item) {
        return buyoutManager.Get(item).IsActive();
    }));
    specs.push_back(boolean("Unidentified", FilterGroup::MiscFlags2, [](const Item &item) {
        return !item.identified();
    }));
    specs.push_back(boolean("Influenced", FilterGroup::MiscFlags2, [](const Item &item) {
        // Only the six conqueror/elder-war influences: Item::hasInfluence()
        // would also match fractured, synthesised, and eldritch items, which
        // have their own filters and are separate concepts in game (F38).
        return item.hasInfluence(Item::SHAPER) || item.hasInfluence(Item::ELDER)
               || item.hasInfluence(Item::CRUSADER) || item.hasInfluence(Item::REDEEMER)
               || item.hasInfluence(Item::HUNTER) || item.hasInfluence(Item::WARLORD);
    }));
    specs.push_back(boolean("Crafted", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.crafted();
    }));
    specs.push_back(boolean("Enchanted", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.enchanted();
    }));
    specs.push_back(boolean("Corrupted", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.corrupted();
    }));
    specs.push_back(boolean("Fractured", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.fractured();
    }));
    specs.push_back(
        boolean("Split", FilterGroup::MiscFlags2, [](const Item &item) { return item.split(); }));
    specs.push_back(boolean("Synthesized", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.synthesized();
    }));
    specs.push_back(boolean("Mutated", FilterGroup::MiscFlags2, [](const Item &item) {
        return item.mutated();
    }));
    specs.push_back(FilterSpec{"Mods", FilterGroup::Mods, Debounced, ModsPayload{}});
    return FilterCatalog(std::move(specs));
}
