// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bucket.h"
#include "column.h"
#include "fetchsourcekey.h"
#include "filters/filterstate.h"
#include "item.h"
#include "items_model.h"
#include "locationinventory.h"
#include "util/util.h"

class BuyoutManager;
class FilterCatalog;
class ItemsModel;
class QModelIndex;
struct BuyoutChangeSet;

class Search
{
    Q_GADGET
public:
    enum class ViewMode : int { ByTab = 0, ByItem = 1 };
    Q_ENUM(ViewMode)

    // The inventory resolves bucket metadata to the freshest location seen
    // per stable display key (M2 D6); null (tests without a pipeline) keeps
    // each item's embedded location and the published tab list alone.
    Search(BuyoutManager &bo,
           const QString &caption,
           const FilterCatalog &catalog,
           const LocationInventory *location_inventory = nullptr);
    ~Search();
    void FilterItems(const Items &items);
    const QString &caption() const { return m_caption; }
    // The visible filtered collection. The delta path maintains the
    // active mode's buckets and the indexes incrementally (S4/S5) and
    // this flat vector is rebuilt lazily from the maintained side on
    // first read — no delta pays an O(collection) pass to keep it fresh.
    const Items &items() const;
    const std::vector<std::unique_ptr<Column>> &columns() const { return m_columns; }
    ItemsModel &model() { return m_model; }
    // Expansion is keyed by the stable (type, id) display key (M2 R6-3):
    // header text mutates on rename, so a header-keyed save would orphan
    // the expansion state exactly when a delta renames the expanded tab.
    const std::set<LocationInventory::Key> &expandedKeys() const { return m_expanded_keys; }
    void setExpandedKeys(std::set<LocationInventory::Key> keys);

    // Folder expansion (folder-tree D9, R3): folders are expanded unless
    // the user collapsed them. The folders this search's user collapsed,
    // by stable key, maintained from the view's expanded and collapsed
    // signals; a folder in the set stays collapsed across resets and
    // re-insertion.
    bool folderCollapsed(const LocationInventory::Key &key) const
    {
        return m_collapsed_folder_keys.count(key) > 0;
    }
    const std::set<LocationInventory::Key> &collapsedFolderKeys() const
    {
        return m_collapsed_folder_keys;
    }
    void setCollapsedFolderKeys(std::set<LocationInventory::Key> keys)
    {
        m_collapsed_folder_keys = std::move(keys);
    }
    void setFolderCollapsed(const LocationInventory::Key &key, bool collapsed)
    {
        if (collapsed) {
            m_collapsed_folder_keys.insert(key);
        } else {
            m_collapsed_folder_keys.erase(key);
        }
    }

    // The scroll state captured immediately before the last reset of this
    // search's model (M2 R6-3): a top-row anchor plus the raw scrollbar
    // value as the fallback when the anchored row no longer exists.
    struct ScrollAnchor
    {
        // The top visible row's bucket, and the item's stable id when that
        // row was an item (empty when it was the bucket header itself).
        std::optional<LocationInventory::Key> bucket_key;
        QString item_id;
        int scrollbar_value{0};
    };
    const ScrollAnchor &scrollAnchor() const { return m_scroll_anchor; }
    void setScrollAnchor(ScrollAnchor anchor) { m_scroll_anchor = std::move(anchor); }

    // Global stable-identity lookup over the visible filtered result (M2
    // R6-3): index-backed, rebuilt by every refilter, so reselection never
    // scans the model and survives an item moving to another tab
    // mid-refresh. Returns null for unknown or empty ids.
    std::shared_ptr<Item> visibleItemById(const QString &id) const
    {
        const auto it = m_visible_by_id.find(id);
        return (it != m_visible_by_id.end()) ? it->second : nullptr;
    }

