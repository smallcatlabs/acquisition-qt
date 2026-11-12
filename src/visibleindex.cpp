// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "visibleindex.h"

#include <algorithm>

void VisibleIndex::Clear()
{
    m_by_id.clear();
    m_unindexed_ids.clear();
    m_duplicate_ids.clear();
    m_empty_id_count = 0;
    m_item_count = 0;
}

void VisibleIndex::Insert(const std::shared_ptr<Item> &item)
{
    m_item_count += item->count();
    const QString id = item->id();
    if (id.isEmpty()) {
        // Id-less items cannot be identity-tracked; the unindexed mark
        // sends consumers down the every-occurrence path.
        ++m_empty_id_count;
        m_unindexed_ids.insert(id);
        return;
    }
    const auto [it, inserted] = m_by_id.emplace(id, item);
    if (!inserted) {
        // Mid-refresh divergence: one id visible twice. Track every
        // occurrence so a removal restores the survivor exactly.
        auto &occurrences = m_duplicate_ids[id];
        if (occurrences.empty()) {
            occurrences.push_back(it->second);
        }
        occurrences.push_back(item);
        m_unindexed_ids.insert(id);
    }
}

void VisibleIndex::Remove(const std::shared_ptr<Item> &item)
{
    m_item_count -= item->count();
    const QString id = item->id();
    if (id.isEmpty()) {
        if ((m_empty_id_count > 0) && (--m_empty_id_count == 0)) {
            m_unindexed_ids.erase(id);
        }
        return;
    }
    const auto dup = m_duplicate_ids.find(id);
    if (dup != m_duplicate_ids.end()) {
        auto &occurrences = dup->second;
        const auto occurrence = std::find(occurrences.begin(), occurrences.end(), item);
        if (occurrence != occurrences.end()) {
            occurrences.erase(occurrence);
        }
        if (occurrences.size() == 1) {
            // Unique again: the survivor is the freshly-refiltered answer.
            m_by_id[id] = occurrences.front();
            m_duplicate_ids.erase(dup);
            m_unindexed_ids.erase(id);
        } else if (!occurrences.empty()) {
            m_by_id[id] = occurrences.front();
        } else {
            m_by_id.erase(id);
            m_duplicate_ids.erase(dup);
            m_unindexed_ids.erase(id);
        }
    } else {
        const auto it = m_by_id.find(id);
        if ((it != m_by_id.end()) && (it->second == item)) {
            m_by_id.erase(it);
        }
    }
}
