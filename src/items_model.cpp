// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "items_model.h"

#include "bucket.h"
#include "buyoutmanager.h"
#include "itemlocation.h"
#include "search.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/spikeprofile.h" // SPIKE(m3-sort-profile)
#include "util/util.h"

#include <algorithm>
#include <iterator>
#include <memory>
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

    + stash tab title (called "bucket" elsewhere)
    |- item
    |- item
      ...
    + another stash tab or character
    |- item
    |- item

    and so on
*/

int ItemsModel::rowCount(const QModelIndex &parent) const
{
    // Root element, contains buckets
    if (!parent.isValid()) {
        return static_cast<int>(m_search.buckets().size());
    }
    if ((parent.model() != this) || (parent.column() != 0)) {
        return 0;
    }
    // Bucket, contains elements
    if (!parent.parent().isValid()) {
        const int bucket_row = parent.row();
        if (m_search.has_bucket(bucket_row)) {
            return static_cast<int>(m_search.bucket(bucket_row).items().size());
        } else {
            return 0;
        }
    }
    // Element, contains nothing
    return 0;
}

int ItemsModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() && (parent.model() != this)) {
        return 0;
    }
    // Root element, contains buckets
    if (!parent.isValid()) {
        return static_cast<int>(m_search.columns().size());
    }
    // Bucket, contains elements
    if (!parent.parent().isValid()) {
        return static_cast<int>(m_search.columns().size());
    }
    // Element, contains nothing
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

    if (index.internalId() == 0) {
        if (index.column() > 0) {
            return QVariant();
        }

        const ItemLocation &location = m_search.GetTabLocation(index);
        if (role == Qt::CheckStateRole) {
            if (!location.IsValid()) {
                return QVariant();
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
            const auto bo = m_bo_manager.GetTab(location);
            if (bo.IsActive()) {
                title += QString(" [%1]").arg(bo.AsText());
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
    const int bucket_row = index.parent().row();
    if (m_search.has_bucket(bucket_row)) {
        const Bucket &bucket = m_search.bucket(bucket_row);
        const int item_row = index.row();
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
    if (index.column() == 0 && index.internalId() == 0) {
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

    if (role == Qt::CheckStateRole && index.column() == 0 && index.internalId() == 0) {
        const ItemLocation &location = m_search.GetTabLocation(index);
        if (!location.IsValid() || m_bo_manager.GetRefreshLocked(location)) {
            return false;
        }
        m_bo_manager.SetRefreshChecked(location, value.toBool());

        // It's possible that our tabs can have the same name.  Right now we don't have a
        // way to differentiate these tabs so indicate dataChanged event for each tab with
        // the same name as the current checked tab so the 'check' is properly updated in
        // the layout
        QString target_hash = location.id();
        auto row_count = rowCount();
        for (int i = 0; i < row_count; ++i) {
            auto match_index = this->index(i);
            if (m_search.GetTabLocation(match_index).id() == target_hash) {
                emit dataChanged(match_index, match_index, {Qt::CheckStateRole});
            }
        }
        return true;
    }
    return false;
}

void ItemsModel::refreshCheckStates()
{
    const int rows = rowCount();
    if (rows > 0) {
        emit dataChanged(index(0, 0), index(rows - 1, 0), {Qt::CheckStateRole});
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
    m_sort_order = order;
    m_sort_column = column;

    struct ItemIndexSnapshot
    {
        QModelIndex from;
        int bucket_row;
        int column;
        std::shared_ptr<Item> item;
    };

    const int64_t spike_t0 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    // Emit before snapshotting: listeners such as QItemSelectionModel create
    // persistent indexes inside their layoutAboutToBeChanged handlers, and
    // those must be included in the remapping below.
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);

    const int64_t spike_t1 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    std::vector<ItemIndexSnapshot> snapshots;
    const auto persistent_indexes = persistentIndexList();
    snapshots.reserve(persistent_indexes.size());
    for (const QModelIndex &persistent_index : persistent_indexes) {
        if (!persistent_index.isValid() || persistent_index.internalId() == 0) {
            continue;
        }

        const int bucket_row = static_cast<int>(persistent_index.internalId() - 1);
        if (!m_search.has_bucket(bucket_row)) {
            continue;
        }

        const Bucket &bucket = m_search.bucket(bucket_row);
        const int item_row = persistent_index.row();
        if (!bucket.has_item(item_row)) {
            continue;
        }

        snapshots.push_back(
            {persistent_index, bucket_row, persistent_index.column(), bucket.item(item_row)});
    }

    const int64_t spike_t2 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    m_search.Sort(column, order);

    const int64_t spike_t3 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    QModelIndexList from;
    QModelIndexList to;
    from.reserve(snapshots.size());
    to.reserve(snapshots.size());
    for (const ItemIndexSnapshot &snapshot : snapshots) {
        if (!m_search.has_bucket(snapshot.bucket_row)) {
            continue;
        }

        const Bucket &bucket = m_search.bucket(snapshot.bucket_row);
        const auto &items = bucket.items();
        const auto item = std::find(items.begin(), items.end(), snapshot.item);
        if (item == items.end()) {
            continue;
        }

        const int item_row = static_cast<int>(std::distance(items.begin(), item));
        const QModelIndex parent_index = index(snapshot.bucket_row, 0);
        from.push_back(snapshot.from);
        to.push_back(index(item_row, snapshot.column, parent_index));
    }
    changePersistentIndexList(from, to);
    const int64_t spike_t4 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
    SetSorted(true);

    // SPIKE(m3-sort-profile): section attribution for the model sort.
    {
        const int64_t spike_t5 = spike_profile::now_ns();
        spike_profile::sort_layout_about_ns = spike_t1 - spike_t0;
        spike_profile::sort_snapshot_ns = spike_t2 - spike_t1;
        spike_profile::sort_search_sort_ns = spike_t3 - spike_t2;
        spike_profile::sort_remap_ns = spike_t4 - spike_t3;
        spike_profile::sort_layout_changed_ns = spike_t5 - spike_t4;
        spike_profile::model_sort_total_ns = spike_t5 - spike_t0;
    }
}

QModelIndex ItemsModel::parent(const QModelIndex &index) const
{
    // bucket
    if (!index.isValid() || index.model() != this || index.internalId() == 0) {
        return QModelIndex();
    }
    // item
    const int bucket_row = static_cast<int>(index.internalId() - 1);
    if (!m_search.has_bucket(bucket_row)) {
        return QModelIndex();
    }
    return createIndex(bucket_row, 0, static_cast<quintptr>(0));
}

QModelIndex ItemsModel::index(int row, int column, const QModelIndex &parent) const
{
    if ((row < 0) || (column < 0) || (parent.isValid() && parent.model() != this)
        || !hasIndex(row, column, parent)) {
        return QModelIndex();
    }

    if (parent.isValid()) {
        // item, we pass parent's (bucket's) row through ID parameter
        return createIndex(row, column, static_cast<quintptr>(parent.row()) + 1);
    } else {
        return createIndex(row, column, static_cast<quintptr>(0));
    }
}
