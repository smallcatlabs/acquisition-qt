// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "search.h"

#include <algorithm>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "bucket.h"
#include "buyoutmanager.h"
#include "column.h"
#include "filters/filtermatchers.h"
#include "filters/filterspec.h"
#include "items_model.h"
#include "modelprobes.h"
#include "util/fatalerror.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    // The By-Tab display order: rendered location order with the stable id
    // breaking positional ties, so two tabs colliding on one position
    // mid-refresh keep a deterministic order. Shared by the refilter's
    // bucket sort and the delta path's insertion/reposition (S4) — one
    // definition of display order.
    bool bucketDisplayLess(const Bucket &a, const Bucket &b)
    {
        const ItemLocation &la = a.location();
        const ItemLocation &lb = b.location();
        if (la < lb) {
            return true;
        }
        if (lb < la) {
            return false;
        }
        return la.id() < lb.id();
    }

} // namespace

Search::Search(BuyoutManager &bo_manager,
               const QString &caption,
               const FilterCatalog &catalog,
               const LocationInventory *location_inventory)
    : m_bo_manager(bo_manager)
    , m_location_inventory(location_inventory)
    , m_filter_catalog(catalog)
    , m_model(bo_manager, *this)
    , m_caption(caption)
    , m_filtered(false)
    , m_filtered_item_count(0)
    , m_current_mode(ViewMode::ByTab)
    , m_refresh_reason(RefreshReason::Unknown)
{
    using move_only = std::unique_ptr<Column>;
    move_only init[] = {std::make_unique<NameColumn>(),
                        std::make_unique<PriceColumn>(bo_manager),
                        std::make_unique<DateColumn>(bo_manager),
                        std::make_unique<PropertyColumn>("Q", "Quality"),
                        std::make_unique<PropertyColumn>("Stack", "Stack Size"),
                        std::make_unique<CorruptedColumn>(),
                        std::make_unique<CraftedColumn>(),
                        std::make_unique<EnchantedColumn>(),
                        std::make_unique<InfluncedColumn>(),
                        std::make_unique<PropertyColumn>("PD", "Physical Damage"),
                        std::make_unique<ElementalDamageColumn>(0),
                        std::make_unique<ElementalDamageColumn>(1),
                        std::make_unique<ElementalDamageColumn>(2),
                        std::make_unique<ChaosDamageColumn>(),
                        std::make_unique<PropertyColumn>("APS", "Attacks per Second"),
                        std::make_unique<DPSColumn>(),
                        std::make_unique<pDPSColumn>(),
                        std::make_unique<eDPSColumn>(),
                        std::make_unique<cDPSColumn>(),
                        std::make_unique<PropertyColumn>("Crit", "Critical Strike Chance"),
                        std::make_unique<PropertyColumn>("Ar", "Armour"),
                        std::make_unique<PropertyColumn>("Ev", "Evasion Rating"),
                        std::make_unique<PropertyColumn>("ES", "Energy Shield"),
                        std::make_unique<PropertyColumn>("B", "Chance to Block"),
                        std::make_unique<PropertyColumn>("Lvl", "Level"),
                        std::make_unique<ItemlevelColumn>()};
    m_columns = std::vector<move_only>(std::make_move_iterator(std::begin(init)),
                                       std::make_move_iterator(std::end(init)));

    m_filter_states.reserve(static_cast<size_t>(m_filter_catalog.size()));
    for (qsizetype index = 0; index < m_filter_catalog.size(); ++index) {
        const auto &spec = m_filter_catalog[index];
        FilterState state = MakeDefaultState(spec);
        const bool matchingState = std::visit(
            [&state](const auto &payload) {
                using Payload = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, TextPayload>) {
                    return std::holds_alternative<TextState>(state);
                } else if constexpr (std::is_same_v<Payload, ComboPayload>) {
                    return std::holds_alternative<ComboState>(state);
                } else if constexpr (std::is_same_v<Payload, MinMaxPayload>) {
                    return std::holds_alternative<MinMaxState>(state);
                } else if constexpr (std::is_same_v<Payload, ColorsPayload>) {
                    return std::holds_alternative<ColorsState>(state);
                } else if constexpr (std::is_same_v<Payload, BoolPayload>) {
                    return std::holds_alternative<BoolState>(state);
                } else {
                    return std::holds_alternative<ModsState>(state);
                }
            },
            spec.payload);
        Q_ASSERT(matchingState);
        m_filter_states.emplace_back(std::move(state));
    }
    Q_ASSERT(m_filter_states.size() == static_cast<size_t>(m_filter_catalog.size()));
}

Search::~Search() = default;

const FilterState &Search::filterStateAt(qsizetype index) const
{
    return m_filter_states.at(static_cast<size_t>(index));
}

void Search::setFilterState(qsizetype index, FilterState state)
{
    auto &current = m_filter_states.at(static_cast<size_t>(index));
    Q_ASSERT(current.index() == state.index());
    if (current == state) {
        return;
    }
    current = std::move(state);
    m_states_dirty = true;
}

void Search::setExpandedKeys(std::set<LocationInventory::Key> keys)
{
    m_expanded_keys = std::move(keys);
}

const std::vector<Bucket> &Search::buckets() const
{
    switch (m_current_mode) {
    case ViewMode::ByTab:
        return m_bucket_by_tab;
        break;
    case ViewMode::ByItem:
        return m_bucket_by_item;
        break;
    default:
        spdlog::error("Invalid view mode: {}", m_current_mode);
        return m_bucket_by_item;
        break;
    }
}

std::vector<Bucket> &Search::active_buckets()
{
    switch (m_current_mode) {
    case ViewMode::ByTab:
        return m_bucket_by_tab;
        break;
    case ViewMode::ByItem:
        return m_bucket_by_item;
        break;
    default:
        spdlog::error("Invalid view mode: {}", static_cast<int>(m_current_mode));
        return m_bucket_by_item;
        break;
    }
}

bool Search::has_bucket(int row) const
{
    return (row >= 0) && (row < static_cast<int>(buckets().size()));
}

const Bucket &Search::bucket(int row) const
{
    const auto &bucket_list = buckets();
    const int bucket_count = static_cast<int>(bucket_list.size());
    if ((row < 0) || (row >= bucket_count)) {
        const int mode = static_cast<std::underlying_type_t<Search::ViewMode>>(m_current_mode);
        const QString message
            = QString("Bucket row out of bounds: %1 bucket size: %2 mode: %3. Program will abort.")
                  .arg(QString::number(row), QString::number(bucket_count), QString::number(mode));
        FatalError(message);
    }
    return bucket_list[row];
}

const QModelIndex Search::index(const std::shared_ptr<Item> &item) const
{
    if (!item) {
        // Return an invalid index because there is no current item.
        return QModelIndex();
    }
    // Look for a bucket that matches the item's location. In ByItem mode
    // the single bucket has a null location and holds every visible item,
    // so it is always searched; a ByTab bucket must match the item's
    // stable display key (M2 D6 — never mutable header text or position).
    const auto &bucket_list = buckets();
    const auto item_key = LocationInventory::KeyFor(item->location());
    const int bucket_count = static_cast<int>(bucket_list.size());
    for (int row = 0; row < bucket_count; ++row) {
        // Check each search bucket against the item's location.
        const auto &bucket = bucket_list[row];
        if ((m_current_mode == ViewMode::ByItem)
            || (LocationInventory::KeyFor(bucket.location()) == item_key)) {
            // Check each item in the bucket.
            const QModelIndex parent = m_model.BucketIndex(row);
            const auto &items = bucket.items();
            const int item_count = static_cast<int>(items.size());
            for (int n = 0; n < item_count; ++n) {
                const auto &model_item = items[n];
                if (item == model_item) {
                    // Found the index of a match.
                    return m_model.index(n, 0, parent);
                }
            }
        }
    }
    // If we get here, that means the previously selected item is no
    // longer part of the current view.
    return QModelIndex();
}

