// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// SPIKE(S1-M2): deterministic large-collection generator for the D9 cadence
// spike. Items are described by compact specs (so a 1m collection does not
// hold a million poe::Item structs) and materialized on demand; the same
// (config, seed) always produces the same collection and the same churn
// sequence, which is what lets M2-M2 reuse these datasets as fixed recorded
// reply/removal shapes (R7-3).

#pragma once

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "item.h"
#include "itemlocation.h"
#include "poe/types/item.h"
#include "poe/types/stashtab.h"

class SpikeDataset
{
public:
    struct Config
    {
        int tab_count = 2000;
        int mean_items_per_tab = 50;
        // Fraction of quad tabs (24x24, 576-item cap). A ~1m-item account is
        // realistically quad-heavy; the default matches a typical mix.
        double quad_share = 0.1;
        uint32_t seed = 20260729;
    };

    struct ItemSpec
    {
        uint64_t serial = 0; // stable identity: survives churn "keep"/"modify"
        int name_index = 0;
        int base_index = 0;
        int frame_type = 0; // 0..3
        int ilvl = 1;
        int x = 0;
        int y = 0;
        int quality = 0; // 0 = no Quality property
    };

    struct TabSpec
    {
        poe::StashTab stash;
        std::vector<ItemSpec> items;
    };

    explicit SpikeDataset(const Config &config)
        : m_config(config)
        , m_rng(config.seed)
    {
        m_tabs.reserve(static_cast<size_t>(config.tab_count));
        // Tab sizes: mostly sparse-to-normal tabs with a tail of dense quads,
        // scaled so the mean lands near mean_items_per_tab.
        std::lognormal_distribution<double> size_dist(0.0, 0.9);
        for (int t = 0; t < config.tab_count; ++t) {
            TabSpec tab;
            tab.stash.id = QString("spiketab%1").arg(t, 10, 16, QChar('0'));
            tab.stash.name = QString("%1 %2")
                                 .arg(kTabNames[static_cast<size_t>(t) % kTabNames.size()])
                                 .arg(t);
            const bool quad = (t % 100) < static_cast<int>(config.quad_share * 100.0);
            tab.stash.type = quad ? "QuadStash" : "PremiumStash";
            tab.stash.index = static_cast<unsigned>(t);
            tab.stash.metadata.colour = QString("%1").arg((t * 37) % 0xffffff, 6, 16, QChar('0'));
            const int cap = quad ? 576 : 144;
            const double scale = static_cast<double>(config.mean_items_per_tab)
                                 / std::exp(0.9 * 0.9 / 2.0);
            int count = static_cast<int>(size_dist(m_rng) * scale * (quad ? 2.0 : 1.0));
            count = std::clamp(count, 1, cap);
            tab.items.reserve(static_cast<size_t>(count));
            for (int n = 0; n < count; ++n) {
                tab.items.push_back(MakeSpec(quad));
            }
            m_tabs.push_back(std::move(tab));
        }
    }

    int tabCount() const { return static_cast<int>(m_tabs.size()); }

    qsizetype totalItems() const
    {
        qsizetype total = 0;
        for (const auto &tab : m_tabs) {
            total += static_cast<qsizetype>(tab.items.size());
        }
        return total;
    }

    QString tabName(int tab_index) const
    {
        return m_tabs[static_cast<size_t>(tab_index)].stash.name;
    }

    ItemLocation location(int tab_index) const
    {
        return ItemLocation(m_tabs[static_cast<size_t>(tab_index)].stash);
    }

    std::vector<ItemLocation> locations() const
    {
        std::vector<ItemLocation> result;
        result.reserve(m_tabs.size());
        for (int t = 0; t < tabCount(); ++t) {
            result.push_back(location(t));
        }
        return result;
    }

    Items tabItems(int tab_index) const
    {
        const TabSpec &tab = m_tabs[static_cast<size_t>(tab_index)];
        const ItemLocation loc = location(tab_index);
        Items items;
        items.reserve(tab.items.size());
        for (const ItemSpec &spec : tab.items) {
            items.push_back(std::make_shared<Item>(Materialize(spec), loc));
        }
        return items;
    }

    Items allItems() const
    {
        Items items;
        items.reserve(static_cast<size_t>(totalItems()));
        for (int t = 0; t < tabCount(); ++t) {
            const Items tab_items = tabItems(t);
            items.insert(items.end(), tab_items.begin(), tab_items.end());
        }
        return items;
    }