    // True when the id index cannot represent every visible occurrence of
    // this id (M3 S2): a duplicated id (the index keeps the first
    // occurrence only — R6-3 reselection wants exactly one item) or the
    // empty id shared by id-less items. The buyout repaint falls back to
    // scanning every bucket for such ids; rebuilt by every refilter.
    bool visibleIdUnindexed(const QString &id) const
    {
        return m_unindexed_visible_ids.count(id) > 0;
    }
    const std::shared_ptr<Item> &currentItem() const { return m_current_item; }
    void setCurrentItem(std::shared_ptr<Item> item) { m_current_item = std::move(item); }
    const std::optional<ItemLocation> &currentBucket() const { return m_current_bucket; }
    void setCurrentBucket(std::optional<ItemLocation> bucket)
    {
        m_current_bucket = std::move(bucket);
    }
    bool defaultExpanded() const { return m_filtered || (m_current_mode == ViewMode::ByItem); }
    const std::vector<Bucket> &buckets() const;
    void RenameCaption(const QString &newName);
    QString GetCaption() const;
    ItemLocation GetTabLocation(const QModelIndex &index) const;
    void SetViewMode(ViewMode mode);
    ViewMode GetViewMode() const { return m_current_mode; }
    bool has_bucket(int row) const;
    const Bucket &bucket(int row) const;
    const QModelIndex index(const std::shared_ptr<Item> &item) const;
    void SetRefreshReason(RefreshReason reason) { m_refresh_reason = reason; }

    // Sorts the materialized buckets whose flags are invalid (D2): the
    // By-Item flat bucket always, expanded By-Tab buckets by their marks.
    // Collapsed buckets are untouched — the invalidation events cleared
    // their flags, and their sort defers to expansion.
    void Sort(int column, Qt::SortOrder order);

    // Sort-on-expand's per-bucket entry (D2 rule 2), called under the
    // model's layout-change protocol by ItemsModel.
    void SortBucket(int row, int column, Qt::SortOrder order);

    // The view's materialization marks (D1/D2), driven by the tree's
    // expand/collapse signals through ItemsModel. Collapse evicts the
    // bucket's keys; its order and flag persist (D2 rule 3).
    void MarkBucketExpanded(int row);
    void MarkBucketCollapsed(int row);

    // R2-4 / D1 rule 2: evicts every resident key vector, both view modes.
    // Orders and flags persist. Runs on deactivation and on a sort-column
    // switch.
    void EvictResidentKeys();

    // The R3-1 eager carve-out at the activation boundary (D4 rule 1, M3
    // S5): a clean By-Item search's flat bucket holds resident keys
    // whenever the search is active, so no delta ever meets a keyless
    // flat bucket and the merge budget holds unconditionally. Called
    // after activation has decided dirtiness — a dirty search refiltered
    // instead, its sort supplying the keys. No-op in By-Tab mode and
    // when keys are already resident.
    void HydrateFlatBucketKeys();

    // D1 rules 2-3: clears every bucket's sorted flag, both view modes —
    // any (column, order) change invalidates every order at once.
    void InvalidateAllOrder();

    // The buyout key effect (D1 rule 4, R3-2): rebuilds the affected
    // items' entries in whichever key vectors are resident and clears the
    // affected buckets' sorted flags, both view modes — so the batch's
    // re-sort never runs on stale resident keys. Returns the active-mode
    // rows of the affected MATERIALIZED buckets — exactly the set the
    // batch must reorder now; an empty return means the batch needs no
    // layout operation at all (collapsed buckets defer to expansion).
    // `column` is the active sort column.
    std::vector<int> InvalidateBuyoutOrder(const BuyoutChangeSet &changes, int column);

    const FilterCatalog &catalog() const { return m_filter_catalog; }
    qsizetype filterStateCount() const { return static_cast<qsizetype>(m_filter_states.size()); }
    const FilterState &filterStateAt(qsizetype index) const;

    // The only way to change a filter state. Marks the search dirty when the
    // new state differs, so a state changed while this search is in the
    // background still forces a refilter when it is next shown.
    void setFilterState(qsizetype index, FilterState state);

    // D9 rule 1 (items-pipeline M2): every delta marks every search
    // items-dirty; the flag is cleared only by this search's own successful
    // refilter and is consumed by the same refilter-on-next-activation gate
    // as m_states_dirty.
    bool itemsDirty() const { return m_items_dirty; }
    void setItemsDirty(bool dirty) { m_items_dirty = dirty; }

    // The active-filter membership test the delta path applies to every
    // arrival (D3; formerly the M2 D9 intersection's match half — the
    // intersection sets themselves were deleted by the spec's July 31,
    // 2026 post-freeze amendment when S5's throttle retirement removed
    // their final consumer).
    bool MatchesActiveFilters(const Item &item) const;

