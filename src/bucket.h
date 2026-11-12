// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <vector>

#include "column.h"
#include "item.h"
#include "itemlocation.h"
#include "sortkey.h"

// The bucket's resident sort keys (items-pipeline-m3.md D1): index-aligned
// with the bucket's items, derived from exactly one column. `column` doubles
// as the residency marker — null means evicted. The store owns its share of
// the live-key-bytes gauge (ModelProbes::live_key_bytes) and releases it on
// eviction, move, and destruction, so the gauge balances however a bucket
// dies (refilter rebuild, search deletion, mode switch).
struct ResidentKeyStore
{
    std::vector<ItemSortKey> keys;
    const Column *column{nullptr};
    std::int64_t bytes{0};

    ResidentKeyStore() = default;
    ResidentKeyStore(const ResidentKeyStore &) = delete;
    ResidentKeyStore &operator=(const ResidentKeyStore &) = delete;
    ResidentKeyStore(ResidentKeyStore &&other) noexcept
        : keys(std::move(other.keys))
        , column(other.column)
        , bytes(other.bytes)
    {
        other.keys.clear();
        other.column = nullptr;
        other.bytes = 0;
    }
    ResidentKeyStore &operator=(ResidentKeyStore &&other) noexcept
    {
        if (this != &other) {
            Release();
            keys = std::move(other.keys);
            column = other.column;
            bytes = other.bytes;
            other.keys.clear();
            other.column = nullptr;
            other.bytes = 0;
        }
        return *this;
    }
    ~ResidentKeyStore() { Release(); }

    bool resident() const { return column != nullptr; }
    // Evicts the keys and returns their bytes to the gauge.
    void Release();
};

// A bucket holds set of filtered items.
// Items are "bucketed" by their location: stash tab / character.
class Bucket
{
public:
    Bucket() = default;
    explicit Bucket(const ItemLocation &location);

    // Non-copyable
    Bucket(const Bucket &) = delete;
    Bucket &operator=(const Bucket &) = delete;

    // Moveable
    Bucket(Bucket &&) = default;
    Bucket &operator=(Bucket &&) = default;

    void AddItem(const std::shared_ptr<Item> &item);
    void AddItems(const Items &items);
    // The raw item vector. NOT valid inside a ReplaceSourceRows
    // notification window — model-facing consumers go through size() /
    // has_item() / item(), which answer through the replace translation
    // while one is active.
    const Items &items() const { return m_items; }
    int size() const;
    bool has_item(int row) const;
    const std::shared_ptr<Item> &item(int row) const;
    const ItemLocation &location() const { return m_location; }

    // Metadata refresh (M3 D3, R1-4): a delta's location anchor supersedes
    // the bucket's rendered metadata; identity (the stable (type, id) key)
    // never changes through this.
    void SetLocation(const ItemLocation &location) { m_location = location; }

    // The stable bucket identity the model's child indexes carry (M3 S4):
    // assigned by Search when the bucket enters a bucket vector, unique
    // within the search for the model's lifetime, and independent of the
    // bucket's display row — so top-level insert/remove/move operations
    // never invalidate a child index's parent mapping.
    std::uint64_t serial() const { return m_serial; }
    void SetSerial(std::uint64_t serial) { m_serial = serial; }

    // The serial of the folder bucket this bucket is currently rendered
    // under (folder-tree D2/D4), 0 at the top level. The model's current
    // structure, maintained by Search's structural operations — not the
    // target nesting, which Search derives from folder membership.
    std::uint64_t parentSerial() const { return m_parent_serial; }
    void SetParentSerial(std::uint64_t serial) { m_parent_serial = serial; }

    // Row-level content operations for the S4 delta path (D3). Both keep
    // the resident key vector index-aligned with the items and the
    // live-key-bytes gauge balanced. InsertRows takes the arrivals' keys
    // when the caller merged into a key-resident bucket; passing no keys
    // into a key-resident bucket evicts (the alignment cannot be kept).
    // Per-call vector splices: fine for By-Tab buckets (capped at 576
    // rows), never for the flat bucket — that is ReplaceSourceRows.
    void RemoveRows(int first, int count);
    void InsertRows(int first, const Items &items, const std::vector<ItemSortKey> *keys = nullptr);