void Search::Sort(int column, Qt::SortOrder order)
{
    const int column_count = static_cast<int>(m_columns.size());
    if ((column < 0) || (column >= column_count)) {
        return;
    }
    // D2: only visible order is paid for. The flat By-Item bucket is
    // always materialized (rule 6); a By-Tab bucket is materialized while
    // expanded. A materialized bucket whose flag is valid skips entirely;
    // collapsed buckets are left as the invalidation events flagged them.
    auto &col = *m_columns[column];
    const bool by_item = (m_current_mode == ViewMode::ByItem);
    for (auto &bucket : active_buckets()) {
        if ((by_item || bucket.expanded()) && !bucket.sorted()) {
            bucket.Sort(col, order);
        }
    }
}

void Search::SortBucket(int row, int column, Qt::SortOrder order)
{
    const int column_count = static_cast<int>(m_columns.size());
    auto &bucket_list = active_buckets();
    if ((column < 0) || (column >= column_count) || (row < 0)
        || (row >= static_cast<int>(bucket_list.size()))) {
        return;
    }
    bucket_list[static_cast<size_t>(row)].Sort(*m_columns[column], order);
}

void Search::MarkBucketExpanded(int row)
{
    auto &bucket_list = active_buckets();
    if ((row >= 0) && (row < static_cast<int>(bucket_list.size()))) {
        bucket_list[static_cast<size_t>(row)].SetExpanded(true);
    }
}

void Search::MarkBucketCollapsed(int row)
{
    auto &bucket_list = active_buckets();
    if ((row >= 0) && (row < static_cast<int>(bucket_list.size()))) {
        Bucket &bucket = bucket_list[static_cast<size_t>(row)];
        bucket.SetExpanded(false);
        // Collapse is a view event, not a model event (D2 rule 3): the
        // keys evict, the sorted order and flag persist.
        bucket.EvictKeys();
    }
}

void Search::EvictResidentKeys()
{
    for (auto &bucket : m_bucket_by_tab) {
        bucket.EvictKeys();
    }
    for (auto &bucket : m_bucket_by_item) {
        bucket.EvictKeys();
    }
}

void Search::HydrateFlatBucketKeys()
{
    if ((m_current_mode != ViewMode::ByItem) || m_bucket_by_item.empty()) {
        return;
    }
    const int column = m_model.GetSortColumn();
    if ((column < 0) || (column >= static_cast<int>(m_columns.size()))) {
        return;
    }
    m_bucket_by_item.front().EnsureResidentKeys(*m_columns[column]);
}

void Search::InvalidateAllOrder()
{
    for (auto &bucket : m_bucket_by_tab) {
        bucket.InvalidateOrder();
    }
    for (auto &bucket : m_bucket_by_item) {
        bucket.InvalidateOrder();
    }
}

std::vector<int> Search::InvalidateBuyoutOrder(const BuyoutChangeSet &changes, int column)
{
    std::vector<int> resort_rows;
    const int column_count = static_cast<int>(m_columns.size());
    if ((column < 0) || (column >= column_count)) {
        return resort_rows;
    }
    const Column &col = *m_columns[column];

    // Resolve the affected buckets through the visible-id index so the
    // scoped pricing pass stays O(delta + affected bucket); an id the
    // index cannot fully represent (duplicated across buckets, or the
    // empty id) forces the every-bucket path, the same fallback shape as
    // the rule-5 repaint. Only item-level buyouts feed Price/Date keys
    // (BuyoutManager::Get is item-keyed; tab prices reach items through
    // propagation's item-level Sets in the same batch), so tab ids alone
    // affect no order.
    bool every_bucket = changes.everything;
    std::set<LocationInventory::Key> affected_tabs;
    if (!every_bucket) {
        for (const QString &id : changes.item_ids) {
            if (visibleIdUnindexed(id)) {
                every_bucket = true;
                affected_tabs.clear();
                break;
            }
            if (const auto item = visibleItemById(id)) {
                affected_tabs.insert(LocationInventory::KeyFor(item->location()));
            }
        }
    }

    // Both view modes' buckets are covered: the inactive mode's flags
    // must not claim validity a later mode switch would trust. Only the
    // active mode's materialized affected buckets are returned — they are
    // the ones the batch reorders now.
    const bool by_item = (m_current_mode == ViewMode::ByItem);
    for (size_t row = 0; row < m_bucket_by_tab.size(); ++row) {
        Bucket &bucket = m_bucket_by_tab[row];
        const bool affected = every_bucket
                              || (affected_tabs.count(LocationInventory::KeyFor(bucket.location()))
                                  > 0);
        if (affected) {
            bucket.RebuildKeyEntries(col, changes.item_ids, every_bucket);
            bucket.InvalidateOrder();
            if (!by_item && bucket.expanded()) {
                resort_rows.push_back(static_cast<int>(row));
            }
        }
    }
    const bool flat_affected = every_bucket || !affected_tabs.empty();
    for (auto &bucket : m_bucket_by_item) {
        if (flat_affected) {
            bucket.RebuildKeyEntries(col, changes.item_ids, every_bucket);
            bucket.InvalidateOrder();
            if (by_item) {
                resort_rows.push_back(0);
            }
        }
    }
    return resort_rows;
}