    // The result of one delta application (M3 S4, D3). `processed` is the
    // R1-7 adjudication: true when the delta was applied or correctly
    // adjudicated "no visible change" — the search stays clean; false when
    // application was skipped for any reason — the caller marks the search
    // dirty (fail-safe direction). The two change flags separate grains
    // that S4/S6 originally conflated (S6 review round 1):
    // `content_changed` means item rows were removed or inserted — it
    // drives the staleness marks and lazy rebuilds; `model_changed` means
    // ANY model operation ran (row ops, bucket insert/remove/move,
    // metadata repaint) — it drives view-side presentation work such as
    // the column resize. Metadata-only deltas and empty-bucket
    // structural changes set only the latter.
    struct DeltaApplication
    {
        bool processed{false};
        bool content_changed{false};
        bool model_changed{false};
        // Buckets inserted by this delta (new tab, or first filter-passing
        // items for a hidden one — and the folder such a bucket needed,
        // folder-tree D7), as final bucket rows, ascending; the caller
        // default-expands them in the view when the search is
        // default-expanded.
        std::vector<int> inserted_bucket_rows;
    };

    // Applies a TabRefreshed delta. By-Tab: bucket-scoped operations
    // (D3) — metadata first (R1-4 — rename/move/color render now, item
    // intersection notwithstanding), then a source-scoped row replacement
    // within the display bucket — arrivals merged into the retained sorted
    // order when the bucket is visible (R2-2), appended arrival-ordered
    // when collapsed. By-Item (D4, S5): the flat bucket's per-delta
    // contract — erase the source's rows as contiguous removeRows runs,
    // then merge the filter-accepted arrivals into the resident sorted
    // order; the By-Tab side is marked stale and rebuilds at the next
    // mode switch. Visible indexes are maintained incrementally in both.
    DeltaApplication ApplyTabDelta(const ItemLocation &location, const Items &items);

    // The result of the final snapshot's reconciliation (M3 S6, R1-2).
    // The change flags carry the same grains as DeltaApplication's:
    // `content_changed` = item rows removed/inserted (staleness marks),
    // `model_changed` = any model operation (presentation work). A new
    // empty tab or a deleted empty bucket is model-only — it must not
    // mark the flat bucket stale (S6 review round 1).
    struct SnapshotReconciliation
    {
        bool content_changed{false};
        bool model_changed{false};
        // Buckets the reconciliation inserted (final display rows,
        // ascending); the caller default-expands them in the view when the
        // search is default-expanded, like a delta's inserted bucket.
        std::vector<int> inserted_bucket_rows;
    };

    // The R1-2 authoritative row reconciliation on `ItemsRefreshed` (S6):
    // one pass diffing this search's model against the post-snapshot
    // published state per stable key — buckets and rows for deleted tabs
    // removed, newly listed tabs inserted at their display positions
    // (unfiltered searches; filtered ones still hide empty buckets),
    // metadata refreshed against the rebased locations, bucket order
    // corrected via move ops. Row operations only — never a reset;
    // O(collection) once per refresh is accepted. The pass is
    // authoritative at the row grain (the diff is by item identity, and
    // the worker publishes the same Item objects the deltas delivered),
    // so it also discharges R1-7's fail-safe dirtiness: any content a
    // skipped delta left stale is replaced here and the items-dirty flag
    // clears. In By-Item mode the diff runs against the flat bucket and
    // the By-Tab side is marked stale for the next mode switch.
    SnapshotReconciliation ReconcileFinalSnapshot(const Items &published);

    // Applies a ChildrenReconciled aggregate as row removals (D3):
    // visible items under the parent's stable key whose fetch source is
    // outside the expected set leave via contiguous removeRows batches —
    // scoped to the parent's bucket in By-Tab, to the parent's rows
    // within the flat bucket in By-Item (S5).
    DeltaApplication ApplyChildReconciliation(const ItemLocation &parent,
                                              const std::vector<FetchSourceKey> &expected);

    // The model's child-index identity (M3 S4): child indexes carry the
    // bucket's stable serial, so top-level insert/remove/move operations
    // never invalidate a child's parent mapping. -1 for unknown serials.
    int rowForSerial(std::uint64_t serial) const;

