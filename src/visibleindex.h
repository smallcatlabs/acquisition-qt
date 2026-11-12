// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

#include <cstddef>
#include <memory>
#include <set>
#include <unordered_map>

#include "item.h"

// A search's visible result by stable item id (M2 R6-3 reselection),
// rebuilt by every refilter and maintained per delta (M3 S4): inserting
// and removing one item is O(1), and the index answers as if freshly
// refiltered (deltaUpdatesVisibleIndexesIncrementally). It also keeps the
// visible item count the search caption shows.
class VisibleIndex
{
public:
    void Clear();
    void Insert(const std::shared_ptr<Item> &item);
    void Remove(const std::shared_ptr<Item> &item);

    // Global stable-identity lookup over the visible filtered result, so
    // reselection never scans the model and survives an item moving to
    // another tab mid-refresh. Returns null for unknown or empty ids.
    std::shared_ptr<Item> itemById(const QString &id) const
    {
        const auto it = m_by_id.find(id);
        return (it != m_by_id.end()) ? it->second : nullptr;
    }

    // True when the id index cannot represent every visible occurrence of
    // this id (M3 S2): a duplicated id (the index keeps the first
    // occurrence only — R6-3 reselection wants exactly one item) or the
    // empty id shared by id-less items. The buyout repaint falls back to
    // scanning every bucket for such ids.
    bool unindexed(const QString &id) const { return m_unindexed_ids.count(id) > 0; }

    // The visible items' stack sizes, summed.
    std::size_t itemCount() const { return m_item_count; }

private:
    // For a duplicated id the entry is the first occurrence — the
    // invariant is m_by_id[id] == m_duplicate_ids[id].front().
    std::unordered_map<QString, std::shared_ptr<Item>> m_by_id;

    // Ids the index above cannot fully represent: duplicated ids and the
    // empty id (see unindexed).
    std::set<QString> m_unindexed_ids;

    // Every visible occurrence of a currently-duplicated non-empty id
    // (mid-refresh divergence can show one id in two tabs). Kept so a
    // delta removing one occurrence restores the survivor to the id index
    // exactly. Empty except mid-refresh.
    std::unordered_map<QString, Items> m_duplicate_ids;
    // Visible items sharing the empty id; the unindexed set carries the
    // empty id while this is nonzero.
    std::size_t m_empty_id_count{0};

    std::size_t m_item_count{0};
};
