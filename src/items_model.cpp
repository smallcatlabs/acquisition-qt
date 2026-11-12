// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "items_model.h"

#include "bucket.h"
#include "buyoutmanager.h"
#include "itemlocation.h"
#include "locationinventory.h"
#include "modelprobes.h"
#include "search.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

ItemsModel::ItemsModel(BuyoutManager &bo_manager, Search &search)
    : m_bo_manager(bo_manager)
    , m_search(search)
    , m_sort_order(Qt::DescendingOrder)
    , m_sort_column(0)
    , m_sorted(false)
{}

/*
    Tree structure:

    + stash folder (a folder bucket, folder-tree D2)
    | + stash tab title (called "bucket" elsewhere)
    | |- item
    | |- item
    + stash tab title
    |- item
    |- item
      ...
    + another stash tab or character
    |- item
    |- item

    and so on. Depth is fixed: a folder holds bucket headers, every other
    bucket holds items. internalId is 0 for a top-level header and
    otherwise the serial of the parent bucket, so the row kind follows
    from the parent (D5): a row under a folder bucket is a bucket header,
    a row under any other bucket is an item.
*/

QModelIndex ItemsModel::BucketIndex(int bucket_row, int column) const
{
    if (!m_search.has_bucket(bucket_row) || (column < 0)
        || (column >= static_cast<int>(m_search.columns().size()))) {
        return QModelIndex();
    }
    // D5: a top-level header carries 0, a nested header its parent's
    // serial.
    const int parent_row = m_search.parentBucketRow(bucket_row);
    const quintptr parent_serial = (parent_row >= 0)
                                       ? static_cast<quintptr>(m_search.bucket(parent_row).serial())
                                       : 0;
    return createIndex(m_search.rowInParent(bucket_row), column, parent_serial);
}

ItemsModel::RowRef ItemsModel::Locate(const QModelIndex &index) const
{
    if (!index.isValid() || (index.model() != this)) {
        return RowRef{};
    }
    if (index.internalId() == 0) {
        // A top-level header.
        return RowRef{m_search.childBucketRow(-1, index.row()), -1};
    }
    // The internalId is the parent bucket's stable serial (M3 S4), so the
    // mapping survives structural operations; a serial that no longer
    // resolves locates nothing (D5).
    const int owner = m_search.rowForSerial(index.internalId());
    if (owner < 0) {
        return RowRef{};
    }
    if (m_search.isFolderBucket(owner)) {
        // A nested bucket header.
        return RowRef{m_search.childBucketRow(owner, index.row()), -1};
    }
    return RowRef{owner, index.row()};
}

int ItemsModel::rowCount(const QModelIndex &parent) const
{
    // Root element, contains buckets
    if (!parent.isValid()) {
        return m_search.childBucketCount(-1);
    }
    if ((parent.model() != this) || (parent.column() != 0)) {
        return 0;
    }
    // Bucket, contains elements. size() answers through the A′ replace
    // translation while a flat replacement's notifications are running.
    const RowRef ref = Locate(parent);
    if (ref.isBucket()) {
        // A folder holds bucket headers (D2); its own items — none, D12 —
        // are never rendered (R8).
        if (m_search.isFolderBucket(ref.bucket_row)) {
            return m_search.childBucketCount(ref.bucket_row);
        }
        return m_search.bucket(ref.bucket_row).size();
    }
    // Element, contains nothing
    return 0;
}

int ItemsModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() && (parent.model() != this)) {
        return 0;
    }
    // Root element and buckets carry the columns; elements contain nothing.
    if (!parent.isValid() || Locate(parent).isBucket()) {
        return static_cast<int>(m_search.columns().size());
    }
    return 0;
}

QVariant ItemsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0
        && section < static_cast<int>(m_search.columns().size())) {
        return QString(m_search.columns()[section]->name());
    }
    return QVariant();
}