    // The row mapping behind ItemsModel's two conversions (folder-tree
    // D4). A "bucket row" indexes the active mode's bucket vector and is
    // the bucket handle throughout Search, ItemsModel and MainWindow; the
    // model row of a bucket header is its row within its parent (the
    // root, or a folder bucket). Rebuilt with the other row lookups, so
    // each answers the model's current structure at every notification
    // boundary. -1 for out-of-range arguments.
    //
    // The bucket row of the `row`-th child of `parent_bucket_row` (-1:
    // the root).
    int childBucketRow(int parent_bucket_row, int row) const;
    // The number of bucket headers under `parent_bucket_row` (-1: the
    // root).
    int childBucketCount(int parent_bucket_row) const;
    // The bucket row of a bucket's parent header; -1 at the top level.
    int parentBucketRow(int bucket_row) const;
    // A bucket header's model row within its parent.
    int rowInParent(int bucket_row) const;
    // True for a By-Tab bucket whose location is a Folder-type stash
    // (folder-tree Terms): its children are bucket headers, never items.
    bool isFolderBucket(int bucket_row) const;

    // The active-mode bucket row for a stable display key, -1 when
    // absent (S4 review round 2): O(log tabs) through the delta path's
    // key map in By-Tab; the By-Item flat bucket answers only for its
    // own null-location key. Keeps per-delta consumers (the selected-
    // bucket reconcile) off O(tabs) scans.
    int rowForKey(const LocationInventory::Key &key) const;

    // The active-mode bucket row that holds a visible item: its tab's
    // bucket by stable key in By-Tab, the flat bucket in By-Item. -1 when
    // no such bucket is present.
    int bucketRowFor(const Item &item) const;

private:
    std::vector<Bucket> &active_buckets();
    const ItemLocation &canonicalLocation(const ItemLocation &embedded) const;

    // S4 delta internals. The index helpers keep m_visible_by_id, the
    // duplicate-occurrence side table, the unindexed set, and the filtered
    // item count answering as if freshly refiltered, in O(1) per item.
    void IndexInsertVisible(const std::shared_ptr<Item> &item);
    void IndexRemoveVisible(const std::shared_ptr<Item> &item);
    void AssignSerials();
    void RebuildRowLookups();
    int FindBucketRow(const LocationInventory::Key &key) const;

