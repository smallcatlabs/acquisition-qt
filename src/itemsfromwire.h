// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

#include "item.h"

class ItemLocation;

namespace poe {
    struct Character;
    struct Item;
    struct StashTab;
} // namespace poe

// The one walk from a wire payload to Items, shared by the startup cache load
// and the live refresh handlers so the two cannot disagree about which items
// a payload holds (#260): each item, then — recursively — the items socketed
// into it. An item's location is the base location placed at that item; a
// socketed item is placed relative to the item it sits in.
//
// These append to `out` and do nothing else: discarding a folder's items,
// logging and reserving stay with the callers.

// Append the items of one wire array.
void AppendWireItems(const std::vector<poe::Item> &items, const ItemLocation &base, Items &out);

// Append a stash's items, if it carries any.
void AppendWireItems(const poe::StashTab &stash, const ItemLocation &base, Items &out);

// Called once per character container that is present, before its items are
// appended, with the container's name and its top-level item count.
using CharacterContainerVisitor = std::function<void(const char *name, size_t count)>;

// Append a character's items from every container Acquisition ingests:
// equipment, inventory, rucksack and jewels. This is the only place that list
// is written down; `guardian` and `skills` are deliberately left out (F63).
void AppendWireItems(const poe::Character &character,
                     const ItemLocation &base,
                     Items &out,
                     const CharacterContainerVisitor &visit = {});