void Search::FilterItems(const Items &items)
{
    spdlog::debug("FilterItems: reason({})", m_refresh_reason);

    // If we're just changing tabs we don't need to update anything, unless a
    // filter state changed since we last filtered. That happens when a form
    // edit writes through to this search and the debounced refresh lands on a
    // different one: the buckets and caption would otherwise stay stale.
    // The gate also tests items-dirty (M2 D9 rule 1): a streamed delta marks
    // every search, and the flag forces a refilter on next activation.
    if ((m_refresh_reason == RefreshReason::TabChanged) && !m_states_dirty && !m_items_dirty) {
        return;
    }

    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.refilters;
    }

    m_model.beginUpdate();

    // Create a temporary vector of only the filters that are
    // active, so we don't have to check every filter against
    // every item.
    std::vector<qsizetype> active_filters;
    active_filters.reserve(m_filter_states.size());
    for (qsizetype index = 0; index < static_cast<qsizetype>(m_filter_states.size()); ++index) {
        if (IsActive(m_filter_states.at(static_cast<size_t>(index)))) {
            active_filters.push_back(index);
        }
    }
    active_filters.shrink_to_fit();

    // Reset everything before starting to filter items. The visible
    // indexes are rebuilt from scratch below — the whole-collection
    // rebuild the index-rebuild probe counts (post-M3, deltas maintain
    // them incrementally and only this path increments it).
    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.index_rebuilds;
    }
    m_items.clear();
    // "Filtered" means ANY filter is active (the spec's July 31, 2026
    // post-freeze amendment at D2 rule 5), not the old "at least one
    // item was rejected" snapshot: the membership decisions hanging off
    // this flag (hidden empty buckets, default expansion) must be stable
    // across deltas, and the snapshot could be flipped by one delta — a
    // whole-view change no bucket-scoped operation can express. This
    // definition flips only at filter edits, which are full refilters by
    // construction (D6).
    m_filtered = !active_filters.empty();
    m_filtered_item_count = 0;
    m_visible_by_id.clear();
    m_unindexed_visible_ids.clear();
    m_duplicate_visible_ids.clear();
    m_empty_id_visible_count = 0;

    // A single bucket with null location is used to view all items at once.
    m_bucket_by_item.clear();
    m_bucket_by_item.emplace_back(ItemLocation());

    // Temporarily store items-by-tabs in a map keyed by the STABLE display
    // identity (M2 D6/R5-1): ItemLocation orders stash locations by
    // position, so keying on the location itself can split a moved tab into
    // old- and new-position buckets, file fresh items under a stale header,
    // or merge two different tabs whose positions collide mid-refresh. Each
    // bucket renders the freshest metadata seen for its key.
    std::map<LocationInventory::Key, Bucket> bucketed_tabs;

    // Try to minimize the number of times we have to loop over each item,
    // because some players have hundreds of thousands or millions of items.
    for (const auto &item : items) {
        // Start by assuming there is a match and run through evey
        // filter until we find that one that will filter out the
        // current item.
        bool matches = true;
        for (const qsizetype index : active_filters) {
            const auto &state = m_filter_states.at(static_cast<size_t>(index));
            if (!MatchesFilter(*item, m_filter_catalog[index], state)) {
                // Now that we know this item will be filtered out,
                // we don't need to check any more filters.
                matches = false;
                break;
            }
        }
        if (matches) {
            // This item passed through all the filters, so we can
            // add it to the list of items and total count.
            m_items.push_back(item);

            // Add this item to the "By Item" bucket.
            m_bucket_by_item.front().AddItem(item);

            // Add this item to the associated "By Tab" bucket.
            const ItemLocation &location = item->location();
            const auto key = LocationInventory::KeyFor(location);
            auto bucket_it = bucketed_tabs.find(key);
            if (bucket_it == bucketed_tabs.end()) {
                bucket_it = bucketed_tabs.emplace(key, Bucket(canonicalLocation(location))).first;
            }
            bucket_it->second.AddItem(item);

            // Record the stable identity for R6-3 reselection and the
            // count; the shared helper keeps refilter and delta
            // bookkeeping identical (S4).
            IndexInsertVisible(item);
        }
    }

    // We need to add empty tabs here as there are no items to force their addition
    // But only do so if no filters are active as we want to hide empty tabs when
    // filtering. The published tab list resolves through the canonical
    // inventory too, and tabs known only to the inventory (discovered
    // mid-refresh by an empty delta, R6-1) are included until the final
    // snapshot publishes them.
    if (!m_filtered) {
        for (auto &location : m_bo_manager.GetStashTabLocations()) {
            const auto key = LocationInventory::KeyFor(location);
            if (!bucketed_tabs.count(key)) {
                bucketed_tabs.emplace(key, Bucket(canonicalLocation(location)));
            }
        }
        if (m_location_inventory) {
            for (const auto &[key, location] : m_location_inventory->entries()) {
                if (!bucketed_tabs.count(key)) {
                    bucketed_tabs.emplace(key, Bucket(location));
                }
            }
        }
    }

    // Move the "By Tab" buckets into their final location, ordered by their
    // rendered locations — the map above is keyed for identity, not display
    // order. Stable ids break positional ties so two tabs colliding on one
    // position mid-refresh keep a deterministic order.
    m_bucket_by_tab.clear();
    m_bucket_by_tab.reserve(bucketed_tabs.size());
    for (auto &element : bucketed_tabs) {
        m_bucket_by_tab.emplace_back(std::move(element.second));
    }
    std::sort(m_bucket_by_tab.begin(), m_bucket_by_tab.end(), bucketDisplayLess);

    // Fresh buckets get fresh identities (S4): the reset invalidates every
    // persistent index, so serial continuity across a refilter would buy
    // nothing.
    AssignSerials();
    RebuildRowLookups();
    m_items_stale = false;
    m_flat_bucket_stale = false;
    m_tab_buckets_stale = false;

    // A filtered By-Tab search is default-expanded (D2 rule 5): mark the
    // fresh buckets materialized now, so the post-refilter sort pass
    // establishes every visible bucket's order eagerly in one pass and the
    // restore's expansion signals find valid flags. Unfiltered fresh
    // buckets start collapsed; RestoreViewExpansion's expand signals sort
    // exactly the restored ones.
    if ((m_current_mode == ViewMode::ByTab) && m_filtered) {
        for (auto &bucket : m_bucket_by_tab) {
            bucket.SetExpanded(true);
        }
    }

    // Let the model know that current sort order has been invalidated
    m_model.SetSorted(false);
    m_model.endUpdate();

    m_states_dirty = false;
    m_items_dirty = false; // a successful refilter clears its own flag (D9 rule 3)
}

const Items &Search::items() const
{
    if (m_items_stale) {
        // Lazily reconciled from the maintained side: the delta path
        // never pays an O(collection) pass, and only user-initiated
        // boundaries (mode switch) and tests read this. In By-Item mode
        // the flat bucket is the maintained structure (S5) — the By-Tab
        // side is stale until the next mode switch rebuilds it.
        m_items.clear();
        if (m_tab_buckets_stale) {
            if (!m_bucket_by_item.empty()) {
                const auto &flat = m_bucket_by_item.front().items();
                m_items.insert(m_items.end(), flat.begin(), flat.end());
            }
        } else {
            for (const auto &bucket : m_bucket_by_tab) {
                m_items.insert(m_items.end(), bucket.items().begin(), bucket.items().end());
            }
        }
        m_items_stale = false;
    }
    return m_items;
}

int Search::rowForSerial(std::uint64_t serial) const
{
    const auto it = m_row_by_serial.find(serial);
    return (it != m_row_by_serial.end()) ? it->second : -1;
}

int Search::rowForKey(const LocationInventory::Key &key) const
{
    if (m_current_mode == ViewMode::ByItem) {
        return (!m_bucket_by_item.empty()
                && (LocationInventory::KeyFor(m_bucket_by_item.front().location()) == key))
                   ? 0
                   : -1;
    }
    return FindBucketRow(key);
}

void Search::AssignSerials()
{
    for (auto &bucket : m_bucket_by_tab) {
        bucket.SetSerial(m_next_bucket_serial++);
    }
    for (auto &bucket : m_bucket_by_item) {
        bucket.SetSerial(m_next_bucket_serial++);
    }
}

void Search::RebuildRowLookups()
{
    // Serial map: the ACTIVE mode's rows (the model's child-index
    // identity). Key map: the By-Tab display buckets (the delta path's
    // stable-key lookup) — always By-Tab, because deltas only apply
    // there. Structural ops pay O(tabs) here by nature of row positions.
    m_row_by_serial.clear();
    const auto &bucket_list = buckets();
    for (size_t row = 0; row < bucket_list.size(); ++row) {
        m_row_by_serial[bucket_list[row].serial()] = static_cast<int>(row);
    }
    // The row mapping (folder-tree D4): every bucket is a top-level header.
    const int bucket_count = static_cast<int>(bucket_list.size());
    m_top_level_rows.resize(static_cast<size_t>(bucket_count));
    m_parent_rows.assign(static_cast<size_t>(bucket_count), -1);
    m_rows_in_parent.resize(static_cast<size_t>(bucket_count));
    for (int row = 0; row < bucket_count; ++row) {
        m_top_level_rows[static_cast<size_t>(row)] = row;
        m_rows_in_parent[static_cast<size_t>(row)] = row;
    }
    m_row_by_key.clear();
    for (size_t row = 0; row < m_bucket_by_tab.size(); ++row) {
        m_row_by_key[LocationInventory::KeyFor(m_bucket_by_tab[row].location())] = static_cast<int>(
            row);
    }
}