QVariant ItemsModel::data(const QModelIndex &index, int role) const
{
    // Bucket title
    if (!index.isValid()) {
        return QVariant();
    }
    if ((index.model() != this) || (index.column() < 0)
        || (index.column() >= static_cast<int>(m_search.columns().size()))) {
        return QVariant();
    }

    const RowRef ref = Locate(index);
    if (ref.isBucket()) {
        if (index.column() > 0) {
            return QVariant();
        }

        const ItemLocation &location = m_search.bucket(ref.bucket_row).location();
        if (role == Qt::CheckStateRole) {
            if (!location.IsValid()) {
                return QVariant();
            }
            if (m_search.isFolderBucket(ref.bucket_row)) {
                return FolderCheckState(ref.bucket_row);
            }
            if (m_bo_manager.GetRefreshLocked(location)) {
                return Qt::PartiallyChecked;
            }
            return (m_bo_manager.GetRefreshChecked(location) ? Qt::Checked : Qt::Unchecked);
        }
        if (role == Qt::DisplayRole) {
            if (!location.IsValid()) {
                return "All Items";
            }
            QString title(location.GetHeader());
            // A folder header carries no buyout suffix (folder-tree D6/D11,
            // R7): a tab buyout stored against a folder id by an earlier
            // version stays in the store, unrendered.
            if (!m_search.isFolderBucket(ref.bucket_row)) {
                const Buyout bo = m_bo_manager.EffectiveTab(location).buyout;
                if (bo.IsActive()) {
                    title += QString(" [%1]").arg(bo.AsText());
                }
            }
            return title;
        }
        if (location.IsValid() && location.type() == ItemLocationType::STASH) {
            if (role == Qt::BackgroundRole) {
                QColor backgroundColor(location.getR(), location.getG(), location.getB());
                if (backgroundColor.isValid()) {
                    return backgroundColor;
                }
            }
            if (role == Qt::ForegroundRole) {
                QColor backgroundColor(location.getR(), location.getG(), location.getB());
                return Util::recommendedForegroundTextColor(backgroundColor);
            }
        }
        return QVariant();
    }
    auto &column = m_search.columns()[index.column()];
    const int bucket_row = ref.bucket_row;
    if (ref.isItem()) {
        const Bucket &bucket = m_search.bucket(bucket_row);
        const int item_row = ref.item_row;
        if (bucket.has_item(item_row)) {
            const Item &item = *bucket.item(item_row);
            if (role == Qt::DisplayRole) {
                return column->value(item);
            } else if (role == Qt::ForegroundRole) {
                return column->color(item);
            } else if (role == Qt::DecorationRole) {
                return column->icon(item);
            }
        } else {
            spdlog::error("items model cannot get data: bucket {} does not have {} items",
                          bucket_row,
                          item_row);
        }
    } else {
        spdlog::error("items model cannot get data: bucket {} does not exist", bucket_row);
    }
    return QVariant();
}

Qt::ItemFlags ItemsModel::flags(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return Qt::ItemFlags();
    }
    if ((index.model() != this) || (index.column() < 0)
        || (index.column() >= static_cast<int>(m_search.columns().size()))) {
        return Qt::ItemFlags();
    }
    const RowRef ref = Locate(index);
    if ((index.column() == 0) && ref.isBucket()) {
        // A folder is checkable while it has present members (D10); its id
        // is never read from the stored refresh state.
        if (m_search.isFolderBucket(ref.bucket_row)) {
            if (m_search.childBucketCount(ref.bucket_row) > 0) {
                return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
            }
            return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
        }
        const ItemLocation &location = m_search.GetTabLocation(index);
        if (location.IsValid() && !m_bo_manager.GetRefreshLocked(location)) {
            return Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
        }
    }
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

