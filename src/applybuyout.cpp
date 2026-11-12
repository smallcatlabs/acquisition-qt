// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "applybuyout.h"

#include <QCoreApplication>

#include "buyout.h"
#include "buyoutmanager.h"
#include "item.h"
#include "itemsmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

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
    if ((result.applied == 0) && (result.locked == 0) && (result.not_saved == 0)) {
        return QCoreApplication::translate("ApplyBuyout",
                                           "Price not applied: nothing selected can be priced.");
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