    // One simulated per-tab fetch: a fraction of the tab's items is removed,
    // a fraction is modified in place (same id, changed fields), the rest is
    // kept verbatim, and roughly as many new items arrive as were removed.
    // Every returned Item is a NEW object — exactly what an M2 delta does —
    // so pointer identity breaks while stable identity survives.
    Items ChurnTab(int tab_index, double churn)
    {
        TabSpec &tab = m_tabs[static_cast<size_t>(tab_index)];
        const bool quad = tab.stash.type == "QuadStash";
        std::uniform_real_distribution<double> roll(0.0, 1.0);
        std::vector<ItemSpec> next;
        next.reserve(tab.items.size());
        int removed = 0;
        for (ItemSpec &spec : tab.items) {
            const double r = roll(m_rng);
            if (r < churn / 2.0) {
                ++removed; // removed by this fetch
            } else if (r < churn) {
                ItemSpec modified = spec; // same serial: modified in place
                modified.ilvl = std::clamp(modified.ilvl + 1, 1, 86);
                modified.quality = (modified.quality + 5) % 30;
                next.push_back(modified);
            } else {
                next.push_back(spec);
            }
        }
        for (int n = 0; n < removed; ++n) {
            next.push_back(MakeSpec(quad));
        }
        tab.items = std::move(next);
        return tabItems(tab_index);
    }

    void RenameTab(int tab_index, const QString &new_name)
    {
        m_tabs[static_cast<size_t>(tab_index)].stash.name = new_name;
    }

private:
    ItemSpec MakeSpec(bool quad)
    {
        const int grid = quad ? 24 : 12;
        std::uniform_int_distribution<int> name_dist(0, static_cast<int>(kPrefixes.size()) - 1);
        std::uniform_int_distribution<int> base_dist(0, static_cast<int>(kBases.size()) - 1);
        std::uniform_int_distribution<int> frame_dist(0, 99);
        std::uniform_int_distribution<int> ilvl_dist(1, 86);
        std::uniform_int_distribution<int> pos_dist(0, grid - 1);
        std::uniform_int_distribution<int> quality_dist(0, 29);

        ItemSpec spec;
        spec.serial = m_next_serial++;
        spec.name_index = name_dist(m_rng);
        spec.base_index = base_dist(m_rng);
        const int frame_roll = frame_dist(m_rng);
        spec.frame_type = (frame_roll < 35) ? 0 : (frame_roll < 65) ? 1 : (frame_roll < 92) ? 2 : 3;
        spec.ilvl = ilvl_dist(m_rng);
        spec.x = pos_dist(m_rng);
        spec.y = pos_dist(m_rng);
        spec.quality = quality_dist(m_rng) < 10 ? quality_dist(m_rng) : 0;
        return spec;
    }

    poe::Item Materialize(const ItemSpec &spec) const
    {
        static const std::array<const char *, 4> kFrameIds
            = {"Normal", "Magic", "Rare", "Unique"};

        poe::Item item;
        item.verified = false;
        item.w = 1;
        item.h = 1;
        item.icon = "https://web.poecdn.com/image/spike.png";
        item.id = QString("spikeitem%1").arg(spec.serial, 16, 16, QChar('0'));
        item.baseType = kBases[static_cast<size_t>(spec.base_index)];
        item.typeLine = item.baseType;
        if (spec.frame_type >= 2) {
            item.name = QString("%1 %2")
                            .arg(kPrefixes[static_cast<size_t>(spec.name_index)],
                                 kSuffixes[static_cast<size_t>(spec.serial % kSuffixes.size())]);
        }
        item.identified = true;
        item.ilvl = spec.ilvl;
        item.frameType = static_cast<poe::FrameType>(spec.frame_type);
        item.frameTypeId = kFrameIds[static_cast<size_t>(spec.frame_type)];
        item.x = static_cast<unsigned>(spec.x);
        item.y = static_cast<unsigned>(spec.y);
        if (spec.quality > 0) {
            poe::ItemProperty quality;
            quality.name = "Quality";
            quality.values.emplace_back(QString("+%1%").arg(spec.quality), 1);
            item.properties = std::vector<poe::ItemProperty>{quality};
        }
        return item;
    }

    static constexpr std::array<const char *, 16> kPrefixes
        = {"Blazing", "Frozen",  "Storm",  "Ancient", "Gloom",  "Iron",    "Vile",   "Sacred",
           "Dread",   "Whisper", "Raging", "Hollow",  "Karui",  "Maraketh", "Vaal",  "Eternal"};
    static constexpr std::array<const char *, 12> kSuffixes
        = {"Bane",  "Song",   "Roar",  "Bite", "Whorl", "Grasp",
           "Brand", "Beacon", "Pledge", "Mark", "Ward",  "Call"};
    static constexpr std::array<const char *, 18> kBases
        = {"Sword",   "Axe",    "Bow",     "Shield", "Amulet",     "Ring",
           "Helmet",  "Boots",  "Gloves",  "Wand",   "Sceptre",    "Chestplate",
           "Quiver",  "Belt",   "Dagger",  "Claw",   "Staff",      "Jewel"};
    static constexpr std::array<const char *, 10> kTabNames
        = {"Currency", "Maps",  "Gear",   "Uniques", "Essence",
           "Fossils",  "Cards", "Chaos",  "Dump",    "Trade"};

    Config m_config;
    std::mt19937 m_rng;
    uint64_t m_next_serial = 1;
    std::vector<TabSpec> m_tabs;
};