bool ItemsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || (index.model() != this)) {
        return false;
    }

    const RowRef ref = Locate(index);
    if (role == Qt::CheckStateRole && index.column() == 0 && ref.isBucket()) {
        if (m_search.isFolderBucket(ref.bucket_row)) {
            return SetFolderChecked(ref.bucket_row, value.toInt() != Qt::Unchecked);
        }
        // The store refuses the By-Item header's empty location and a
        // refresh-locked tab.
        const ItemLocation &location = m_search.GetTabLocation(index);
        if (!m_bo_manager.SetRefreshChecked(location, value.toBool())) {
            return false;
        }

        // It's possible that our tabs can have the same name.  Right now we don't have a
        // way to differentiate these tabs so indicate dataChanged event for each tab with
        // the same name as the current checked tab so the 'check' is properly updated in
        // the layout
        QString target_hash = location.id();
        const int bucket_count = static_cast<int>(m_search.buckets().size());
        for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
            if (m_search.bucket(bucket_row).location().id() == target_hash) {
                const QModelIndex match_index = BucketIndex(bucket_row);
                emit dataChanged(match_index, match_index, {Qt::CheckStateRole});
                // A nested tab's folder derives its state from it (D10).
                const int folder_row = m_search.parentBucketRow(bucket_row);
                if (folder_row >= 0) {
                    const QModelIndex folder_index = BucketIndex(folder_row);
                    emit dataChanged(folder_index, folder_index, {Qt::CheckStateRole});
                }
            }
        }
        return true;
    }
    return false;
}

QVariant ItemsModel::FolderCheckState(int folder_row) const
{
    // D10 / R5: derived from the members present in this search — all
    // refreshing, none, or mixed; a refresh-locked member counts as
    // refreshing (GetRefreshChecked includes the lock). No members, no
    // checkbox.
    const int members = m_search.childBucketCount(folder_row);
    if (members == 0) {
        return QVariant();
    }
    int refreshing = 0;
    for (int n = 0; n < members; ++n) {
        const int member = m_search.childBucketRow(folder_row, n);
        if (m_bo_manager.GetRefreshChecked(m_search.bucket(member).location())) {
            ++refreshing;
        }
    }
    if (refreshing == members) {
        return Qt::Checked;
    }
    return (refreshing == 0) ? Qt::Unchecked : Qt::PartiallyChecked;
}

bool ItemsModel::SetFolderChecked(int folder_row, bool checked)
{
    // D10: the value goes to each present member that is not
    // refresh-locked — never to the folder id — then the members and the
    // folder repaint.
    const int members = m_search.childBucketCount(folder_row);
    if (members == 0) {
        return false;
    }
    for (int n = 0; n < members; ++n) {
        const ItemLocation &location = m_search.bucket(m_search.childBucketRow(folder_row, n))
                                           .location();
        m_bo_manager.SetRefreshChecked(location, checked); // refused when locked
    }
    const QModelIndex folder = BucketIndex(folder_row);
    emit dataChanged(index(0, 0, folder), index(members - 1, 0, folder), {Qt::CheckStateRole});
    emit dataChanged(folder, folder, {Qt::CheckStateRole});
    return true;
}

void ItemsModel::refreshCheckStates()
{
    // Every header row, nested ones included (D10): the top level in one
    // range, each folder's members in another.
    const int rows = rowCount();
    if (rows > 0) {
        emit dataChanged(index(0, 0), index(rows - 1, 0), {Qt::CheckStateRole});
    }
    const int bucket_count = static_cast<int>(m_search.buckets().size());
    for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
        const int members = m_search.childBucketCount(bucket_row);
        if (members > 0) {
            const QModelIndex folder = BucketIndex(bucket_row);
            emit dataChanged(index(0, 0, folder),
                             index(members - 1, 0, folder),
                             {Qt::CheckStateRole});
        }
    }
}

void ItemsModel::sort(int column, Qt::SortOrder order)
{
    if ((column < 0) || (column >= columnCount())) {
        return;
    }

    // Ignore sort requests if we're already sorted
    if (m_sorted && (m_sort_column == column) && (m_sort_order == order)) {
        return;
    }

    spdlog::debug("Sorting items model by column {}", column);
    // D1 rules 2-3: a column switch discards every resident key vector
    // (keys are derived from the outgoing column); any (column, order)
    // change invalidates every bucket's order at once. The post-refilter
    // indicator pass arrives with neither changed and clears nothing —
    // fresh buckets are already unsorted, and untouched valid buckets
    // (a background search re-activating after a scoped buyout batch)
    // keep their skip.
    if (column != m_sort_column) {
        m_search.EvictResidentKeys();
    }
    if ((column != m_sort_column) || (order != m_sort_order)) {
        m_search.InvalidateAllOrder();
    }
    m_sort_order = order;
    m_sort_column = column;
    ApplySort(column, order);
}

