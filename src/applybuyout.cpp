// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "applybuyout.h"

#include <QCoreApplication>
#include <QStringList>

#include <algorithm>
#include <unordered_map>

#include "buyout.h"
#include "buyoutmanager.h"
#include "item.h"
#include "itemsmanager.h"
#include "locationinventory.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    // A target's identity: an item's id, or a location's type and id.
    using TargetKey = std::pair<bool, std::pair<ItemLocationType, QString>>;

    std::vector<TargetKey> sortedKeys(const PriceTargets &targets)
    {
        std::vector<TargetKey> keys;
        keys.reserve(targets.rows.size());
        for (const PriceTarget &row : targets.rows) {
            keys.push_back(row.isItem()
                               ? TargetKey{true, {ItemLocationType::STASH, row.item->id()}}
                               : TargetKey{false, LocationInventory::KeyFor(row.location)});
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

} // namespace

PriceTarget PriceTarget::ForItem(std::shared_ptr<const Item> item)
{
    const ItemLocation location = item->location();
    return PriceTarget{.item = std::move(item), .location = location};
}

PriceTarget PriceTarget::ForLocation(const ItemLocation &location)
{
    return PriceTarget{.item = nullptr, .location = location};
}

bool PriceTargets::operator==(const PriceTargets &other) const
{
    return (rows.size() == other.rows.size()) && (sortedKeys(*this) == sortedKeys(other));
}

ResolvedTargets ResolvePriceTargets(const PriceTargets &targets,
                                    const std::vector<std::shared_ptr<Item>> &items,
                                    const LocationInventory &locations)
{
    // One pass over the published items for the ids wanted: the copy at the
    // location each was shown in, and the first copy anywhere.
    struct Found
    {
        LocationInventory::Key shown;
        const Item *there{nullptr};
        const Item *anywhere{nullptr};
    };
    std::unordered_map<QString, Found> found;
    for (const PriceTarget &row : targets.rows) {
        if (row.isItem()) {
            found.emplace(row.item->id(),
                          Found{.shown = LocationInventory::KeyFor(row.location),
                                .there = nullptr,
                                .anywhere = nullptr});
        }
    }
    if (!found.empty()) {
        for (const auto &item : items) {
            const auto it = found.find(item->id());
            if (it == found.end()) {
                continue;
            }
            if (it->second.anywhere == nullptr) {
                it->second.anywhere = item.get();
            }
            if ((it->second.there == nullptr)
                && (LocationInventory::KeyFor(item->location()) == it->second.shown)) {
                it->second.there = item.get();
            }
        }
    }

    ResolvedTargets resolved;
    const auto &known = locations.entries();
    for (const PriceTarget &row : targets.rows) {
        if (row.isItem()) {
            // A named id: at() returns a reference, and GCC's
            // -Wdangling-reference flags one taken from a call whose
            // argument is a temporary (Item::id() returns by value).
            const QString id = row.item->id();
            const Found &f = found.at(id);
            if (const Item *item = f.there ? f.there : f.anywhere) {
                resolved.targets.items.push_back(item);
            } else {
                ++resolved.missing;
            }
        } else if (known.contains(LocationInventory::KeyFor(row.location))) {
            resolved.targets.tabs.push_back(row.location);
        } else {
            ++resolved.missing;
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
        if (buyouts.EffectiveTab(tab).IsGameSet()) {
            spdlog::trace("ApplyBuyout: tab {} is priced in the game", tab.id());
            ++result.locked;
            continue;
        }
        count(buyouts.SetTab(tab, buyout));
    }
    for (const Item *item : targets.items) {
        if (buyouts.EffectiveTab(item->location()).IsGameSet()
            || buyouts.Effective(*item).IsGameSet()) {
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
    const auto tr = [](const char *text) {
        return QCoreApplication::translate("ApplyBuyout", text);
    };
    QStringList parts;
    if (result.not_saved > 0) {
        // Only a rejection by the database is logged per edit (as an
        // error), so only then does the notice point at the log; a session
        // whose buyouts did not load refuses every edit and logs that once.
        parts.append((buyouts_loaded ? tr("Price not saved for %1 row(s). Check the log file "
                                          "for details.")
                                     : tr("Price not saved for %1 row(s): prices cannot be "
                                          "saved in this session."))
                         .arg(result.not_saved));
    }
    if (result.missing > 0) {
        parts.append(
            tr("%1 row(s) the price was typed for are no longer there.").arg(result.missing));
    }
    if (result.locked > 0) {
        parts.append(tr("%1 row(s) are priced in the game and were not changed; change their "
                        "price in the game.")
                         .arg(result.locked));
    }
    if (parts.isEmpty()) {
        return QString();
    }
    if (result.applied > 0) {
        parts.prepend(tr("Price saved for %1 row(s).").arg(result.applied));
    }
    return parts.join(' ');
}
