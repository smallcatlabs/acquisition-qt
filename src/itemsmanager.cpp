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

namespace {

    // The one rule for prices read from the game (#251), shared by the tab
    // pass, the item pass and the per-delta pass. `text` is an item's note or
    // a tab's name. Returns the buyout to store, or nothing to leave the
    // stored one alone:
    // - text that yields an ACTIVE buyout sets it (game pricing wins);
    // - otherwise a stored game-set price is reset to inherit, so a deleted
    //   note, a renamed tab, or a note naming an unknown currency (#273)
    //   never leaves a stale, locked price behind;
    // - anything else (no stored price, a manual price) is left alone.
    // A price inherited from a tab carries the game source too; it belongs to
    // propagation, and clearing it here would delete and rewrite its row on
    // every pass.
    std::optional<Buyout> GameBuyoutUpdate(BuyoutManager &manager,
                                           const Buyout &stored,
                                           const QString &text)
    {
        if (!text.isEmpty()) {
            const Buyout parsed = manager.StringToBuyout(text);
            if (parsed.IsActive()) {
                return parsed;
            }
        }
        if (stored.IsGameSet() && !stored.inherited) {
            return Buyout();
        }
        return std::nullopt;
    }

} // namespace

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

void ItemsManager::ApplyAutoTabBuyouts()
{
    spdlog::trace("ItemsManager::ApplyAutoTabBuyouts() entered");
    // Pricing passes batch (M3 R1-6): one model update at pass end when
    // this pass is outermost, never one per SetTab.
    const BuyoutBatch batch(m_buyout_manager);
    // Can handle everything related to auto-tab pricing here.
    // 1. First format we need to honor is ascendency pricing formats which is top priority and overrides other types
    // 2. Second priority is to honor manual user pricing
    // 3. Third priority it to apply pricing based on ideally user specified formats (doesn't exist yet)

    // Loop over all tabs, create buyout based on tab name which applies auto-pricing policies
    for (auto const &loc : m_buyout_manager.GetStashTabLocations()) {
        const auto update = GameBuyoutUpdate(m_buyout_manager,
                                             m_buyout_manager.GetTab(loc),
                                             loc.tab_label());
        if (update) {
            m_buyout_manager.SetTab(loc, *update);
        }
    }

    // Need to compress tab buyouts here, as the tab names change we accumulate and save BO's
    // for tabs that no longer exist I think.
    m_buyout_manager.CompressTabBuyouts();
}

void ItemsManager::ApplyAutoItemBuyouts()
{
    spdlog::trace("ItemsManager::ApplyAutoItemBuyouts() entered");
    // Pricing passes batch (M3 R1-6): one model update at pass end when
    // this pass is outermost, never one per Set.
    const BuyoutBatch batch(m_buyout_manager);
    // Loop over all items, check for note field with pricing and apply
    for (auto const &item : m_items.Flat()) {
        const auto update = GameBuyoutUpdate(m_buyout_manager,
                                             m_buyout_manager.Get(*item),
                                             item->note());
        if (update) {
            m_buyout_manager.Set(*item, *update);
        }
    }

    // Buyouts of items that are gone are deliberately kept, not pruned
    // (iss381), so that a user's prices are as hard as possible to lose;
    // stale entries pile up as a result.
}

void ItemsManager::PropagateTabBuyouts()
{
    spdlog::trace("ItemsManager::PropagateTabBuyouts() entered");
    // Pricing passes batch (M3 R1-6). When a user command triggers this
    // pass, the command's own batch encloses it and this boundary emits
    // nothing (R3-3).
    const BuyoutBatch batch(m_buyout_manager);
    m_buyout_manager.ClearRefreshLocks();
    // A tab whose own price requires a refresh is locked from the tab list,
    // so a priced tab with no loaded items (empty, or not fetched yet) is
    // refreshed too (#148). Remove-only tabs are skipped, as below, and so
    // are folders, which hold no items and cannot be priced (D11).
    for (const auto &tab : m_buyout_manager.GetStashTabLocations()) {
        if (!tab.removeonly() && !tab.IsFolder() && m_buyout_manager.GetTab(tab).RequiresRefresh()) {
            m_buyout_manager.SetRefreshLocked(tab);
        }
    }
    for (auto &item_ptr : m_items.Flat()) {
        Item &item = *item_ptr;
        auto item_bo = m_buyout_manager.Get(item);
        auto tab_bo = m_buyout_manager.GetTab(item.location());

        if (item_bo.IsInherited()) {
            if (tab_bo.IsActive()) {
                // Any propagation from tab price to item price should include this bit set
                tab_bo.inherited = true;
                tab_bo.last_update = QDateTime::currentDateTime();
                m_buyout_manager.Set(item, tab_bo);
            } else {
                // This effectively 'clears' buyout by setting back to 'inherit' state.
                m_buyout_manager.Set(item, Buyout());
            }
        }

        // If any savable bo's are set on an item or the tab then lock the refresh state.
        // Skip remove-only tabs because they are not editable, nor indexed for trade now.
        if (item.location().removeonly() == false) {
            if (m_buyout_manager.Get(item).RequiresRefresh() || tab_bo.RequiresRefresh()) {
                m_buyout_manager.SetRefreshLocked(item.location());
            }
        }
    }
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

    m_buyout_manager.SetStashTabLocations(tabs);
    {
        // The snapshot's pricing sequence is one batch (M3 R3-4): nothing
        // observes UI state between these passes, so they must produce a
        // single model update, never up to three.
        const BuyoutBatch snapshot_batch(m_buyout_manager);
        ApplyAutoTabBuyouts();
        ApplyAutoItemBuyouts();
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
    // The scoped per-delta pass batches like the final passes (M3 R1-6):
    // one model update per pass, never one per Set.
    const BuyoutBatch batch(m_buyout_manager);
    // Scoped per-delta pricing (D7), restricted to steps that are safe on
    // BOTH update outcomes (R1-4) — an update can end without a final pass:
    for (const auto &item : delta_items) {
        // 1. Note-based item buyouts, mirroring ApplyAutoItemBuyouts's
        //    per-item rule (re-derivable from the item's own note).
        const auto update = GameBuyoutUpdate(m_buyout_manager,
                                             m_buyout_manager.Get(*item),
                                             item->note());
        if (update) {
            m_buyout_manager.Set(*item, *update);
        }

        // 2. Tab-inheritance propagation, mirroring PropagateTabBuyouts's
        //    per-item rule against the currently published tab-buyout state.
        //    GetTab keys on the stable location id, so a renamed tab's
        //    streamed items still find their existing tab buyout. Tab-name
        //    auto-pricing (SetTab) is deliberately absent: it is
        //    final-pass-only (D7).
        Buyout tab_bo = m_buyout_manager.GetTab(item->location());
        if (m_buyout_manager.Get(*item).IsInherited()) {
            if (tab_bo.IsActive()) {
                tab_bo.inherited = true;
                tab_bo.last_update = QDateTime::currentDateTime();
                m_buyout_manager.Set(*item, tab_bo);
            } else {
                m_buyout_manager.Set(*item, Buyout());
            }
        }

        // 3. Monotone refresh-lock additions only — ClearRefreshLocks stays
        //    exclusive to the final pass. Fail-safe in the right direction:
        //    after a failed update the worst case is one redundant tab in
        //    the next checked refresh, never a priced tab dropped from it.
        if (item->location().removeonly() == false) {
            if (m_buyout_manager.Get(*item).RequiresRefresh() || tab_bo.RequiresRefresh()) {
                m_buyout_manager.SetRefreshLocked(item->location());
            }
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
