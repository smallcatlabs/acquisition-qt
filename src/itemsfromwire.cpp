// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "itemsfromwire.h"

#include <array>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "itemlocation.h"
#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/stashtab.h"

void AppendWireItems(const std::vector<poe::Item> &items, const ItemLocation &base, Items &out)
{
    for (const auto &item : items) {
        const ItemLocation location = base.getItemLocation(item);
        out.push_back(std::make_shared<Item>(item, location));
        if (item.socketedItems) {
            AppendWireItems(*item.socketedItems, location, out);
        }
    }
}

void AppendWireItems(const poe::StashTab &stash, const ItemLocation &base, Items &out)
{
    if (stash.items) {
        AppendWireItems(*stash.items, base, out);
    }
}

void AppendWireItems(const poe::Character &character,
                     const ItemLocation &base,
                     Items &out,
                     const CharacterContainerVisitor &visit)
{
    const std::array containers{
        std::make_pair("equipment", &character.equipment),
        std::make_pair("inventory", &character.inventory),
        std::make_pair("rucksack", &character.rucksack),
        std::make_pair("jewels", &character.jewels),
    };
    for (const auto &[name, items] : containers) {
        if (!*items) {
            continue;
        }
        if (visit) {
            visit(name, (*items)->size());
        }
        AppendWireItems(**items, base, out);
    }
    // Their items show under the character and cannot be listed for sale
    // where they sit (#418, F63). A guardian's item names the character's own
    // slot, so the array an item arrived in is what says where it is.
    const std::array set_apart{
        std::make_tuple("guardian", &character.guardian, ItemLocation::Container::Guardian),
        std::make_tuple("wickerman", &character.wickerman, ItemLocation::Container::Wickerman),
    };
    for (const auto &[name, items, container] : set_apart) {
        if (!*items) {
            continue;
        }
        if (visit) {
            visit(name, (*items)->size());
        }
        AppendWireItems(**items, base.inContainer(container), out);
    }
}