int Search::childBucketRow(int parent_bucket_row, int row) const
{
    if ((parent_bucket_row >= 0) || (row < 0)
        || (row >= static_cast<int>(m_top_level_rows.size()))) {
        return -1;
    }
    return m_top_level_rows[static_cast<size_t>(row)];
}

int Search::childBucketCount(int parent_bucket_row) const
{
    return (parent_bucket_row < 0) ? static_cast<int>(m_top_level_rows.size()) : 0;
}

int Search::parentBucketRow(int bucket_row) const
{
    if ((bucket_row < 0) || (bucket_row >= static_cast<int>(m_parent_rows.size()))) {
        return -1;
    }
    return m_parent_rows[static_cast<size_t>(bucket_row)];
}

int Search::rowInParent(int bucket_row) const
{
    if ((bucket_row < 0) || (bucket_row >= static_cast<int>(m_rows_in_parent.size()))) {
        return -1;
    }
    return m_rows_in_parent[static_cast<size_t>(bucket_row)];
}

int Search::FindBucketRow(const LocationInventory::Key &key) const
{
    const auto it = m_row_by_key.find(key);
    return (it != m_row_by_key.end()) ? it->second : -1;
}

void Search::IndexInsertVisible(const std::shared_ptr<Item> &item)
{
    m_filtered_item_count += item->count();
    const QString id = item->id();
    if (id.isEmpty()) {
        // Id-less items cannot be identity-tracked; the unindexed mark
        // sends consumers down the every-occurrence path.
        ++m_empty_id_visible_count;
        m_unindexed_visible_ids.insert(id);
        return;
    }
    const auto [it, inserted] = m_visible_by_id.emplace(id, item);
    if (!inserted) {
        // Mid-refresh divergence: one id visible twice. Track every
        // occurrence so a removal restores the survivor exactly.
        auto &occurrences = m_duplicate_visible_ids[id];
        if (occurrences.empty()) {
            occurrences.push_back(it->second);
        }
        occurrences.push_back(item);
        m_unindexed_visible_ids.insert(id);
    }
}

void Search::IndexRemoveVisible(const std::shared_ptr<Item> &item)
{
    m_filtered_item_count -= item->count();
    const QString id = item->id();
    if (id.isEmpty()) {
        if ((m_empty_id_visible_count > 0) && (--m_empty_id_visible_count == 0)) {
            m_unindexed_visible_ids.erase(id);
        }
        return;
    }
    const auto dup = m_duplicate_visible_ids.find(id);
    if (dup != m_duplicate_visible_ids.end()) {
        auto &occurrences = dup->second;
        const auto occurrence = std::find(occurrences.begin(), occurrences.end(), item);
        if (occurrence != occurrences.end()) {
            occurrences.erase(occurrence);
        }
        if (occurrences.size() == 1) {
            // Unique again: the survivor is the freshly-refiltered answer.
            m_visible_by_id[id] = occurrences.front();
            m_duplicate_visible_ids.erase(dup);
            m_unindexed_visible_ids.erase(id);
        } else if (!occurrences.empty()) {
            m_visible_by_id[id] = occurrences.front();
        } else {
            m_visible_by_id.erase(id);
            m_duplicate_visible_ids.erase(dup);
            m_unindexed_visible_ids.erase(id);
        }
    } else {
        const auto it = m_visible_by_id.find(id);
        if ((it != m_visible_by_id.end()) && (it->second == item)) {
            m_visible_by_id.erase(it);
        }
    }
}

int Search::ApplyBucketMetadata(int bucket_row, const ItemLocation &canonical, bool *model_changed)
{
    Bucket &bucket = m_bucket_by_tab[static_cast<size_t>(bucket_row)];
    const ItemLocation old_location = bucket.location();
    const bool rendered_changed = (old_location.GetHeader() != canonical.GetHeader())
                                  || (old_location.getR() != canonical.getR())
                                  || (old_location.getG() != canonical.getG())
                                  || (old_location.getB() != canonical.getB());
    bucket.SetLocation(canonical);

    // Reposition when display ordering changed (D3): the vector is sorted
    // by display order except possibly this bucket, so its destination is
    // found by walking outward from its current row.
    int target = bucket_row;
    while ((target > 0)
           && bucketDisplayLess(m_bucket_by_tab[static_cast<size_t>(bucket_row)],
                                m_bucket_by_tab[static_cast<size_t>(target - 1)])) {
        --target;
    }
    if (target == bucket_row) {
        while ((target + 1 < static_cast<int>(m_bucket_by_tab.size()))
               && bucketDisplayLess(m_bucket_by_tab[static_cast<size_t>(target + 1)],
                                    m_bucket_by_tab[static_cast<size_t>(bucket_row)])) {
            ++target;
        }
    }
    if ((target != bucket_row) && (m_current_mode == ViewMode::ByTab)
        && m_model.BeginMoveBucketRow(bucket_row, target)) {
        auto first = m_bucket_by_tab.begin();
        if (target < bucket_row) {
            std::rotate(first + target, first + bucket_row, first + bucket_row + 1);
        } else {
            std::rotate(first + bucket_row, first + bucket_row + 1, first + target + 1);
        }
        RebuildRowLookups();
        m_model.EndMoveBucketRow();
        bucket_row = target;
        if (model_changed) {
            *model_changed = true;
        }
    }
    if (rendered_changed && (m_current_mode == ViewMode::ByTab)) {
        m_model.EmitBucketMetadataChanged(bucket_row);
        if (model_changed) {
            *model_changed = true;
        }
    }
    return bucket_row;
}

int Search::InsertBucketRow(const ItemLocation &canonical, const Items &accepted)
{
    Bucket bucket(canonical);
    bucket.SetSerial(m_next_bucket_serial++);
    bucket.AddItems(accepted);

    int row = 0;
    while ((row < static_cast<int>(m_bucket_by_tab.size()))
           && bucketDisplayLess(m_bucket_by_tab[static_cast<size_t>(row)], bucket)) {
        ++row;
    }
    m_model.BeginInsertBucketRow(row);
    m_bucket_by_tab.insert(m_bucket_by_tab.begin() + row, std::move(bucket));
    RebuildRowLookups();
    m_model.EndInsertBucketRow();
    for (const auto &item : accepted) {
        IndexInsertVisible(item);
    }
    return row;
}

void Search::RemoveBucketRow(int bucket_row)
{
    m_model.BeginRemoveBucketRow(bucket_row);
    m_bucket_by_tab.erase(m_bucket_by_tab.begin() + bucket_row);
    RebuildRowLookups();
    m_model.EndRemoveBucketRow();
}

template<typename Predicate>
bool Search::RemoveBucketRows(int bucket_row, Predicate &&predicate)
{
    // The active mode's bucket (S5): the By-Tab display bucket the delta
    // resolved, or the By-Item flat bucket at row 0.
    Bucket &bucket = active_buckets()[static_cast<size_t>(bucket_row)];
    const auto &bucket_items = bucket.items();
    // Contiguous runs, removed back-to-front so earlier runs' rows stay
    // valid — O(runs) model operations (D3).
    struct Run
    {
        int first;
        int last;
    };
    std::vector<Run> runs;
    for (int row = 0; row < static_cast<int>(bucket_items.size()); ++row) {
        if (predicate(*bucket_items[static_cast<size_t>(row)])) {
            if (!runs.empty() && (runs.back().last == row - 1)) {
                runs.back().last = row;
            } else {
                runs.push_back({row, row});
            }
        }
    }
    for (auto run = runs.rbegin(); run != runs.rend(); ++run) {
        m_model.BeginRemoveItemRows(bucket_row, run->first, run->last);
        for (int row = run->first; row <= run->last; ++row) {
            IndexRemoveVisible(bucket_items[static_cast<size_t>(row)]);
        }
        bucket.RemoveRows(run->first, run->last - run->first + 1);
        m_model.EndRemoveItemRows();
    }
    return !runs.empty();
}

