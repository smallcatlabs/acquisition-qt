// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "itemsmanager.h"

#include <QSettings>

#include <optional>
#include <set>

#include "buyoutmanager.h"
#include "item.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

ItemsManager::ItemsManager(QSettings &settings, BuyoutManager &buyout_manager)
    : m_settings(settings)
    , m_buyout_manager(buyout_manager)
    , m_auto_update_timer(std::make_unique<QTimer>())
{
    spdlog::trace("ItemsManager::ItemsManager() entered");

    const int interval = m_settings.value("autoupdate_interval", 30).toInt();
    m_auto_update_timer->setSingleShot(false);
    m_auto_update_timer->setInterval(interval * 60 * 1000);
    connect(m_auto_update_timer.get(), &QTimer::timeout, this, &ItemsManager::OnAutoRefreshTimer);

    const bool autoupdate = m_settings.value("autoupdate", false).toBool();
    if (autoupdate) {
        m_auto_update_timer->start();
    }
}

ItemsManager::~ItemsManager() {}

void ConnectRefreshRequests(ItemsManager &manager, ItemsManagerWorker &worker)
{
    QObject::connect(&manager, &ItemsManager::UpdateSignal, &worker, &ItemsManagerWorker::Update);
    QObject::connect(&manager,
                     &ItemsManager::AutoUpdateSignal,
                     &worker,
                     &ItemsManagerWorker::AutoUpdate);
}

void ItemsManager::OnStatusUpdate(ProgramState state, const QString &status)
{
    emit StatusUpdate(state, status);
}

void ItemsManager::PropagateTabBuyouts()
{
    spdlog::trace("ItemsManager::PropagateTabBuyouts() entered");
    // A tab's price is no longer copied onto its items (#293): an item
    // without its own price shows its tab's when it is read
    // (BuyoutManager::Effective). What is left of this final pass is the
    // refresh locks, recomputed from the prices in force.
    m_buyout_manager.ClearRefreshLocks();
    // A tab whose own price requires a refresh is locked from the tab list,
    // so a priced tab with no loaded items (empty, or not fetched yet) is
    // refreshed too (#148). Remove-only tabs are skipped, as below, and so
    // are folders, which hold no items and cannot be priced (D11).
    for (const auto &tab : m_buyout_manager.GetStashTabLocations()) {
        if (!tab.removeonly() && !tab.IsFolder()
            && m_buyout_manager.EffectiveTab(tab).buyout.RequiresRefresh()) {
            m_buyout_manager.SetRefreshLocked(tab);
        }
    }
    // Skip remove-only tabs because they are not editable, nor indexed for
    // trade now.
    for (const auto &item : m_items.Flat()) {
        if (!item->location().removeonly() && PriceRequiresRefresh(*item)) {
            m_buyout_manager.SetRefreshLocked(item->location());
        }
    }
}

bool ItemsManager::PriceRequiresRefresh(const Item &item) const
{
    return m_buyout_manager.Effective(item).buyout.RequiresRefresh()
           || m_buyout_manager.EffectiveTab(item.location()).buyout.RequiresRefresh();
}

void ItemsManager::OnItemsRefreshed(const Items &items,
                                    const std::vector<ItemLocation> &tabs,
                                    bool initial_refresh)
{
    spdlog::trace("ItemsManager::OnItemsRefreshed() entered");
    m_items.ResetTo(items);

    spdlog::debug("There are {} items and {} tabs after the refresh.", m_items.size(), tabs.size());
    // Debug-only diagnostic, gated so release users never pay a
    // whole-collection scan for a log they cannot see (F46, absorbed by M2
    // per R1-9). The scan never runs on the delta path either way.
    if (spdlog::should_log(spdlog::level::debug)) {
        int n = 0;
        for (const auto &item : items) {
            if (item->category().isEmpty()) {
                spdlog::trace("Unable to categorize {}", item->PrettyName());
                ++n;
            }
        }
        if (n > 0) {
            spdlog::debug("There are {} uncategorized items.", n);
        }
    }

    // Snapshot boundary: the published tab list is authoritative for the
    // canonical inventory — deletions and ordering take effect here (D6).
    m_location_inventory.ResetTo(tabs);

    {
        // The snapshot's pricing sequence is one batch (M3 R3-4): nothing
        // observes UI state between these steps, so they produce a single
        // model update. The game's prices are never stored (#293): the tab
        // list's names are read here, the notes with their items; manual
        // tab prices of tabs that are gone leave memory, and the refresh
        // locks are recomputed.
        const BuyoutBatch snapshot_batch(m_buyout_manager);
        m_buyout_manager.SetStashTabLocations(tabs);
        m_buyout_manager.CompressTabBuyouts();
        PropagateTabBuyouts();
    }

    emit ItemsRefreshed(initial_refresh);
}