    // Folder-tree structure (D2, D3, D7, D8), By-Tab only. The By-Tab
    // vector is kept in depth-first tree order: each folder bucket is
    // followed by its nested buckets, and siblings at either level are in
    // bucketDisplayLess order — which is D3's lexicographic order on
    // (group anchor, header before members, self). Each bucket records the
    // folder it is currently rendered under (Bucket::parentSerial), and
    // the row lookups derive the model's structure from that, so a
    // primitive's single row notification and its vector change agree at
    // every notification boundary.
    //
    // The one function deciding nesting (D2/D3), used by both the
    // ordering and the row mapping: the bucket row of the present folder
    // bucket a bucket with `location` belongs under, -1 for the top level.
    int NestingParentRow(const ItemLocation &location) const;
    // A folder's location when the canonical inventory or the published
    // tab list knows it as a Folder-type stash (D2 folder presence).
    std::optional<ItemLocation> KnownFolderLocation(const QString &folder_id) const;
    // The one By-Tab grouping rule (M2 D6/R5-1, D2 rule 5, folder-tree
    // D2), shared by the refilter, the mode switch and the final
    // snapshot's reconciliation: `accepted` grouped by stable display key
    // under each key's canonical location; every listed tab, empty or
    // not, when the search is unfiltered; and each known folder a present
    // member names.
    std::map<LocationInventory::Key, Bucket> GroupByTab(const Items &accepted) const;
    // Sorts a freshly built By-Tab vector into tree order and records each
    // bucket's parent (refilter and mode-switch rebuilds).
    void ArrangeTabTree();
    // A bucket's position among `parent_row`'s children by
    // bucketDisplayLess, not counting `skip_row`.
    int SiblingPosition(int parent_row, const Bucket &bucket, int skip_row) const;
    // Primitives: one model row operation each; return the bucket's final
    // bucket row. A folder moves with its nested run; a folder is removed
    // only with no nested buckets.
    int InsertBucketUnder(Bucket bucket, int parent_row);
    void RemoveBucketAt(int bucket_row);
    int MoveBucketTo(int bucket_row, int parent_row, int position);
    int MoveBucketUnder(int bucket_row, int parent_row);
    // D7 Insert: a bucket at its tree position — its known but absent
    // folder inserted first; a folder adopting its present top-level
    // members. Returns the serials of every bucket inserted.
    std::vector<std::uint64_t> InsertTabBucket(const ItemLocation &canonical, const Items &accepted);
    // The present folder bucket's row for `folder_id`, inserting it (and
    // adopting its members) when absent but known; -1 when unknown.
    int EnsureFolderPresent(const QString &folder_id, std::vector<std::uint64_t> *inserted);
    void AdoptMembers(int folder_row);
    // D7 Remove: unwinds the visible indexes for the bucket's items and
    // removes its row — a folder's members first moved to the top level.
    // With `cascade`, a filtered search also drops the folder the bucket
    // left when it has no present members left. Returns true when item
    // rows left with the bucket.
    bool RemoveTabBucket(int bucket_row, bool cascade);
    // D7 Emptiness, the one predicate every "filtered searches hide empty
    // buckets" site goes through: no items for a tab bucket, no present
    // members for a folder bucket.
    bool IsEmptyForFilter(int bucket_row) const;
    // True when a non-folder bucket naming `folder_id` is present.
    bool HasPresentMembers(const QString &folder_id) const;
    // Final-snapshot settling (D7): true when every bucket sits under its
    // nesting parent in sibling order; SettleTabTree restores that with
    // move operations.
    bool TabTreeInOrder() const;
    // True when a bucket sits under its nesting parent and between its
    // neighbouring siblings in order — O(1) plus one key lookup.
    bool BucketInPlace(int bucket_row) const;
    void SettleTabTree();
    void SortSiblings(int parent_row);
    // A delta's inserted serials as final bucket rows, ascending.
    std::vector<int> InsertedRows(const std::vector<std::uint64_t> &serials) const;

    // Renders a delta's fresh location anchor on an existing bucket:
    // dataChanged for header text/color, a move when its tree position
    // changes — within its parent, or across parents when its folder id
    // changed (D7/D8), applying the presence rule to the folder entered
    // and the folder left. Returns the bucket's (possibly moved) row; when
    // `model_changed` is given, ORs in whether any model operation ran;
    // a folder the move needed is recorded in `inserted`.
    int ApplyBucketMetadata(int bucket_row,
                            const ItemLocation &canonical,
                            bool *model_changed = nullptr,
                            std::vector<std::uint64_t> *inserted = nullptr);
    // Removes the bucket rows matching `predicate` as contiguous
    // removeRows runs, unwinding the visible indexes per removed item.
    // Returns true when anything was removed.
    template<typename Predicate>
    bool RemoveBucketRows(int bucket_row, Predicate &&predicate);
    // Inserts filter-accepted arrivals: sorted merge into a materialized
    // sorted bucket (R2-2 in By-Tab; the D4 merge in By-Item, where the
    // flat bucket is materialized by definition), arrival-ordered append
    // otherwise.
    void InsertArrivals(int bucket_row, const Items &accepted);
    // The D4 flat-bucket content application (S5): source-scoped erase,
    // then the sorted merge of the accepted arrivals; visible indexes and
    // staleness marks maintained. The arrivals were already filtered.
    DeltaApplication ApplyFlatDelta(const FetchSourceKey &source, const Items &accepted);
    // Rebuilds the By-Tab display buckets from the maintained flat
    // collection when leaving By-Item mode (S5): By-Item deltas mark the
    // By-Tab side stale instead of maintaining two structures per delta.
    void RebuildTabBucketsFromFlat();

    BuyoutManager &m_bo_manager;
    const LocationInventory *m_location_inventory{nullptr};

    // Catalog and filter states are index-aligned. MainWindow owns the catalog
    // and outlives every Search.
    const FilterCatalog &m_filter_catalog;
    std::vector<FilterState> m_filter_states;