void Search::InsertArrivals(int bucket_row, const Items &accepted)
{
    Bucket &bucket = active_buckets()[static_cast<size_t>(bucket_row)];
    const int column = m_model.GetSortColumn();
    const bool sortable = (column >= 0) && (column < static_cast<int>(m_columns.size()));
    // Materialized: an expanded By-Tab bucket, or the By-Item flat bucket
    // by definition (D1/D2 rule 6).
    const bool materialized = (m_current_mode == ViewMode::ByItem) || bucket.expanded();

    if (materialized && bucket.sorted() && sortable) {
        // R2-2: the visible-bucket sorted merge (the same single merge
        // pass D4 rule 2 states for the flat bucket). Sorting the
        // arrivals alone cannot establish the bucket's global order when
        // sibling sources' rows interleave under the sort; merging them
        // into the retained rows' order does.
        const Column &col = *m_columns[column];
        bucket.EnsureResidentKeys(col); // key-consuming op hydrates first (R3-1)
        const Qt::SortOrder order = m_model.GetSortOrder();
        auto &probes = ModelProbes::instance();
        const auto less = [&probes, order](const ItemSortKey &lhs, const ItemSortKey &rhs) {
            if (probes.enabled) {
                ++probes.keyed_compares;
            }
            return (order == Qt::AscendingOrder) ? (lhs < rhs) : (rhs < lhs);
        };
        if (probes.enabled) {
            // The merge is this bucket's order-refresh event: fresh keyed
            // order as part of application (staleOrderNeverSurvivesDelta).
            ++probes.bucket_sorts;
            ++probes.bucket_sorts_by_location[LocationInventory::KeyFor(bucket.location())];
        }

        std::vector<std::pair<ItemSortKey, std::shared_ptr<Item>>> arrivals;
        arrivals.reserve(accepted.size());
        for (const auto &item : accepted) {
            arrivals.emplace_back(col.key(*item), item);
        }
        std::sort(arrivals.begin(), arrivals.end(), [&less](const auto &lhs, const auto &rhs) {
            return less(lhs.first, rhs.first);
        });

        // Insertion positions against the retained order, computed before
        // any insertion mutates the key vector; nondecreasing because the
        // arrivals are sorted.
        const auto &retained = bucket.residentKeys();
        std::vector<int> positions;
        positions.reserve(arrivals.size());
        for (const auto &arrival : arrivals) {
            positions.push_back(static_cast<int>(
                std::upper_bound(retained.begin(), retained.end(), arrival.first, less)
                - retained.begin()));
        }

        int offset = 0;
        size_t start = 0;
        while (start < arrivals.size()) {
            size_t end = start + 1;
            while ((end < arrivals.size()) && (positions[end] == positions[start])) {
                ++end;
            }
            Items run_items;
            std::vector<ItemSortKey> run_keys;
            run_items.reserve(end - start);
            run_keys.reserve(end - start);
            for (size_t n = start; n < end; ++n) {
                run_keys.push_back(std::move(arrivals[n].first));
                run_items.push_back(std::move(arrivals[n].second));
            }
            const int first = positions[start] + offset;
            const int last = first + static_cast<int>(end - start) - 1;
            m_model.BeginInsertItemRows(bucket_row, first, last);
            bucket.InsertRows(first, run_items, &run_keys);
            m_model.EndInsertItemRows();
            for (const auto &item : run_items) {
                IndexInsertVisible(item);
            }
            offset += static_cast<int>(end - start);
            start = end;
        }
    } else {
        // Collapsed (or unsortable): arrival-ordered append; the order
        // defers to expansion (D2, collapsedInvalidBucketResortsOnReexpand).
        const int first = static_cast<int>(bucket.items().size());
        const int last = first + static_cast<int>(accepted.size()) - 1;
        m_model.BeginInsertItemRows(bucket_row, first, last);
        bucket.InsertRows(first, accepted, nullptr);
        m_model.EndInsertItemRows();
        for (const auto &item : accepted) {
            IndexInsertVisible(item);
        }
        bucket.InvalidateOrder();
    }
}

Search::DeltaApplication Search::ApplyTabDelta(const ItemLocation &location, const Items &items)
{
    const FetchSourceKey source = FetchSourceKey::ForLocation(location);
    const auto delta_key = LocationInventory::KeyFor(location);
    const ItemLocation &canonical = canonicalLocation(location);

    // Filter the arrivals once. O(delta).
    Items accepted;
    accepted.reserve(items.size());
    for (const auto &item : items) {
        if (MatchesActiveFilters(*item)) {
            accepted.push_back(item);
        }
    }

    if (m_current_mode == ViewMode::ByItem) {
        // D4 (S5): the flat bucket's per-delta contract. The flat view
        // renders no per-tab metadata; the anchor lands when the By-Tab
        // side rebuilds at the next mode switch.
        return ApplyFlatDelta(source, accepted);
    }

    DeltaApplication result;
    int bucket_row = FindBucketRow(delta_key);
    if (bucket_row < 0) {
        // No bucket owns this stable key. One appears when arrivals pass
        // the filters, or metadata-only in an unfiltered search (R1-4's
        // new-empty-tab clause); a filtered search with nothing visible
        // has adjudicated "no visible change".
        if (accepted.empty() && m_filtered) {
            result.processed = true;
            return result;
        }
        result.inserted_bucket_row = InsertBucketRow(canonical, accepted);
        result.model_changed = true;
        if (!accepted.empty()) {
            m_items_stale = true;
            m_flat_bucket_stale = true;
            result.content_changed = true;
        }
        result.processed = true;
        return result;
    }

    // Metadata half first (R1-4): the anchor renders now, item
    // intersection notwithstanding; the content ops below then target the
    // bucket's settled row.
    bucket_row = ApplyBucketMetadata(bucket_row, canonical, &result.model_changed);

    // Content half: a source-scoped replace (R1-1) — remove exactly the
    // rows fetched from this delta's source, then insert the arrivals.
    const bool removed = RemoveBucketRows(bucket_row, [&source](const Item &item) {
        return FetchSourceKey::ForLocation(item.location()) == source;
    });
    if (!accepted.empty()) {
        InsertArrivals(bucket_row, accepted);
    }

    if (removed || !accepted.empty()) {
        m_items_stale = true;
        m_flat_bucket_stale = true;
        result.content_changed = true;
        result.model_changed = true;
    }

    // A filtered search hides empty buckets (S4 review round 1): a bucket
    // this delta emptied leaves the view, converging to the
    // freshly-refiltered state. Unfiltered searches keep the empty row
    // (emptyDeltaEmptiesBucketWithoutRemovingIt).
    if (m_filtered && m_bucket_by_tab[static_cast<size_t>(bucket_row)].items().empty()) {
        RemoveBucketRow(bucket_row);
        result.model_changed = true;
        result.processed = true;
        return result;
    }

    // Defensive: a visible bucket must never keep stale order past a
    // delta (staleOrderNeverSurvivesDelta). The merge path preserved
    // sortedness; the append path only runs on collapsed buckets — this
    // covers the expanded-but-invalid corner.
    Bucket &bucket = m_bucket_by_tab[static_cast<size_t>(bucket_row)];
    if (result.content_changed && bucket.expanded() && !bucket.sorted()) {
        m_model.ResortBucket(bucket_row);
    }

    result.processed = true;
    return result;
}

