// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "applybuyout.h"

#include <QCoreApplication>

#include <algorithm>
#include <unordered_map>

#include "buyout.h"
#include "buyoutmanager.h"
#include "item.h"
#include "itemsmanager.h"
#include "locationinventory.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    std::vector<QString> sortedIds(const PriceTargets &targets)
    {
        std::vector<QString> ids = targets.item_ids;
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    std::vector<LocationInventory::Key> sortedKeys(const PriceTargets &targets)
    {
        std::vector<LocationInventory::Key> keys;
        keys.reserve(targets.tabs.size());
        for (const ItemLocation &tab : targets.tabs) {
            keys.push_back(LocationInventory::KeyFor(tab));
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

} // namespace

bool PriceTargets::operator==(const PriceTargets &other) const
{
    return (item_ids.size() == other.item_ids.size()) && (tabs.size() == other.tabs.size())
           && (sortedIds(*this) == sortedIds(other)) && (sortedKeys(*this) == sortedKeys(other));
}

ResolvedTargets ResolvePriceTargets(const PriceTargets &targets,
                                    const std::vector<std::shared_ptr<Item>> &items,
                                    const LocationInventory &locations)
{
    ResolvedTargets resolved;
    const auto &known = locations.entries();
    for (const ItemLocation &tab : targets.tabs) {
        if (known.contains(LocationInventory::KeyFor(tab))) {
            resolved.targets.tabs.push_back(tab);
        } else {
            ++resolved.missing;
        }
    }
    if (!targets.item_ids.empty()) {
        // One pass over the published items; the first item with an id
        // stands for it (buyouts are keyed by id).
        std::unordered_map<QString, const Item *> found;
        for (const QString &id : targets.item_ids) {
            found.emplace(id, nullptr);
        }
        for (const auto &item : items) {
            const auto it = found.find(item->id());
            if ((it != found.end()) && (it->second == nullptr)) {
                it->second = item.get();
            }
        }
        for (const QString &id : targets.item_ids) {
            if (const Item *item = found.at(id)) {
                resolved.targets.items.push_back(item);
            } else {
                ++resolved.missing;
            }
        }
    }
    return resolved;
}

ApplyBuyoutResult ApplyBuyout(BuyoutManager &buyouts,
                              ItemsManager &items,
                              const BuyoutTargets &targets,
                              const Buyout &buyout)
{
    ApplyBuyoutResult result;
    const auto count = [&result](bool saved) {
        if (saved) {
            ++result.applied;
        } else {
            ++result.not_saved;
        }
    };

    // One command, one batch: the edits and the trailing propagation pass
    // reach the model as one update. The pass's own boundary nests inside
    // this one and emits nothing (M3 R3-3).
    const BuyoutBatch batch(buyouts);
    for (const ItemLocation &tab : targets.tabs) {
        if (buyouts.GetTab(tab).IsGameSet()) {
            spdlog::trace("ApplyBuyout: tab {} is priced in the game", tab.id());
            ++result.locked;
            continue;
        }
        count(buyouts.SetTab(tab, buyout));
    }
    for (const Item *item : targets.items) {
        if (buyouts.GetTab(item->location()).IsGameSet() || buyouts.Get(*item).IsGameSet()) {
            spdlog::trace("ApplyBuyout: item {} is priced in the game", item->id());
            ++result.locked;
            continue;
        }
        count(buyouts.Set(*item, buyout));
    }
    items.PropagateTabBuyouts();
    return result;
}

QString ApplyBuyoutNotice(const ApplyBuyoutResult &result, bool buyouts_loaded)
{
    if ((result.not_saved == 0) && (result.missing > 0)) {
        return QCoreApplication::translate("ApplyBuyout",
                                           "Price not applied to %1 row(s): they are no longer "
                                           "there.")
            .arg(result.missing);
    }
    if (result.not_saved == 0) {
        // Only part of a multi-selection can be priced in the game: a row
        // the game priced cannot be edited on its own.
        return (result.locked == 0)
                   ? QString()
                   : QCoreApplication::translate("ApplyBuyout",
                                                 "Some selected rows are priced in the game and "
                                                 "were not changed. Change their price in the "
                                                 "game.");
    }
    // Only a rejection by the database is logged per edit (as an error), so
    // only then does the notice point at the log; a session whose buyouts
    // did not load refuses every edit and logs that once.
    return buyouts_loaded
               ? QCoreApplication::translate("ApplyBuyout",
                                             "Price not saved. Check the log file for details.")
               : QCoreApplication::translate(
                     "ApplyBuyout", "Price not saved: prices cannot be saved in this session.");
}