    // True when a filter state changed since the last time this search
    // actually filtered. A tab change alone does not need a refilter; a tab
    // change after a state change does.
    bool m_states_dirty{false};

    // True when a streamed delta changed the underlying items since this
    // search last filtered (D9 rule 1).
    bool m_items_dirty{false};

    // The visible result by stable item id (R6-3 reselection), rebuilt by
    // every refilter and maintained per delta (S4). For a duplicated id
    // the entry is the first occurrence — the invariant is
    // m_visible_by_id[id] == m_duplicate_visible_ids[id].front().
    std::unordered_map<QString, std::shared_ptr<Item>> m_visible_by_id;

    // Ids the index above cannot fully represent: duplicated ids and the
    // empty id (see visibleIdUnindexed).
    std::set<QString> m_unindexed_visible_ids;

    // Every visible occurrence of a currently-duplicated non-empty id
    // (mid-refresh divergence can show one id in two tabs). Kept so a
    // delta removing one occurrence restores the survivor to the id index
    // exactly — the indexes answer as if freshly refiltered
    // (deltaUpdatesVisibleIndexesIncrementally). Empty except mid-refresh.
    std::unordered_map<QString, Items> m_duplicate_visible_ids;
    // Visible items sharing the empty id; the unindexed set carries the
    // empty id while this is nonzero.
    size_t m_empty_id_visible_count{0};

    std::vector<std::unique_ptr<Column>> m_columns;

    ItemsModel m_model;
    std::vector<Bucket> m_bucket_by_tab;
    std::vector<Bucket> m_bucket_by_item;

    // Child-index identity (S4): the active mode's bucket row per stable
    // bucket serial, rebuilt on refilter/mode switch and maintained by
    // the structural delta operations. The stable-key map is its By-Tab
    // sibling, giving the delta path O(log tabs) bucket lookup instead of
    // a linear scan (S4 review round 1); both are remapped together.
    // Structural ops (bucket insert/remove/move) inherently pay O(tabs)
    // to remap row positions — accepted: they are rare (metadata
    // reposition, tab discovery, filtered-empty drop), and any
    // row-position scheme pays the same.
    std::unordered_map<std::uint64_t, int> m_row_by_serial;
    std::map<LocationInventory::Key, int> m_row_by_key;
    // The active mode's row mapping (folder-tree D4, rebuilt with the maps
    // above): the bucket rows of the top-level headers in model order, and
    // per bucket row its parent's bucket row (-1 at the top level) and its
    // model row within that parent.
    std::vector<int> m_top_level_rows;
    std::vector<int> m_parent_rows;
    std::vector<int> m_rows_in_parent;
    // Per bucket row, the number of buckets nested under it (folders).
    std::vector<int> m_child_counts;
    // Counts structural row operations (insert/remove/move of a bucket
    // header), so composite operations report model_changed exactly.
    std::uint64_t m_structural_ops{0};
    std::uint64_t m_next_bucket_serial{1};

    // Staleness marks: the delta path maintains the ACTIVE mode's buckets
    // and the indexes (S4 By-Tab, S5 By-Item); the other side rebuilds
    // lazily — m_items on first read, the inactive mode's buckets at the
    // next mode switch. At most one of the two mode marks is set: each
    // mode's application stales the other side, and a refilter clears
    // both.
    mutable bool m_items_stale{false};
    bool m_flat_bucket_stale{false};
    bool m_tab_buckets_stale{false};

    QString m_caption;
    mutable Items m_items;
    // True when any filter is active (the spec's post-freeze amendment
    // at D2 rule 5 — see the FilterItems comment): stable across
    // deltas, flips only at filter edits. Drives hidden empty buckets
    // and default expansion.
    bool m_filtered;
    size_t m_filtered_item_count;
    std::set<LocationInventory::Key> m_expanded_keys;
    std::set<LocationInventory::Key> m_collapsed_folder_keys;
    ScrollAnchor m_scroll_anchor;
    std::shared_ptr<Item> m_current_item;
    std::optional<ItemLocation> m_current_bucket;
    ViewMode m_current_mode;
    RefreshReason m_refresh_reason;
};

template<>
struct fmt::formatter<Search::ViewMode, char> : QtEnumFormatter<Search::ViewMode>
{};