    // The A′ flat replace (D4 rule 2, S5 remedy): removes every row
    // matching `predicate` and inserts `arrivals` — merged into the
    // resident sorted order when `column` is non-null (hydrating first,
    // R3-1), appended arrival-ordered with order invalidated and keys
    // evicted when null. The item and key vectors are rebuilt in ONE
    // O(n + d) pass — no per-run splice — while the same contiguous-run
    // removeRows/insertRows batches a per-run splice would emit are
    // driven through the callbacks; between each begin/end pair the
    // bucket answers size()/item() through a row translation over the
    // outgoing and final vectors, so the model is consistent at every
    // notification boundary. Removals notify back-to-front and
    // insertions forward, both in current logical coordinates. Returns
    // the removed items for the caller's index maintenance.
    Items ReplaceSourceRows(const std::function<bool(const Item &)> &predicate,
                            const Items &arrivals,
                            const Column *column,
                            Qt::SortOrder order,
                            const std::function<void(int, int)> &begin_remove,
                            const std::function<void()> &end_remove,
                            const std::function<void(int, int)> &begin_insert,
                            const std::function<void()> &end_insert);

    // Public hydration entry (R3-1): every key-consuming operation
    // hydrates missing keys first. The S4 merge consumes keys outside
    // Sort, so the entry is exposed.
    void EnsureResidentKeys(const Column &column) { HydrateKeys(column); }
    const std::vector<ItemSortKey> &residentKeys() const { return m_keys.keys; }

    // Sorts unconditionally on this column's keys — hydrating the resident
    // vector if evicted (R3-1), reusing it otherwise (R2-3) — and sets the
    // sorted flag. The D2 skip-when-valid lives in the callers (Search,
    // ItemsModel), which own flag validity against the model's current
    // (column, order).
    void Sort(const Column &column, Qt::SortOrder order);

    // D2's per-bucket sorted-validity flag. Sorted-order validity and key
    // residency are independent axes (R3-1): sorted-but-keyless is a
    // legitimate state, entered by expanding with a valid flag or by
    // collapse/deactivation eviction.
    bool sorted() const { return m_sorted; }
    void InvalidateOrder() { m_sorted = false; }

    // The live materialization mark (D1: a materialized bucket is an
    // expanded By-Tab bucket; the By-Item flat bucket is materialized by
    // definition and never consults this). Maintained by the view's
    // expand/collapse signals and by FilterItems for default-expanded
    // searches.
    bool expanded() const { return m_expanded; }
    void SetExpanded(bool expanded) { m_expanded = expanded; }

    // Collapse/deactivation eviction (D1/R2-3/R2-4): keys leave, order and
    // flag persist.
    void EvictKeys() { m_keys.Release(); }

    // The buyout key effect (R3-2): rebuild the resident entries of the
    // affected items — every entry when `everything` — so a batch's
    // re-sort never runs on stale resident keys. No-op when evicted (a
    // collapsed bucket's keys stay absent); keys resident for another
    // column are evicted instead of patched.
    void RebuildKeyEntries(const Column &column, const std::set<QString> &ids, bool everything);

private:
    void HydrateKeys(const Column &column);

    // Active ReplaceSourceRows notification window (null otherwise):
    // size()/has_item()/item() answer through it. Defined in bucket.cpp;
    // points at a stack object alive for the call's duration.
    struct ReplaceWindow;

    Items m_items;
    ItemLocation m_location;
    ResidentKeyStore m_keys;
    const ReplaceWindow *m_replace_window{nullptr};
    std::uint64_t m_serial{0};
    std::uint64_t m_parent_serial{0};
    bool m_sorted{false};
    bool m_expanded{false};
};