void ItemsModel::Resort()
{
    if ((m_sort_column < 0) || (m_sort_column >= columnCount())) {
        return;
    }
    ApplySort(m_sort_column, m_sort_order);
}

void ItemsModel::ResortBucket(int row)
{
    if ((m_sort_column < 0) || (m_sort_column >= columnCount()) || !m_search.has_bucket(row)) {
        return;
    }
    ApplySort(m_sort_column, m_sort_order, row);
}

void ItemsModel::OnBucketExpanded(int row)
{
    if (!m_search.has_bucket(row) || m_search.isFolderBucket(row)) {
        return;
    }
    m_search.MarkBucketExpanded(row);
    // D2 rule 2: an invalid flag sorts the bucket first (building its
    // keys, which stay resident — D1); a valid flag does no sort work and
    // builds no keys — the bucket stays sorted-but-keyless until a
    // key-consuming event hydrates it (R3-1).
    if (!m_search.bucket(row).sorted()) {
        ApplySort(m_sort_column, m_sort_order, row);
    }
}

void ItemsModel::OnBucketCollapsed(int row)
{
    if (m_search.GetViewMode() == Search::ViewMode::ByItem) {
        return; // the flat bucket is materialized by definition (D2 rule 6)
    }
    if (!m_search.has_bucket(row) || m_search.isFolderBucket(row)) {
        return;
    }
    m_search.MarkBucketCollapsed(row);
}

void ItemsModel::ApplySort(int column, Qt::SortOrder order, int only_bucket)
{
    struct ItemIndexSnapshot
    {
        QModelIndex from;
        int bucket_row;
        int column;
        std::shared_ptr<Item> item;
    };

    // Emit before snapshotting: listeners such as QItemSelectionModel create
    // persistent indexes inside their layoutAboutToBeChanged handlers, and
    // those must be included in the remapping below. A scoped sort names
    // its bucket in the parents list, so listeners save and restore state
    // for that subtree only instead of the whole model.
    QList<QPersistentModelIndex> parents;
    if (only_bucket >= 0) {
        parents.append(QPersistentModelIndex(BucketIndex(only_bucket)));
    }
    emit layoutAboutToBeChanged(parents, QAbstractItemModel::VerticalSortHint);

    std::vector<ItemIndexSnapshot> snapshots;
    const auto persistent_indexes = persistentIndexList();
    snapshots.reserve(persistent_indexes.size());
    for (const QModelIndex &persistent_index : persistent_indexes) {
        const RowRef ref = Locate(persistent_index);
        if (!ref.isItem()) {
            continue; // headers never move under a sort
        }

        const int bucket_row = ref.bucket_row;
        if ((only_bucket >= 0) && (bucket_row != only_bucket)) {
            continue; // a scoped sort moves rows in one bucket only
        }

        const Bucket &bucket = m_search.bucket(bucket_row);
        const int item_row = ref.item_row;
        if (!bucket.has_item(item_row)) {
            continue;
        }

        snapshots.push_back(
            {persistent_index, bucket_row, persistent_index.column(), bucket.item(item_row)});
    }

    if (only_bucket >= 0) {
        m_search.SortBucket(only_bucket, column, order);
    } else {
        m_search.Sort(column, order);
    }

    QModelIndexList from;
    QModelIndexList to;
    from.reserve(snapshots.size());
    to.reserve(snapshots.size());
    // Each bucket holding a snapshot is walked once (#272): its snapshots
    // are found by item as the walk meets them, at their item's first
    // row, and the walk stops when none is left.
    std::map<int, std::unordered_multimap<const Item *, size_t>> pending_by_bucket;
    for (size_t n = 0; n < snapshots.size(); ++n) {
        pending_by_bucket[snapshots[n].bucket_row].emplace(snapshots[n].item.get(), n);
    }
    auto &probes = ModelProbes::instance();
    for (auto &[bucket_row, pending] : pending_by_bucket) {
        if (!m_search.has_bucket(bucket_row)) {
            continue;
        }
        const auto &items = m_search.bucket(bucket_row).items();
        const QModelIndex parent_index = BucketIndex(bucket_row);
        for (size_t row = 0; (row < items.size()) && !pending.empty(); ++row) {
            if (probes.enabled) {
                ++probes.sort_remap_steps;
            }
            const auto [first, last] = pending.equal_range(items[row].get());
            for (auto it = first; it != last; ++it) {
                const ItemIndexSnapshot &snapshot = snapshots[it->second];
                from.push_back(snapshot.from);
                to.push_back(index(static_cast<int>(row), snapshot.column, parent_index));
            }
            pending.erase(first, last);
        }
    }
    changePersistentIndexList(from, to);
    emit layoutChanged(parents, QAbstractItemModel::VerticalSortHint);
    if (only_bucket < 0) {
        // A scoped expand-sort says nothing about the materialized set as
        // a whole; only the view-wide pass marks the model sorted.
        SetSorted(true);
    }
}

