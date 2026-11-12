// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QDir>
#include <QString>

#include <functional>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

#include "item.h"
#include "itemlocation.h"
#include "sessionidentity.h"

class ItemsManagerWorker;

namespace poe {
    struct StashTab;
}

// What the start-up cache load found (#294): the cached tabs and their items,
// as the worker starts from them.
struct CachedItems
{
    // False when the user store could not be opened; everything else is then
    // empty.
    bool loaded{false};
    std::vector<ItemLocation> tabs;
    Items items;
    std::set<QString> tab_id_index;
};

// Called with a progress line for each cached tab as it is loaded.
using CacheLoadProgress = std::function<void(const QString &status)>;

// Load the session's cached tabs and items from the user store in `data_dir`.
// Opens its own connection to the store, so it may run on any thread; it
// returns early with what it has when `stop` is requested.
CachedItems LoadCachedItems(const QDir &data_dir,
                            const SessionIdentity &identity,
                            std::stop_token stop,
                            const CacheLoadProgress &progress = {});

// Run LoadCachedItems on a thread of its own and hand the result to
// `worker` (ItemsManagerWorker::Seed) on the worker's thread. The returned
// thread is the load's owner: destroying it stops the load and joins it, so
// it must be destroyed before `worker`, and before logging is torn down
// (F29).
//
// `store_usable` is whether the session's own user store opened. That store
// is opened (and migrated) first, on the main thread; when it could not be,
// nothing is loaded and the worker is seeded with an empty, not-loaded
// result, so the load never opens a store that would still need migrating.
std::jthread StartCacheLoad(ItemsManagerWorker &worker,
                            const QDir &data_dir,
                            const SessionIdentity &identity,
                            bool store_usable);

// Folder-tree D12 (R8, A4): a Folder-type stash holds tabs, never items.
// Returns true, with one warning naming the count, when `stash` is a folder
// carrying items, which the caller must then discard; used by the cache load
// and the live refresh alike, so the two cannot disagree.
bool DiscardFolderItems(const poe::StashTab &stash);
