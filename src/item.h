// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QHashFunctions> // Needed to avoid obscure errors in std::unordered_map with QString keys.
#include <QString>

#include <array>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "itemlocation.h"
#include "poe/types/item.h"
#include "poe/types/itemproperty.h"
#include "poe/types/itemsocket.h"

namespace poe {

    struct Item;

}

struct ItemSocketGroup
{
    int r, g, b, w;
};

struct ItemPropertyValue
{
    QString str;
    int type;
};

struct ItemProperty
{
    QString name;
    std::vector<ItemPropertyValue> values;
    int display_mode;
};

struct ItemRequirement
{
    QString name;
    ItemPropertyValue value;
};

struct ItemSocket
{
    unsigned char group;
    char attr;
};

typedef std::vector<QString> ItemMods;
typedef std::unordered_map<QString, double> ModTable;

class Item
{
public:
    typedef const std::unordered_map<QString, QString> CategoryReplaceMap;

    enum INFLUENCE_TYPES {
        NONE,
        SHAPER,
        ELDER,
        CRUSADER,
        REDEEMER,
        HUNTER,
        WARLORD,
        SYNTHESISED,
        FRACTURED,
        SEARING_EXARCH,
        EATER_OF_WORLDS
    };

    Item(const poe::Item &item, const ItemLocation &base_location);

    QString id() const { return m_uid; }
    QString name() const { return m_name; }
    QString typeLine() const { return m_typeLine; }
    QString PrettyName() const;
    bool identified() const { return m_identified; }
    bool corrupted() const { return m_corrupted; }
    bool crafted() const { return m_crafted; }
    bool enchanted() const { return m_enchanted; }
    bool fractured() const { return m_fractured; }
    bool split() const { return m_split; }
    bool synthesized() const { return m_synthesized; }
    bool mutated() const { return m_mutated; }
    bool hasInfluence(INFLUENCE_TYPES type) const
    {
        return std::find(m_influenceList.begin(), m_influenceList.end(), type)
               != m_influenceList.end();
    }
    INFLUENCE_TYPES influenceLeft() const
    {
        return m_influenceList.size() == 0 ? NONE : m_influenceList[0];
    }
    INFLUENCE_TYPES influenceRight() const
    {
        return m_influenceList.size() == 0   ? NONE
               : m_influenceList.size() == 1 ? m_influenceList[0]
                                             : m_influenceList[1];
    }
    bool hasInfluence() const { return !m_influenceList.empty(); }
    int w() const { return m_w; }
    int h() const { return m_h; }
    int frameType() const { return m_frameType; }
    const QString &icon() const { return m_icon; }
    // Property values as the game displays them ("8.00%", "35-120"), for
    // columns and other display; Quality and Level are stored stripped.
    const std::map<QString, QString> &properties() const { return m_properties; }
    // The properties whose value is a single number, parsed: a leading "+"
    // and a trailing "%" are dropped. Ranges and other non-numbers are absent.
    // A catalyst's "Quality (<kind>)" also counts as "Quality" here unless the
    // item carries a plain Quality property.
    const std::map<QString, double> &numeric_properties() const { return m_numeric_properties; }
    const std::vector<ItemProperty> &text_properties() const { return m_text_properties; }
    const std::vector<ItemRequirement> &text_requirements() const { return m_text_requirements; }
    const std::map<QString, ItemMods> &text_mods() const { return m_text_mods; }
    const std::vector<ItemSocket> &text_sockets() const { return m_text_sockets; }
    const QString &hash_v4() const { return m_hash; }
    const QString &old_hash() const { return m_old_hash; }
    const std::vector<std::pair<QString, int>> &elemental_damage() const
    {
        return m_elemental_damage;
    }
    // Requirement values by name, with "Strength", "Dexterity" and
    // "Intelligence" stored under "Str", "Dex" and "Int".
    const std::map<QString, int> &requirements() const { return m_requirements; }
    double DPS() const;
    double pDPS() const;
    double eDPS() const;
    double cDPS() const;
    int sockets_cnt() const { return m_sockets_cnt; }
    int links_cnt() const { return m_links_cnt; }
    const ItemSocketGroup &sockets() const { return m_sockets; }
    const std::vector<ItemSocketGroup> &socket_groups() const { return m_socket_groups; }
    const ItemLocation &location() const { return m_location; }
    // Rebase the embedded location onto fresh tab metadata after a tab-list
    // reconciliation. hash_v4() is deliberately not recomputed: it folds the
    // tab label, but it is only consumed by the one-time startup buyout
    // migration, which runs before any refresh can rebase a location.
    void RebaseLocation(const ItemLocation &fresh_tab) { m_location.rebaseTabMetadata(fresh_tab); }
    const QString &note() const { return m_note; }
    const QString &category() const { return m_category; }
    int count() const { return m_count; }
    const ModTable &mod_table() const { return m_mod_table; }
    int ilvl() const { return m_ilvl; }
    bool operator<(const Item &other) const;
    bool Wearable() const;
    QString POBformat() const;
    static const size_t k_CategoryLevels = 3;
    static const std::array<CategoryReplaceMap, k_CategoryLevels> m_replace_map;

private:
    void LoadInfluences(const poe::Item &item);
    void LoadModifiers(const poe::Item &item);
    void LoadProperties(const std::vector<poe::ItemProperty> &properties);
    void LoadRequirements(const std::vector<poe::ItemProperty> &requirements);
    void LoadSockets(const std::vector<poe::ItemSocket> &sockets);
    void CalculateCategories();
    void CalculateHash(const poe::Item &json);

    QString m_name;
    ItemLocation m_location;
    QString m_typeLine;
    QString m_baseType;
    QString m_category;
    bool m_identified{true};
    bool m_corrupted{false};
    bool m_crafted{false};
    bool m_enchanted{false};
    bool m_fractured{false};
    bool m_split{false};
    bool m_synthesized{false};
    bool m_mutated{false};
    std::vector<INFLUENCE_TYPES> m_influenceList;
    int m_w{0};
    int m_h{0};
    int m_frameType{-1};
    QString m_frameTypeId;
    QString m_icon;
    std::map<QString, QString> m_properties;
    std::map<QString, double> m_numeric_properties;
    QString m_old_hash;
    QString m_hash;
    // vector of pairs [damage, type]
    std::vector<std::pair<QString, int>> m_elemental_damage;
    int m_sockets_cnt{0};
    int m_links_cnt{0};
    ItemSocketGroup m_sockets{0, 0, 0, 0};
    std::vector<ItemSocketGroup> m_socket_groups;
    std::map<QString, int> m_requirements;
    int m_count{1};
    int m_ilvl{0};
    std::vector<ItemProperty> m_text_properties;
    std::vector<ItemRequirement> m_text_requirements;
    std::map<QString, ItemMods> m_text_mods;
    std::vector<ItemSocket> m_text_sockets;
    QString m_note;
    ModTable m_mod_table;
    QString m_uid;
};

typedef std::vector<std::shared_ptr<Item>> Items;