void ItemsModel::RepaintBuyoutCells(const BuyoutChangeSet &changes)
{
    // Batching rule 5 (M3 R1-6): Price/Date cells render buyout state
    // unconditionally, so the affected visible rows repaint on any buyout
    // batch regardless of which column sorts the view. The buyout-
    // dependent columns are a contiguous span (Price, Date), so one
    // dataChanged rectangle per affected bucket covers them.
    const auto &columns = m_search.columns();
    int first_column = -1;
    int last_column = -1;
    for (int n = 0; n < static_cast<int>(columns.size()); ++n) {
        if (columns[n]->buyoutDependent()) {
            if (first_column < 0) {
                first_column = n;
            }
            last_column = n;
        }
    }
    if (first_column < 0) {
        return;
    }

    // Group the affected item ids by their stable bucket key through the
    // visible-id index so only affected buckets are scanned: the scoped
    // pricing pass rides the delta path, which stays O(delta + affected
    // bucket) (M2 hard constraint). The flat By-Item bucket holds the
    // whole visible result and is scanned once — the same O(n) shape as
    // D4's stated merge exception. An affected id the index cannot fully
    // represent — duplicated across buckets mid-refresh, or the empty id
    // shared by id-less items — forces the same every-bucket scan, so
    // every visible occurrence repaints; unique indexed ids (the steady
    // state) keep the bucket-scoped fast path.
    const bool by_item = (m_search.GetViewMode() == Search::ViewMode::ByItem);
    bool scan_every_bucket = by_item || changes.everything;
    std::map<LocationInventory::Key, std::set<QString>> ids_by_bucket;
    if (!scan_every_bucket) {
        for (const QString &id : changes.item_ids) {
            if (m_search.visibleIdUnindexed(id)) {
                scan_every_bucket = true;
                ids_by_bucket.clear();
                break;
            }
            if (const auto item = m_search.visibleItemById(id)) {
                ids_by_bucket[LocationInventory::KeyFor(item->location())].insert(id);
            }
        }
    }

    const auto &buckets = m_search.buckets();
    const int bucket_count = static_cast<int>(buckets.size());
    for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
        const Bucket &bucket = buckets[bucket_row];
        const auto &items = bucket.items();
        const int item_count = static_cast<int>(items.size());
        const QModelIndex header = BucketIndex(bucket_row);

        // Whole-bucket repaint is reserved for the shapes where it is the
        // exact affected set: `everything`, and a tab-level change in
        // By-Tab, where the bucket IS the tab — its header renders the
        // tab buyout and every row can inherit it. The flat By-Item
        // bucket aggregates every tab, so a tab-level change resolves to
        // the affected tab's rows below (S5 review round 1 — never the
        // whole flat bucket), and its header renders no buyout.
        const bool whole_bucket = changes.everything
                                  || (!by_item
                                      && (changes.tab_ids.count(bucket.location().id()) > 0));
        if (whole_bucket) {
            if (!by_item) {
                emit dataChanged(header, header);
            }
            if (item_count > 0) {
                emit dataChanged(index(0, first_column, header),
                                 index(item_count - 1, last_column, header));
            }
            continue;
        }

        // Row-level repaint: affected rows are emitted as MAXIMAL
        // contiguous runs — O(affected runs) rectangles, never one
        // first-to-last span (S5 review round 1: with scattered affected
        // ids in the flat bucket, a spanning rectangle cost O(collection)
        // view-side work per priced delta — ~43 s at 1m).
        const std::set<QString> *bucket_ids = nullptr;
        if (!scan_every_bucket) {
            const auto it = ids_by_bucket.find(LocationInventory::KeyFor(bucket.location()));
            if (it == ids_by_bucket.end()) {
                continue; // fast path: no affected rows in this bucket
            }
            bucket_ids = &it->second;
        }
        const auto affected = [&](int n) {
            const Item &item = *items[n];
            if (bucket_ids) {
                return bucket_ids->count(item.id()) > 0;
            }
            if (changes.item_ids.count(item.id()) > 0) {
                return true;
            }
            return by_item && (changes.tab_ids.count(item.location().id()) > 0);
        };
        int run_first = -1;
        for (int n = 0; n <= item_count; ++n) {
            const bool hit = (n < item_count) && affected(n);
            if (hit && (run_first < 0)) {
                run_first = n;
            } else if (!hit && (run_first >= 0)) {
                emit dataChanged(index(run_first, first_column, header),
                                 index(n - 1, last_column, header));
                run_first = -1;
            }
        }
    }
}

