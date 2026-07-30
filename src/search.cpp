// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "search.h"

#include <memory>
#include <type_traits>

#include "bucket.h"
#include "buyoutmanager.h"
#include "column.h"
#include "filters/filtermatchers.h"
#include "filters/filterspec.h"
#include "items_model.h"
#include "util/fatalerror.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/spikeprofile.h" // SPIKE(m3-sort-profile)

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
            const QModelIndex parent = m_model.index(row);
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
    if ((column >= 0) && (column < column_count)) {
        auto &col = *m_columns[column];
        for (auto &bucket : active_buckets()) {
            // SPIKE(m3-sort-profile): per-bucket sort attribution.
            const int64_t t0 = spike_profile::now_ns();
            bucket.Sort(col, order);
            spike_profile::bucket_sort_samples.emplace_back(static_cast<int>(bucket.items().size()),
                                                            spike_profile::now_ns() - t0);
        }
    }
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

    // SPIKE(m3-sort-profile): refilter split — begin reset / filter loop /
    // bucket ordering / end reset.
    const int64_t spike_t0 = spike_profile::now_ns();

    m_model.beginUpdate();

    const int64_t spike_t1 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

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

    // Reset everything before starting to filter items.
    m_items.clear();
    m_filtered = false;
    m_filtered_item_count = 0;
    m_visible_sources.clear();
    m_visible_sources_by_tab.clear();
    m_visible_by_id.clear();

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
                m_filtered = true;
                break;
            }
        }
        if (matches) {
            // This item passed through all the filters, so we can
            // add it to the list of items and total count.
            m_items.push_back(item);
            m_filtered_item_count += item->count();

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

            // Record the fetch source for the D9 intersection tests.
            const FetchSourceKey source_key = FetchSourceKey::ForLocation(location);
            m_visible_sources.insert(source_key);
            m_visible_sources_by_tab[key].insert(source_key);

            // Record the stable identity for R6-3 reselection. Items
            // without a server id cannot be identity-tracked and fall back
            // to pointer reselection.
            if (const QString item_id = item->id(); !item_id.isEmpty()) {
                m_visible_by_id.emplace(item_id, item);
            }
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

    const int64_t spike_t2 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    // Move the "By Tab" buckets into their final location, ordered by their
    // rendered locations — the map above is keyed for identity, not display
    // order. Stable ids break positional ties so two tabs colliding on one
    // position mid-refresh keep a deterministic order.
    m_bucket_by_tab.clear();
    m_bucket_by_tab.reserve(bucketed_tabs.size());
    for (auto &element : bucketed_tabs) {
        m_bucket_by_tab.emplace_back(std::move(element.second));
    }
    std::sort(m_bucket_by_tab.begin(), m_bucket_by_tab.end(), [](const Bucket &a, const Bucket &b) {
        const ItemLocation &la = a.location();
        const ItemLocation &lb = b.location();
        if (la < lb) {
            return true;
        }
        if (lb < la) {
            return false;
        }
        return la.id() < lb.id();
    });

    const int64_t spike_t3 = spike_profile::now_ns(); // SPIKE(m3-sort-profile)

    // Let the model know that current sort order has been invalidated
    m_model.SetSorted(false);
    m_model.endUpdate();

    // SPIKE(m3-sort-profile): record the split.
    {
        const int64_t spike_t4 = spike_profile::now_ns();
        spike_profile::filter_begin_reset_ns = spike_t1 - spike_t0;
        spike_profile::filter_loop_ns = spike_t2 - spike_t1;
        spike_profile::filter_bucket_order_ns = spike_t3 - spike_t2;
        spike_profile::filter_end_reset_ns = spike_t4 - spike_t3;
        spike_profile::filter_total_ns = spike_t4 - spike_t0;
    }

    m_states_dirty = false;
    m_items_dirty = false; // a successful refilter clears its own flag (D9 rule 3)
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

bool Search::HasVisibleGhostUnder(const ItemLocation &parent,
                                  const std::vector<FetchSourceKey> &expected) const
{
    const auto it = m_visible_sources_by_tab.find(LocationInventory::KeyFor(parent));
    if (it == m_visible_sources_by_tab.end()) {
        return false;
    }
    const std::set<FetchSourceKey> allowed(expected.begin(), expected.end());
    for (const auto &key : it->second) {
        if (allowed.count(key) == 0) {
            return true;
        }
    }
    return false;
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
    if (index.internalId() > 0) {
        // If index represents an item, get location from item as view may be on 'item' view
        // where bucket location doesn't match items location
        const int bucket_row = index.parent().row();
        if (has_bucket(bucket_row)) {
            const Bucket &b = bucket(bucket_row);
            const int item_row = index.row();
            if (b.has_item(item_row)) {
                return b.item(item_row)->location();
            } else {
                spdlog::warn("GetTabLocation(): parent bucket {} does not have {} items",
                             bucket_row,
                             item_row);
            }
        } else {
            spdlog::warn("GetTabLocation(): parent bucket {} does not exist", bucket_row);
        }
    } else {
        // Otherwise index represents a tab already, get location from there
        const int bucket_row = index.row();
        if (has_bucket(bucket_row)) {
            return bucket(bucket_row).location();
        } else {
            spdlog::warn("GetTabLocation(): bucket {} does not exist", bucket_row);
        }
    }
    return ItemLocation();
}

void Search::SetViewMode(ViewMode mode)
{
    if (mode != m_current_mode) {
        m_model.beginUpdate();
        m_current_mode = mode;
        Sort(m_model.GetSortColumn(), m_model.GetSortOrder());
        m_model.SetSorted(true);
        m_model.endUpdate();
    }
}