void ItemsManager::OnTabRefreshed(const ItemLocation &location, const Items &items)
{
    spdlog::trace("ItemsManager::OnTabRefreshed() entered");

    // The published copy stays the pre-update snapshot plus applied
    // replacements (D6): everything previously published for this fetch
    // source is dropped and the delta takes its place — one bucket swap in
    // the source-keyed store, O(replaced + delta) (D3, post-M2-M2). An
    // empty delta empties the fetch source and nothing else; tab deletion
    // stays snapshot-boundary.
    m_items.ReplaceSource(FetchSourceKey::ForLocation(location), items);

    // Every delta's location anchor feeds the canonical inventory, empty
    // deltas included (D6/R6-1).
    m_location_inventory.Ingest(location);

    ApplyScopedPricing(items);

    emit TabRefreshed(location, items);
}

void ItemsManager::OnChildrenReconciled(const ItemLocation &parent,
                                        const std::vector<FetchSourceKey> &expected)
{
    spdlog::trace("ItemsManager::OnChildrenReconciled() entered");

    // Run the worker's own reconcile predicate against OUR baseline
    // (R5-2/R6-2): the expected set is authoritative, so ghosts that only
    // the published copy still holds (divergence across a failed update)
    // are erased too. A walk of the bucket index with set lookup — the
    // erased items are the only ones touched (D3, post-M2-M2).
    const std::set<FetchSourceKey> expected_keys(expected.begin(), expected.end());
    m_items.EraseSourcesIf([&](const FetchSourceKey &key, const ItemLocation &loc) {
        return (key.type == ItemLocationType::STASH) && (loc.id() == parent.id())
               && (expected_keys.count(key) == 0);
    });

    m_location_inventory.Ingest(parent);

    emit ChildrenReconciled(parent, expected);
}

void ItemsManager::ApplyScopedPricing(const Items &delta_items)
{
    // Scoped per-delta pricing (D7), restricted to steps that are safe on
    // BOTH update outcomes (R1-4) — an update can end without a final pass.
    // A note's price comes with its item (#293), and the delta's rows are
    // rebuilt from the new items; what is left is to lock their tabs.
    for (const auto &item : delta_items) {
        // Monotone refresh-lock additions only — ClearRefreshLocks stays
        //    exclusive to the final pass. Fail-safe in the right direction:
        //    after a failed update the worst case is one redundant tab in
        //    the next checked refresh, never a priced tab dropped from it.
        if (!item->location().removeonly() && PriceRequiresRefresh(*item)) {
            m_buyout_manager.SetRefreshLocked(item->location());
        }
    }
}

void ItemsManager::Update(TabSelection type, const std::vector<ItemLocation> &locations)
{
    spdlog::trace("ItemsManager::Update() entered");
    emit UpdateSignal(type, locations);
}

void ItemsManager::SetAutoUpdate(bool update)
{
    spdlog::trace("ItemsManager::SetAutoUpdate() entered");
    m_settings.setValue("autoupdate", update);
    if (update) {
        spdlog::trace("ItemsManager::SetAutoUpdate() starting automatic updates");
        m_auto_update_timer->start();
    } else {
        spdlog::trace("ItemsManager::SetAutoUpdate() stopping automatic updates");
        m_auto_update_timer->stop();
    }
}

void ItemsManager::SetAutoUpdateInterval(int minutes)
{
    spdlog::trace("ItemsManager::SetAutoUpdateInterval() entered");
    spdlog::trace("ItemsManager::SetAutoUpdateInterval() setting interval to {} minutes", minutes);
    m_settings.setValue("autoupdate_interval", minutes);
    m_auto_update_timer->setInterval(minutes * 60 * 1000);
}

void ItemsManager::OnAutoRefreshTimer()
{
    spdlog::trace("ItemsManager::OnAutoRefreshTimer() entered");
    emit AutoUpdateSignal();
}