Search::DeltaApplication Search::ApplyFlatDelta(const FetchSourceKey &source, const Items &accepted)
{
    DeltaApplication result;
    if (m_bucket_by_item.empty()) {
        return result; // no flat bucket to apply to — fail-safe dirty (R1-7)
    }

    // D4 rule 2 under the A′ remedy (S5 review round 1): erase the
    // source's rows and merge the accepted arrivals in ONE O(n + d)
    // rebuild, notified as the same contiguous-run removeRows/insertRows
    // batches — never a per-run vector splice against the
    // collection-sized item and key vectors.
    Bucket &flat = m_bucket_by_item.front();
    const int column = m_model.GetSortColumn();
    const bool sortable = (column >= 0) && (column < static_cast<int>(m_columns.size()));
    const Column *merge_column = (sortable && flat.sorted()) ? m_columns[column].get() : nullptr;
    const Items removed_items = flat.ReplaceSourceRows(
        [&source](const Item &item) {
            return FetchSourceKey::ForLocation(item.location()) == source;
        },
        accepted,
        merge_column,
        m_model.GetSortOrder(),
        [this](int first, int last) { m_model.BeginRemoveItemRows(0, first, last); },
        [this] { m_model.EndRemoveItemRows(); },
        [this](int first, int last) { m_model.BeginInsertItemRows(0, first, last); },
        [this] { m_model.EndInsertItemRows(); });
    for (const auto &item : removed_items) {
        IndexRemoveVisible(item);
    }
    for (const auto &item : accepted) {
        IndexInsertVisible(item);
    }

    if (!removed_items.empty() || !accepted.empty()) {
        m_items_stale = true;
        result.content_changed = true;
        result.model_changed = true;
    }
    // Every delta can carry a fresh location anchor or discover a tab the
    // flat view cannot render; the By-Tab side is marked stale
    // unconditionally and the next mode switch rebuilds it from the flat
    // collection against the canonical inventory (fresh metadata by
    // construction).
    m_tab_buckets_stale = true;

    // Defensive: the flat bucket is always visible and must never keep
    // stale order past a delta (D4 rule 1, staleOrderNeverSurvivesDelta).
    // The merge path preserved sortedness; this covers the invalid corner
    // the append fallback would leave.
    if (result.content_changed && !flat.sorted()) {
        m_model.ResortBucket(0);
    }

    result.processed = true;
    return result;
}

Search::DeltaApplication Search::ApplyChildReconciliation(
    const ItemLocation &parent, const std::vector<FetchSourceKey> &expected)
{
    DeltaApplication result;
    const auto parent_key = LocationInventory::KeyFor(parent);
    if (m_current_mode == ViewMode::ByItem) {
        // D4/D3 (S5): the erase becomes row removals scoped to the
        // parent's rows WITHIN the flat bucket — items under the parent's
        // stable key whose fetch source is outside the expected set; other
        // tabs' rows are untouched even though they share the bucket.
        if (m_bucket_by_item.empty()) {
            return result; // fail-safe dirty (R1-7)
        }
        const std::set<FetchSourceKey> allowed(expected.begin(), expected.end());
        // Removal-only A′ replace: order and resident keys survive (the
        // compaction keeps them aligned), and the erase never splices
        // per run.
        const Items removed_items = m_bucket_by_item.front().ReplaceSourceRows(
            [&parent_key, &allowed](const Item &item) {
                if (LocationInventory::KeyFor(item.location()) != parent_key) {
                    return false;
                }
                return allowed.count(FetchSourceKey::ForLocation(item.location())) == 0;
            },
            Items{},
            nullptr,
            m_model.GetSortOrder(),
            [this](int first, int last) { m_model.BeginRemoveItemRows(0, first, last); },
            [this] { m_model.EndRemoveItemRows(); },
            [this](int first, int last) { m_model.BeginInsertItemRows(0, first, last); },
            [this] { m_model.EndInsertItemRows(); });
        for (const auto &item : removed_items) {
            IndexRemoveVisible(item);
        }
        if (!removed_items.empty()) {
            m_items_stale = true;
            result.content_changed = true;
            result.model_changed = true;
        }
        m_tab_buckets_stale = true; // the aggregate carries a fresh anchor too
        result.processed = true;
        return result;
    }

    int bucket_row = FindBucketRow(parent_key);
    if (bucket_row < 0) {
        result.processed = true; // nothing visible under the parent
        return result;
    }
    bucket_row = ApplyBucketMetadata(bucket_row, canonicalLocation(parent), &result.model_changed);

    // The erase becomes row removals scoped to the parent's bucket (D3):
    // already source-predicate-shaped — keys outside the expected set.
    const std::set<FetchSourceKey> allowed(expected.begin(), expected.end());
    const bool removed = RemoveBucketRows(bucket_row, [&allowed](const Item &item) {
        return allowed.count(FetchSourceKey::ForLocation(item.location())) == 0;
    });
    if (removed) {
        m_items_stale = true;
        m_flat_bucket_stale = true;
        result.content_changed = true;
        result.model_changed = true;
        // Same filtered-empty convergence as the content delta (S4
        // review round 1).
        if (m_filtered && m_bucket_by_tab[static_cast<size_t>(bucket_row)].items().empty()) {
            RemoveBucketRow(bucket_row);
        }
    }
    result.processed = true;
    return result;
}