QModelIndex ItemsModel::parent(const QModelIndex &index) const
{
    // bucket
    if (!index.isValid() || index.model() != this || index.internalId() == 0) {
        return QModelIndex();
    }
    // item: the internalId is the bucket's stable serial (M3 S4), so the
    // parent mapping survives top-level insert/remove/move operations.
    const int bucket_row = m_search.rowForSerial(index.internalId());
    if (bucket_row < 0) {
        return QModelIndex();
    }
    return BucketIndex(bucket_row);
}

QModelIndex ItemsModel::index(int row, int column, const QModelIndex &parent) const
{
    if ((row < 0) || (column < 0) || (parent.isValid() && parent.model() != this)
        || !hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    if (parent.isValid()) {
        // item, or a nested header under a folder: carry the parent
        // bucket's stable serial (nonzero by construction) so a child index
        // never encodes a display position (M3 S4, folder-tree D5).
        const RowRef ref = Locate(parent);
        if (!ref.isBucket()) {
            return QModelIndex();
        }
        return createIndex(row,
                           column,
                           static_cast<quintptr>(m_search.bucket(ref.bucket_row).serial()));
    } else {
        return createIndex(row, column, static_cast<quintptr>(0));
    }
}

void ItemsModel::BeginRemoveItemRows(int bucket_row, int first, int last)
{
    beginRemoveRows(BucketIndex(bucket_row), first, last);
}

void ItemsModel::BeginInsertItemRows(int bucket_row, int first, int last)
{
    beginInsertRows(BucketIndex(bucket_row), first, last);
}

void ItemsModel::BeginInsertBucketRow(int parent_bucket_row, int row)
{
    beginInsertRows(BucketIndex(parent_bucket_row), row, row);
}

void ItemsModel::BeginRemoveBucketRow(int parent_bucket_row, int row)
{
    beginRemoveRows(BucketIndex(parent_bucket_row), row, row);
}

bool ItemsModel::BeginMoveBucketRow(int from_parent_bucket_row,
                                    int from,
                                    int to_parent_bucket_row,
                                    int to)
{
    // beginMoveRows takes the destination in pre-move coordinates: moving
    // down within one parent, the row lands before to+1.
    const bool same_parent = (from_parent_bucket_row == to_parent_bucket_row);
    const int destination = (same_parent && (to > from)) ? (to + 1) : to;
    return beginMoveRows(BucketIndex(from_parent_bucket_row),
                         from,
                         from,
                         BucketIndex(to_parent_bucket_row),
                         destination);
}

void ItemsModel::EmitBucketMetadataChanged(int row)
{
    const QModelIndex header = BucketIndex(row);
    emit dataChanged(header, header);
}
