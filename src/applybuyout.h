// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

#include <vector>

#include "itemlocation.h"

class BuyoutManager;
class Item;
class ItemsManager;
struct Buyout;

// What one price edit applies to: the tab and character rows selected, and
// the items selected. Rows that cannot be priced (folders, the By-Item
// header) are left out by the caller.
struct BuyoutTargets
{
    std::vector<ItemLocation> tabs;
    std::vector<const Item *> items;
};

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
};

// Applies one price edit to its targets as one user command: one buyout
// batch (M3 R2-5) that also encloses the propagation of tab prices to the
// items that inherit them.
ApplyBuyoutResult ApplyBuyout(BuyoutManager &buyouts,
                              ItemsManager &items,
                              const BuyoutTargets &targets,
                              const Buyout &buyout);

// What the status line says after an edit, or an empty string when the
// edit was saved and any earlier notice should go. `buyouts_loaded` is
// BuyoutManager::BuyoutsLoaded().
QString ApplyBuyoutNotice(const ApplyBuyoutResult &result, bool buyouts_loaded);