Search::SnapshotReconciliation Search::ReconcileFinalSnapshot(const Items &published)
{
    SnapshotReconciliation result;
    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.final_reconciliations;
    }

    // The one accepted O(collection) pass per refresh (R1-2): decide
    // target membership for every published item. Deltas already filtered
    // their arrivals with the same states, so for a clean search the
    // accepted set reproduces the visible result and the diffs below find
    // only the snapshot-only mutations — deleted tabs, new listings, the
    // rebased metadata (M2 D6). One reserved pointer→state table carries
    // the whole diff (S6 review round 1): kAccepted marks target
    // membership, kRetained is set for rows the removal pass kept, and
    // "missing" falls out as accepted-but-never-retained — no second
    // collection-sized set, no per-bucket set churn.
    constexpr std::uint8_t kAccepted = 1;
    constexpr std::uint8_t kRetained = 2;
    std::unordered_map<const Item *, std::uint8_t> state;
    state.reserve(published.size());
    std::map<LocationInventory::Key, Items> target_items;
    const bool by_item = (m_current_mode == ViewMode::ByItem);
    for (const auto &item : published) {
        if (!MatchesActiveFilters(*item)) {
            continue;
        }
        state.emplace(item.get(), kAccepted);
        if (!by_item) {
            target_items[LocationInventory::KeyFor(item->location())].push_back(item);
        }
    }

    if (by_item) {
        if (m_bucket_by_item.empty()) {
            // No flat bucket to reconcile against (a never-populated
            // search) — the fail-safe direction keeps the flag dirty and
            // activation refilters (R1-7).
            return result;
        }
        // D4's flat-bucket grain: one A′ replace — erase the rows the
        // snapshot no longer publishes (or no longer accepts), merge the
        // accepted items the bucket does not hold. A clean search's rows
        // are the published objects, so both sides are empty and no model
        // operation runs.
        Bucket &flat = m_bucket_by_item.front();
        for (const auto &item : flat.items()) {
            const auto it = state.find(item.get());
            if (it != state.end()) {
                it->second |= kRetained;
            }
        }
        Items missing;
        for (const auto &item : published) {
            const auto it = state.find(item.get());
            if ((it != state.end()) && (it->second == kAccepted)) {
                missing.push_back(item);
            }
        }
        const int column = m_model.GetSortColumn();
        const bool sortable = (column >= 0) && (column < static_cast<int>(m_columns.size()));
        const Column *merge_column = (sortable && flat.sorted()) ? m_columns[column].get()
                                                                 : nullptr;
        const Items removed_items = flat.ReplaceSourceRows(
            [&state](const Item &item) { return state.find(&item) == state.end(); },
            missing,
            merge_column,
            m_model.GetSortOrder(),
            [this](int first, int last) { m_model.BeginRemoveItemRows(0, first, last); },
            [this] { m_model.EndRemoveItemRows(); },
            [this](int first, int last) { m_model.BeginInsertItemRows(0, first, last); },
            [this] { m_model.EndInsertItemRows(); });
        for (const auto &item : removed_items) {
            IndexRemoveVisible(item);
        }
        for (const auto &item : missing) {
            IndexInsertVisible(item);
        }
        if (!removed_items.empty() || !missing.empty()) {
            m_items_stale = true;
            result.content_changed = true;
            result.model_changed = true;
        }
        // The snapshot rebased every anchor and settled the tab list; the
        // By-Tab side rebuilds against the canonical inventory at the
        // next mode switch (S5).
        m_tab_buckets_stale = true;
        // Defensive: the flat bucket must never keep stale order past the
        // reconciliation (staleOrderNeverSurvivesDelta's corner).
        if (result.content_changed && !flat.sorted()) {
            m_model.ResortBucket(0);
        }
        m_items_dirty = false; // authoritative (R1-7)
        return result;
    }

    // Target display buckets: keys with accepted items, plus every
    // published tab for an unfiltered search (FilterItems' empty-bucket
    // rule — filtered searches still hide empty buckets). emplace keeps
    // FilterItems' precedence: item-derived canonical anchor first, then
    // the published tab list, then the canonical inventory.
    std::map<LocationInventory::Key, ItemLocation> target_buckets;
    for (const auto &[key, bucket_items] : target_items) {
        target_buckets.emplace(key, canonicalLocation(bucket_items.front()->location()));
    }
    if (!m_filtered) {
        for (const auto &location : m_bo_manager.GetStashTabLocations()) {
            target_buckets.emplace(LocationInventory::KeyFor(location),
                                   canonicalLocation(location));
        }
        if (m_location_inventory) {
            for (const auto &[key, location] : m_location_inventory->entries()) {
                target_buckets.emplace(key, location);
            }
        }
    }

    // Deleted buckets leave first — rows and bucket as one top-level
    // removal (the subtree goes with the row), indexes unwound per item.
    for (int row = static_cast<int>(m_bucket_by_tab.size()) - 1; row >= 0; --row) {
        const Bucket &bucket = m_bucket_by_tab[static_cast<size_t>(row)];
        if (target_buckets.count(LocationInventory::KeyFor(bucket.location())) > 0) {
            continue;
        }
        if (!bucket.items().empty()) {
            // Item rows leave with the bucket; an empty deleted bucket is
            // a model-only change (S6 review round 1 — it must not mark
            // the flat bucket stale).
            result.content_changed = true;
        }
        for (const auto &item : bucket.items()) {
            IndexRemoveVisible(item);
        }
        RemoveBucketRow(row);
        result.model_changed = true;
    }

    // Metadata refreshed against the rebased locations: identity is the
    // stable key, so only rendered attributes change here.
    for (size_t row = 0; row < m_bucket_by_tab.size(); ++row) {
        Bucket &bucket = m_bucket_by_tab[row];
        const ItemLocation &canonical = target_buckets.at(
            LocationInventory::KeyFor(bucket.location()));
        const ItemLocation old_location = bucket.location();
        const bool rendered_changed = (old_location.GetHeader() != canonical.GetHeader())
                                      || (old_location.getR() != canonical.getR())
                                      || (old_location.getG() != canonical.getG())
                                      || (old_location.getB() != canonical.getB());
        bucket.SetLocation(canonical);
        if (rendered_changed) {
            m_model.EmitBucketMetadataChanged(static_cast<int>(row));
            result.model_changed = true;
        }
    }

    // Bucket order corrected via move ops. bucketDisplayLess is a TOTAL
    // order (stable-id tiebreak), so "no move needed" is exactly
    // is_sorted — the clean snapshot pays one O(tabs) scan (S6 review
    // round 1: the unguarded selection pass was O(tabs²) on every clean
    // snapshot, against the 2000+-tab driving use case). The selection
    // pass survives as the rare displaced-path mechanism only.
    if (!std::is_sorted(m_bucket_by_tab.begin(), m_bucket_by_tab.end(), bucketDisplayLess)) {
        const int bucket_count = static_cast<int>(m_bucket_by_tab.size());
        for (int row = 0; row < bucket_count; ++row) {
            int best = row;
            for (int n = row + 1; n < bucket_count; ++n) {
                if (bucketDisplayLess(m_bucket_by_tab[static_cast<size_t>(n)],
                                      m_bucket_by_tab[static_cast<size_t>(best)])) {
                    best = n;
                }
            }
            if ((best != row) && m_model.BeginMoveBucketRow(best, row)) {
                auto first = m_bucket_by_tab.begin();
                std::rotate(first + row, first + best, first + best + 1);
                RebuildRowLookups();
                m_model.EndMoveBucketRow();
                result.model_changed = true;
            }
        }
    }

    // Newly listed buckets, at their display positions (the vector is
    // display-ordered after the pass above, so InsertBucketRow's walk is
    // exact). Rows shift as later insertions land, so track serials and
    // resolve them to final rows at the end.
    std::vector<std::uint64_t> inserted_serials;
    static const Items no_items;
    for (const auto &[key, canonical] : target_buckets) {
        if (FindBucketRow(key) >= 0) {
            continue;
        }
        const auto it = target_items.find(key);
        const Items &bucket_items = (it != target_items.end()) ? it->second : no_items;
        const int row = InsertBucketRow(canonical, bucket_items);
        inserted_serials.push_back(m_bucket_by_tab[static_cast<size_t>(row)].serial());
        result.model_changed = true;
        if (!bucket_items.empty()) {
            result.content_changed = true;
        }
    }

    // The row-level diff, by item identity against the published objects
    // (the worker publishes the Item objects the deltas delivered, so a
    // clean search's surviving buckets diff to nothing; content a skipped
    // delta left stale is replaced here — which is what licenses the
    // dirty-flag clear below). The removal predicate is per stable key,
    // not merely global membership (S6 review round 1): a row is retained
    // only when its item is accepted AND belongs under this bucket's key,
    // so an object parked in the wrong bucket (reachable through
    // ApplyTabDelta, whose insertions target the delta anchor) is removed
    // here and re-inserted under its own key — never duplicated. That
    // guarantee is also what makes the kRetained mark sound: retained ⇒
    // in exactly its own key's bucket.
    for (int row = 0; row < static_cast<int>(m_bucket_by_tab.size()); ++row) {
        const ItemLocation &bucket_location = m_bucket_by_tab[static_cast<size_t>(row)].location();
        bool bucket_changed = RemoveBucketRows(row, [&state, &bucket_location](const Item &item) {
            if (state.find(&item) == state.end()) {
                return true;
            }
            const ItemLocation &location = item.location();
            return (location.type() != bucket_location.type())
                   || (location.id() != bucket_location.id());
        });
        const auto it = target_items.find(
            LocationInventory::KeyFor(m_bucket_by_tab[static_cast<size_t>(row)].location()));
        if (it != target_items.end()) {
            const Bucket &bucket = m_bucket_by_tab[static_cast<size_t>(row)];
            for (const auto &item : bucket.items()) {
                const auto entry = state.find(item.get());
                if (entry != state.end()) {
                    entry->second |= kRetained;
                }
            }
            Items missing;
            for (const auto &item : it->second) {
                if (state.at(item.get()) == kAccepted) {
                    missing.push_back(item);
                }
            }
            if (!missing.empty()) {
                InsertArrivals(row, missing);
                bucket_changed = true;
            }
        }
        if (bucket_changed) {
            result.content_changed = true;
            result.model_changed = true;
            // Defensive, like the delta path: a visible bucket never keeps
            // stale order past the reconciliation.
            const Bucket &bucket = m_bucket_by_tab[static_cast<size_t>(row)];
            if (bucket.expanded() && !bucket.sorted()) {
                m_model.ResortBucket(row);
            }
        }
    }

    if (result.content_changed) {
        m_items_stale = true;
        m_flat_bucket_stale = true;
    }
    m_items_dirty = false; // authoritative (R1-7)

    for (const std::uint64_t serial : inserted_serials) {
        const int row = rowForSerial(serial);
        if (row >= 0) {
            result.inserted_bucket_rows.push_back(row);
        }
    }
    std::sort(result.inserted_bucket_rows.begin(), result.inserted_bucket_rows.end());
    return result;
}

