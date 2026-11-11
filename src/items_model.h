// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QAbstractItemModel>

#include "modelprobes.h"

class BuyoutManager;
class Search;
struct BuyoutChangeSet;

class ItemsModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    explicit ItemsModel(BuyoutManager &bo_manager, Search &search);

    // A model index resolved to the search's bucket handle (folder-tree
    // D4): a bucket header is (bucket row, -1), an item is (owning bucket
    // row, item row), anything else — invalid, foreign, or stale — is
    // (-1, -1). The item row is not range-checked; callers test
    // Bucket::has_item as before.
    struct RowRef
    {
        int bucket_row{-1};
        int item_row{-1};
        bool isBucket() const { return (bucket_row >= 0) && (item_row < 0); }
        bool isItem() const { return (bucket_row >= 0) && (item_row >= 0); }
    };
    // The two conversions every "bucket or item" decision goes through
    // (D4): never `index(bucket_row, 0)`, `index.row()` or
    // `index.parent().row()` directly. BucketIndex is invalid for an
    // out-of-range bucket row or column.
    QModelIndex BucketIndex(int bucket_row, int column = 0) const;
    RowRef Locate(const QModelIndex &index) const;

    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
    QModelIndex parent(const QModelIndex &index) const;
    QModelIndex index(int row, int column = 0, const QModelIndex &parent = QModelIndex()) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const;
    Qt::ItemFlags flags(const QModelIndex &index) const;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole);
    // The view-wide sort entry (header clicks, the post-refilter
    // indicator pass). This is also where D1's invalidation rules 2-3
    // act: switching the column evicts every resident key vector, and any
    // (column, order) change clears every bucket's sorted flag — then the
    // materialized buckets re-establish order (Search::Sort) and collapsed
    // ones defer to expansion.
    void sort(int column, Qt::SortOrder order);
    // Re-sorts on the current sort indicator unconditionally (M3 S2): a
    // buyout batch changes Price/Date order without touching the
    // indicator, so sort()'s already-sorted skip must not apply. The
    // caller (OnBuyoutsChanged) invalidates the affected buckets first
    // (R3-2), so the per-bucket flags make this exact.
    void Resort();
    // The single-bucket form (S3 review round 1): a batch whose affected
    // materialized set is one bucket scopes the whole layout operation —
    // signals, persistent-index walk, sort — to that bucket instead of
    // paying the view-wide dance.
    void ResortBucket(int row);
    // The view's expand/collapse signals, per bucket row (D2 rules
    // 2-3): expanding marks the bucket materialized and sorts it under
    // the layout-change protocol iff its flag is invalid; collapsing
    // unmarks it and evicts its keys (By-Item's flat bucket ignores
    // collapse — it is materialized by definition). Both are idempotent.
    // A folder bucket holds no items and materializes nothing either way
    // (folder-tree D9).
    void OnBucketExpanded(int row);
    void OnBucketCollapsed(int row);
    // Batching rule 5 (M3 R1-6): repaint the affected visible Price/Date
    // cells — and affected bucket headers, which render tab buyouts —
    // under any active sort column.
    void RepaintBuyoutCells(const BuyoutChangeSet &changes);

    // The S4 delta-operation protocol (D3): thin wrappers around the
    // protected begin/end pairs, driven by Search while it mutates the
    // bucket vectors — refresh-driven changes are row operations scoped
    // to the affected bucket, never a reset (noModelResetDuringRefresh).
    void BeginRemoveItemRows(int bucket_row, int first, int last);
    void EndRemoveItemRows() { endRemoveRows(); }
    void BeginInsertItemRows(int bucket_row, int first, int last);
    void EndInsertItemRows() { endInsertRows(); }
    // Bucket header rows (folder-tree D7): `parent_bucket_row` is the
    // folder bucket the header sits under, -1 for the top level; `row` is
    // the header's row within that parent.
    void BeginInsertBucketRow(int parent_bucket_row, int row);
    void EndInsertBucketRow() { endInsertRows(); }
    // Filtered searches hide empty buckets (S4 review round 1): a bucket
    // a delta emptied leaves as a row removal under its parent.
    void BeginRemoveBucketRow(int parent_bucket_row, int row);
    void EndRemoveBucketRow() { endRemoveRows(); }
    // Moves one bucket header — with its subtree — within its parent or
    // across parents (D8, S0: beginMoveRows between parents). `to` is the
    // header's row within the destination AFTER the move. Returns false
    // when Qt rejects the move (no-op moves included) — the caller must
    // then not mutate.
    bool BeginMoveBucketRow(int from_parent_bucket_row, int from, int to_parent_bucket_row, int to);
    void EndMoveBucketRow() { endMoveRows(); }
    // A metadata delta's header repaint: text, color, and the checkbox
    // column all live on the bucket row (R1-4).
    void EmitBucketMetadataChanged(int row);
    Qt::SortOrder GetSortOrder() const { return m_sort_order; }
    int GetSortColumn() const { return m_sort_column; }
    void SetSorted(bool val) { m_sorted = val; }
    void beginUpdate()
    {
        if (auto &probes = ModelProbes::instance(); probes.enabled) {
            ++probes.model_resets;
            ++probes.model_resets_by_model[this];
        }
        beginResetModel();
    }
    void endUpdate() { endResetModel(); }
    void refreshCheckStates();

private:
    // The layout-preserving sort machinery shared by sort(), Resort(),
    // and the expand path: snapshots persistent indexes, sorts the
    // search's buckets, remaps. `only_bucket` scopes the whole dance to
    // one bucket (sort-on-expand); -1 sorts the materialized set and
    // marks the model sorted.
    void ApplySort(int column, Qt::SortOrder order, int only_bucket = -1);

    BuyoutManager &m_bo_manager;
    Search &m_search;
    Qt::SortOrder m_sort_order;
    int m_sort_column;
    bool m_sorted;
};
