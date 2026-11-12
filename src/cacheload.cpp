// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "cacheload.h"

#include <QMetaObject>
#include <QThread>

#include <utility>

#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "itemsfromwire.h"
#include "itemsmanagerworker.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "util/programstate.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    // The children of a unique or map stash are not tabs of their own: their
    // items display under the parent.
    bool IsSpecialChild(const poe::StashTab &stash)
    {
        return stash.parent && ((stash.type == "UniqueStash") || (stash.type == "MapStash"));
    }

} // namespace

bool DiscardFolderItems(const poe::StashTab &stash)
{
    if ((stash.type != "Folder") || !stash.items || stash.items->empty()) {
        return false;
    }
    spdlog::warn("Discarding {} items carried by folder '{}' ({}): folders hold tabs, "
                 "not items",
                 stash.items->size(),
                 stash.name,
                 stash.id);
    return true;
}

CachedItems LoadCachedItems(const QDir &data_dir,
                            const SessionIdentity &identity,
                            std::stop_token stop,
                            const CacheLoadProgress &progress)
{
    spdlog::trace("LoadCachedItems() entered");
    const auto report = [&progress](const QString &status) {
        if (progress) {
            progress(status);
        }
    };

    // Manual-testing aid: delay each cached tab so that mid-load GUI actions
    // (quit, refresh-while-initializing) can be exercised by hand. Unset or
    // zero in normal use.
    const int delay_ms = qEnvironmentVariableIntValue("ACQ_PARSE_DELAY_MS");
    if (delay_ms > 0) {
        spdlog::warn("ACQ_PARSE_DELAY_MS is set: delaying {}ms per cached tab", delay_ms);
    }

    // A connection of our own: a database connection belongs to the thread
    // that opened it. Only read here, though that is not enforced.
    UserStore userstore{data_dir, identity.account()};
    CachedItems result;
    if (!userstore.isUsable()) {
        spdlog::error("LoadCachedItems: the user store could not be opened; no cached items");
        return result;
    }
    result.loaded = true;

    const QString &realm = identity.realm();
    const QString &league = identity.league();
    const auto stashes = userstore.stashes().getStashList(realm, league);
    const auto characters = userstore.characters().getCharacterList(realm, league);

    result.tabs.reserve(stashes.size() + characters.size());
    for (const auto &stash : stashes) {
        if (!IsSpecialChild(stash)) {
            result.tabs.push_back(ItemLocation(stash));
        }
    }
    for (const auto &character : characters) {
        result.tabs.push_back(ItemLocation(character));
    }
    result.tabs.shrink_to_fit();
    for (const auto &tab : result.tabs) {
        result.tab_id_index.emplace(tab.id());
    }

    for (size_t i = 0; i < stashes.size(); ++i) {
        if (stop.stop_requested()) {
            return result;
        }
        if (delay_ms > 0) {
            QThread::msleep(delay_ms);
        }
        const auto stash = userstore.stashes().getStash(stashes[i].id, realm, league);
        if (!stash) {
            // The row was listed but its contents were never fetched, so
            // there is nothing cached to load for it.
            continue;
        }
        report(
            QString("Parsing items from stash %1/%2: %3 '%4'")
                .arg(QString::number(i), QString::number(stashes.size()), stash->id, stash->name));
        ItemLocation location;
        if (!IsSpecialChild(*stash)) {
            location = ItemLocation{*stash};
        } else {
            // A special child's items display under its parent's location.
            for (const auto &tab : result.tabs) {
                if (tab.id() == stash->parent) {
                    location = tab;
                    break;
                }
            }
            if (!location.IsValid()) {
                spdlog::error("LoadCachedItems: could not find stash parent");
                continue;
            }
            // The items display under the parent, but they were fetched from
            // (and are keyed for replacement by) this child stash.
            location.setFetchId(stash->id);
        }
        if (!stash->items || DiscardFolderItems(*stash)) {
            continue;
        }
        spdlog::debug("LoadCachedItems: loading {} items from stash # {}: {} ({})",
                      stash->items->size(),
                      stash->index.value_or(-1),
                      stash->id,
                      stash->name);
        result.items.reserve(result.items.size() + stash->items->size());
        AppendWireItems(*stash, location, result.items);
    }

    for (size_t i = 0; i < characters.size(); ++i) {
        if (stop.stop_requested()) {
            return result;
        }
        if (delay_ms > 0) {
            QThread::msleep(delay_ms);
        }
        const QString &name = characters[i].name;
        const auto character = userstore.characters().getCharacter(name, realm);
        if (!character) {
            // Listed but never fetched: nothing cached to load.
            continue;
        }
        report(QString("Parsing items from character %1/%2: %3")
                   .arg(QString::number(i), QString::number(characters.size()), name));
        AppendWireItems(*character,
                        ItemLocation(*character),
                        result.items,
                        [&](const char *container, size_t count) {
                            result.items.reserve(result.items.size() + count);
                            spdlog::debug("LoadCachedItems: loading {} items from character {} "
                                          "{} ({})",
                                          count,
                                          container,
                                          character->id,
                                          character->name);
                        });
    }
    return result;
}

std::jthread StartCacheLoad(ItemsManagerWorker &worker,
                            const QDir &data_dir,
                            const SessionIdentity &identity,
                            bool store_usable)
{
    if (!store_usable) {
        spdlog::error("StartCacheLoad: the user store is not usable; no cached items are loaded");
        QMetaObject::invokeMethod(
            &worker, [&worker] { worker.Seed(CachedItems{}); }, Qt::QueuedConnection);
        return {};
    }
    return std::jthread([&worker, data_dir, identity](std::stop_token stop) {
        // Progress lines go out as the worker's own status signal; its
        // receivers live on the main thread, so they arrive queued.
        CachedItems result = LoadCachedItems(data_dir, identity, stop, [&worker](const QString &s) {
            emit worker.StatusUpdate(ProgramState::Initializing, s);
        });
        if (stop.stop_requested()) {
            return;
        }
        QMetaObject::invokeMethod(
            &worker,
            [&worker, result = std::move(result)]() mutable { worker.Seed(std::move(result)); },
            Qt::QueuedConnection);
    });
}