bool Search::MatchesActiveFilters(const Item &item) const
{
    for (qsizetype index = 0; index < static_cast<qsizetype>(m_filter_states.size()); ++index) {
        const auto &state = m_filter_states.at(static_cast<size_t>(index));
        if (IsActive(state) && !MatchesFilter(item, m_filter_catalog[index], state)) {
            return false;
        }
    }
    return true;
}

const ItemLocation &Search::canonicalLocation(const ItemLocation &embedded) const
{
    return m_location_inventory ? m_location_inventory->Canonical(embedded) : embedded;
}

void Search::RenameCaption(const QString &newName)
{
    m_caption = newName;
}

QString Search::GetCaption() const
{
    return QString("%1 [%2]").arg(m_caption).arg(m_filtered_item_count);
}

ItemLocation Search::GetTabLocation(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return ItemLocation();
    }
    const ItemsModel::RowRef ref = m_model.Locate(index);
    if (ref.isItem()) {
        // If index represents an item, get location from item as view may be on 'item' view
        // where bucket location doesn't match items location
        const Bucket &b = bucket(ref.bucket_row);
        if (b.has_item(ref.item_row)) {
            return b.item(ref.item_row)->location();
        } else {
            spdlog::warn("GetTabLocation(): parent bucket {} does not have {} items",
                         ref.bucket_row,
                         ref.item_row);
        }
    } else if (ref.isBucket()) {
        // Otherwise index represents a tab already, get location from there
        return bucket(ref.bucket_row).location();
    } else {
        spdlog::warn("GetTabLocation(): index (row {}) resolves to no bucket", index.row());
    }
    return ItemLocation();
}

void Search::SetViewMode(ViewMode mode)
{
    if (mode == m_current_mode) {
        return;
    }
    m_model.beginUpdate();
    // Leaving a mode dematerializes its buckets: keys evict, orders and
    // flags persist (D1/R2-3 — a view event, like collapse).
    for (auto &bucket : active_buckets()) {
        bucket.EvictKeys();
    }
    m_current_mode = mode;
    // An items-dirty search skips the rebuilds and sort: its whole
    // collection is stale (application was skipped — the R1-7 fail-safe
    // direction), so working from it would present un-applied state. The
    // caller's mode-switch refilter (D6 boundary, MainWindow)
    // re-establishes everything from fresh state immediately after.
    if (!m_items_dirty) {
        if (mode == ViewMode::ByItem) {
            // A flat bucket gone stale (By-Tab deltas maintained the tab
            // buckets) rebuilds here from the maintained collection — the
            // mode switch is one of D6's honest full-rebuild boundaries.
            if (m_flat_bucket_stale && !m_bucket_by_item.empty()) {
                Bucket &flat = m_bucket_by_item.front();
                Bucket rebuilt{ItemLocation()};
                rebuilt.SetSerial(flat.serial());
                rebuilt.AddItems(items());
                flat = std::move(rebuilt);
                m_flat_bucket_stale = false;
            }
            // The flat bucket is always materialized (D2 rule 6):
            // establish its order now if invalid — the reset leaves
            // nothing else to sort it before it paints.
            const int column = m_model.GetSortColumn();
            if (!m_bucket_by_item.empty() && !m_bucket_by_item.front().sorted() && (column >= 0)
                && (column < static_cast<int>(m_columns.size()))) {
                m_bucket_by_item.front().Sort(*m_columns[column], m_model.GetSortOrder());
            }
            // D4 rule 1 / R3-1 carve-out (S5): keys resident whenever the
            // search is active. The sort above hydrated; a valid flag
            // skipped it, leaving sorted-but-keyless — hydrate now.
            HydrateFlatBucketKeys();
        } else if (m_tab_buckets_stale) {
            // By-Item deltas maintained the flat bucket only (S5): the
            // By-Tab side rebuilds from the maintained collection —
            // membership is unchanged, so this is D6's honest rebuild
            // minus the filter loop. Arriving buckets start collapsed
            // with invalid flags; the caller's expansion restore sorts
            // exactly the buckets it expands (D2 rule 2).
            RebuildTabBucketsFromFlat();
        }
    }
    RebuildRowLookups();
    // Arriving By-Tab buckets start collapsed after the reset; expansion
    // restore sorts the ones whose flags are invalid (D2 rule 2).
    m_model.SetSorted(true);
    m_model.endUpdate();
}

void Search::RebuildTabBucketsFromFlat()
{
    // The same bucketing FilterItems performs, minus the filter loop:
    // membership is exactly the flat bucket's contents, grouped by the
    // stable display key and rendered against the canonical inventory —
    // so metadata deltas that arrived while By-Item was active land here.
    std::map<LocationInventory::Key, Bucket> bucketed_tabs;
    if (!m_bucket_by_item.empty()) {
        for (const auto &item : m_bucket_by_item.front().items()) {
            const ItemLocation &location = item->location();
            const auto key = LocationInventory::KeyFor(location);
            auto bucket_it = bucketed_tabs.find(key);
            if (bucket_it == bucketed_tabs.end()) {
                bucket_it = bucketed_tabs.emplace(key, Bucket(canonicalLocation(location))).first;
            }
            bucket_it->second.AddItem(item);
        }
    }

    // Unfiltered searches show empty tabs, published and inventory-known
    // alike (the FilterItems rule, R6-1).
    if (!m_filtered) {
        for (auto &location : m_bo_manager.GetStashTabLocations()) {
            const auto key = LocationInventory::KeyFor(location);
            if (!bucketed_tabs.count(key)) {
                bucketed_tabs.emplace(key, Bucket(canonicalLocation(location)));
            }
        }
        if (m_location_inventory) {
            for (const auto &[key, location] : m_location_inventory->entries()) {
                if (!bucketed_tabs.count(key)) {
                    bucketed_tabs.emplace(key, Bucket(location));
                }
            }
        }
    }

    m_bucket_by_tab.clear();
    m_bucket_by_tab.reserve(bucketed_tabs.size());
    for (auto &element : bucketed_tabs) {
        m_bucket_by_tab.emplace_back(std::move(element.second));
    }
    std::sort(m_bucket_by_tab.begin(), m_bucket_by_tab.end(), bucketDisplayLess);
    for (auto &bucket : m_bucket_by_tab) {
        bucket.SetSerial(m_next_bucket_serial++);
    }
    m_tab_buckets_stale = false;
}
