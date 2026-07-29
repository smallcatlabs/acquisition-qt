// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "itemsmanager.h"

#include <QNetworkCookie>
#include <QSettings>

#include "buyoutmanager.h"
#include "datastore/datastore.h"
#include "item.h"
#include "modlist.h"
#include "repoe/repoe.h"
#include "shop.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

ItemsManager::ItemsManager(QSettings &settings, BuyoutManager &buyout_manager, DataStore &datastore)
    : m_settings(settings)
    , m_buyout_manager(buyout_manager)
    , m_datastore(datastore)
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

void ItemsManager::OnStatusUpdate(ProgramState state, const QString &status)
{
    emit StatusUpdate(state, status);
}

void ItemsManager::ApplyAutoTabBuyouts()
{
    spdlog::trace("ItemsManager::ApplyAutoTabBuyouts() entered");
    // Can handle everything related to auto-tab pricing here.
    // 1. First format we need to honor is ascendency pricing formats which is top priority and overrides other types
    // 2. Second priority is to honor manual user pricing
    // 3. Third priority it to apply pricing based on ideally user specified formats (doesn't exist yet)

    // Loop over all tabs, create buyout based on tab name which applies auto-pricing policies
    for (auto const &loc : m_buyout_manager.GetStashTabLocations()) {
        auto tab_label = loc.tab_label();
        Buyout buyout = m_buyout_manager.StringToBuyout(tab_label);
        if (buyout.IsActive()) {
            m_buyout_manager.SetTab(loc, buyout);
        }
    }

    // Need to compress tab buyouts here, as the tab names change we accumulate and save BO's
    // for tabs that no longer exist I think.
    m_buyout_manager.CompressTabBuyouts();
}

void ItemsManager::ApplyAutoItemBuyouts()
{
    spdlog::trace("ItemsManager::ApplyAutoItemBuyouts() entered");
    // Loop over all items, check for note field with pricing and apply
    for (auto const &item : m_items) {
        auto const &note = item->note();
        if (!note.isEmpty()) {
            Buyout buyout = m_buyout_manager.StringToBuyout(note);
            // This line may look confusing, buyout returns an active buyout if game
            // pricing was found or a default buyout (inherit) if it was not.
            // If there is a currently valid note we want to apply OR if
            // old note no longer is valid (so basically clear pricing)
            if (buyout.IsActive() || m_buyout_manager.Get(*item).IsGameSet()) {
                m_buyout_manager.Set(*item, buyout);
            }
        }
    }

    // Commenting this out for robustness (iss381) to make it as unlikely as possible that users
    // pricing data will be removed.  Side effect is that stale pricing data will pile up and
    // could be applied to future items with the same hash (which includes tab name).
    // bo.CompressItemBuyouts(m_items);
}

void ItemsManager::PropagateTabBuyouts()
{
    spdlog::trace("ItemsManager::PropagateTabBuyouts() entered");
    m_buyout_manager.ClearRefreshLocks();
    for (auto &item_ptr : m_items) {
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
    m_items = items;

    spdlog::debug("There are {} items and {} tabs after the refresh.", m_items.size(), tabs.size());
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

    m_buyout_manager.SetStashTabLocations(tabs);
    MigrateBuyouts();
    ApplyAutoTabBuyouts();
    ApplyAutoItemBuyouts();
    PropagateTabBuyouts();

    emit ItemsRefreshed(initial_refresh);
}

// SPIKE(S1-M2)
void ItemsManager::SpikeApplyTabDelta(const ItemLocation &tab, const Items &tab_items)
{
    const QString key = tab.fetchSourceKey();
    std::erase_if(m_items,
                  [&key](const auto &item) { return item->location().fetchSourceKey() == key; });
    m_items.insert(m_items.end(), tab_items.begin(), tab_items.end());
    emit TabRefreshed(tab, tab_items);
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
    Update(TabSelection::Checked);
}

void ItemsManager::MigrateBuyouts()
{
    spdlog::trace("ItemsManager::MigrateBuyouts() entered");
    const int db_version = m_datastore.GetInt("db_version");

    // Do nothing if the database has already been migrated.
    if (db_version == 5) {
        spdlog::debug("ItemsManager skipping migration because db_version is {}", db_version);
        return;
    }

    // Migrate from v4 to v5.
    if (db_version == 4) {
        spdlog::debug("ItemsManager migrating from db_version {} to 5", db_version);
        for (const auto &item : m_items) {
            m_buyout_manager.MigrateItem(item->hash_v4(), item->id());
        }
        m_buyout_manager.Save();
        m_datastore.SetInt("db_version", 5);
        return;
    }

    // Log an error if the version is somehow too high.
    if (db_version > 5) {
        spdlog::error("ItemsManager cannot migrate because db_version {} is too new", db_version);
        return;
    }

    // Migrate from older versions to v4.
    spdlog::debug("ItemsManager migrating from db_version {} to 4", db_version);
    for (const auto &item : m_items) {
        m_buyout_manager.MigrateItem(item->old_hash(), item->hash_v4());
    }
    m_buyout_manager.Save();
    m_datastore.SetInt("db_version", 4);

    // Trigger another migration from v4 to v5.
    MigrateBuyouts();
}
