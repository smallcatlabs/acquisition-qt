// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "bucket.h"
#include "column.h"
#include "filters/filterstate.h"
#include "item.h"
#include "items_model.h"
#include "util/util.h"

class BuyoutManager;
class FilterCatalog;
class ItemsModel;
class QModelIndex;

class Search
{
    Q_GADGET
public:
    enum class ViewMode : int { ByTab = 0, ByItem = 1 };
    Q_ENUM(ViewMode)

    Search(BuyoutManager &bo, const QString &caption, const FilterCatalog &catalog);
    ~Search();
    void FilterItems(const Items &items);
    const QString &caption() const { return m_caption; }
    const Items &items() const { return m_items; }
    const std::vector<std::unique_ptr<Column>> &columns() const { return m_columns; }
    ItemsModel &model() { return m_model; }
    const std::set<QString> &expandedHeaders() const { return m_expanded_property; }
    void setExpandedHeaders(std::set<QString> headers);
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
    void Sort(int column, Qt::SortOrder order);

    const FilterCatalog &catalog() const { return m_filter_catalog; }
    qsizetype filterStateCount() const { return static_cast<qsizetype>(m_filter_states.size()); }
    const FilterState &filterStateAt(qsizetype index) const;

    // The only way to change a filter state. Marks the search dirty when the
    // new state differs, so a state changed while this search is in the
    // background still forces a refilter when it is next shown.
    void setFilterState(qsizetype index, FilterState state);

    // SPIKE(S1-M2): D9 rule 1 — every delta marks every search items-dirty;
    // the flag is cleared by this search's own successful refilter and is
    // consumed by the same refilter-on-next-activation gate as m_states_dirty.
    bool itemsDirty() const { return m_items_dirty; }
    void setItemsDirty(bool dirty) { m_items_dirty = dirty; }

    // SPIKE(S1-M2): intersection half A — does this item pass the current
    // active filter set?
    bool MatchesActiveFilters(const Item &item) const;

    // SPIKE(S1-M2): intersection half B — fetch-source keys of the items in
    // the current filtered result, rebuilt by every refilter.
    const std::set<QString> &visibleSources() const { return m_visible_sources; }

    // SPIKE(S1-M2): duration of the last FilterItems pass (filter loop plus
    // the model reset), for the reset-latency record.
    qint64 lastFilterMs() const { return m_last_filter_ms; }

private:
    std::vector<Bucket> &active_buckets();

    BuyoutManager &m_bo_manager;

    // Catalog and filter states are index-aligned. MainWindow owns the catalog
    // and outlives every Search.
    const FilterCatalog &m_filter_catalog;
    std::vector<FilterState> m_filter_states;

    // True when a filter state changed since the last time this search
    // actually filtered. A tab change alone does not need a refilter; a tab
    // change after a state change does.
    bool m_states_dirty{false};

    // SPIKE(S1-M2): see itemsDirty() above.
    bool m_items_dirty{false};
    std::set<QString> m_visible_sources;
    qint64 m_last_filter_ms{0};

    std::vector<std::unique_ptr<Column>> m_columns;

    ItemsModel m_model;
    std::vector<Bucket> m_bucket_by_tab;
    std::vector<Bucket> m_bucket_by_item;

    QString m_caption;
    Items m_items;
    bool m_filtered;
    size_t m_filtered_item_count;
    std::set<QString> m_expanded_property;
    std::shared_ptr<Item> m_current_item;
    std::optional<ItemLocation> m_current_bucket;
    ViewMode m_current_mode;
    RefreshReason m_refresh_reason;
};

template<>
struct fmt::formatter<Search::ViewMode, char> : QtEnumFormatter<Search::ViewMode>
{};
