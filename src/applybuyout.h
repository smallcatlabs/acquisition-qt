// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

#include <memory>
#include <vector>

#include "itemlocation.h"

class BuyoutManager;
class Item;
class ItemsManager;
class LocationInventory;
struct Buyout;

// The rows a price editor shows a price for, and the only rows a price
// typed there can be written to: items by id, tabs and characters by
// location (compared by type and id). Both survive a refresh: an item a
// refresh replaced with the same id is the same target. Rows that cannot
// be priced (folders, the By-Item header) are left out by the caller.
struct PriceTargets
{
    std::vector<QString> item_ids;
    std::vector<ItemLocation> tabs;

    bool empty() const { return item_ids.empty() && tabs.empty(); }
    // The same rows, in any order.
    bool operator==(const PriceTargets &other) const;
};

// What one price edit applies to, resolved to the current objects.
struct BuyoutTargets
{
    std::vector<ItemLocation> tabs;
    std::vector<const Item *> items;
};

// Resolves targets against the current items and locations. Targets no
// longer there are left out and counted in `missing`.
struct ResolvedTargets
{
    BuyoutTargets targets;
    int missing{0};
};
ResolvedTargets ResolvePriceTargets(const PriceTargets &targets,
                                    const std::vector<std::shared_ptr<Item>> &items,
                                    const LocationInventory &locations);

struct ApplyBuyoutResult
{
    // Targets whose stored price now equals the edit.
    int applied{0};
    // Targets priced in the game, which are changed only there: a tab whose
    // name holds a price, every item in such a tab, and an item whose note
    // holds one. They are left alone.
    int locked{0};
    // Targets the manager did not save (see BuyoutManager::Set): the
    // buyouts did not load, or the database refused the write.
    int not_saved{0};
    // Targets no longer there when the price was committed (see
    // ResolvePriceTargets); ApplyBuyout never sets it.
    int missing{0};
};

// Applies one price edit to its targets as one user command: one buyout
// batch (M3 R2-5) that also encloses the propagation of tab prices to the
// items that inherit them.
ApplyBuyoutResult ApplyBuyout(BuyoutManager &buyouts,
                              ItemsManager &items,
                              const BuyoutTargets &targets,
                              const Buyout &buyout);

// What the status line says after an edit: why some targets kept their
// price, or an empty string when every target took it and any earlier
// notice should go. A price that was not saved takes precedence, then rows
// no longer there, then rows priced in the game.
// `buyouts_loaded` is BuyoutManager::BuyoutsLoaded().
QString ApplyBuyoutNotice(const ApplyBuyoutResult &result, bool buyouts_loaded);
