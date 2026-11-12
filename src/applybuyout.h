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

// One row a price editor shows a price for: an item (the copy shown, which
// also says where it was) or a tab or character (its location).
struct PriceTarget
{
    std::shared_ptr<const Item> item;
    ItemLocation location;

    static PriceTarget ForItem(std::shared_ptr<const Item> item);
    static PriceTarget ForLocation(const ItemLocation &location);
    bool isItem() const { return item != nullptr; }
};

// The rows a price editor shows a price for, and the only rows a price
// typed there can be written to. They are compared by identity that
// survives a refresh: an item by id (a refresh that replaces it with the
// same id leaves it the same target), a tab or character by type and id.
// The first row is the one whose price is shown. Rows that cannot be
// priced (folders, the By-Item header) are left out by the caller.
struct PriceTargets
{
    std::vector<PriceTarget> rows;

    bool empty() const { return rows.empty(); }
    // The same rows, in any order.
    bool operator==(const PriceTargets &other) const;
};

// What one price edit applies to, resolved to the current objects.
struct BuyoutTargets
{
    std::vector<ItemLocation> tabs;
    std::vector<const Item *> items;
};

// Resolves targets against the current items and locations. An item
// resolves to the copy with its id at the location it was shown in, if
// there is one, else to any copy with its id. Targets no longer there are
// left out and counted in `missing`.
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

// What the status line says after an edit: every reason some targets kept
// their price, with how many, or an empty string when every target took
// it and any earlier notice should go.
// `buyouts_loaded` is BuyoutManager::BuyoutsLoaded().
QString ApplyBuyoutNotice(const ApplyBuyoutResult &result, bool buyouts_loaded);
