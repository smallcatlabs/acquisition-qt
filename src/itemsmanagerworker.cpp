// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "itemsmanagerworker.h"

#include <QCoroFuture>

#include <QDir>
#include <QFileInfo>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QSettings>
#include <QSignalMapper>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "buyoutmanager.h"
#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "itemlocation.h"
#include "modlist.h"
#include "poe/poe_utils.h"
#include "poe/poeapiclient.h"
#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/stashtab.h"
#include "repoe/repoe.h"
#include "util/json_readers.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

namespace {

    // The Internal FetchError a per-fetch task's catch-all synthesizes from an
    // exceptional future (R5-1): it routes an unexpected throw through the same
    // failure branch a value-level failure takes, so the update aborts cleanly
    // instead of wedging on a completion that never counts.
    RateLimit::FetchError MakeInternalError(const QString &message)
    {
        RateLimit::FetchError error;
        error.kind = RateLimit::FetchError::Kind::Internal;
        error.message = message;
        return error;
    }

    // The single post-await identity gate (IR2/W-IDENTITY). Every per-fetch
    // coroutine routes its handler through this ONE helper after its co_await, so
    // there is one gate to get right — not four independent copies — and the
    // W-IDENTITY mutation proof (bypassing the token check here) regression-covers
    // all four fetch kinds at once. The update token is the update's identity: a
    // straggler from a failed update carries that update's stopped token, so its
    // result is discarded rather than applied to whatever update is now active.
    // The handler runs on the completing stack, still inside the caller's
    // catch-all, so a throw from it flows to the per-fetch terminal abort.
    template<typename Handler>
    void ProcessIfLive(const std::stop_token &token, Handler &&handler)
    {
        if (!token.stop_requested()) {
            std::forward<Handler>(handler)();
        }
    }

    // Folder-tree D12 (R8, A4): a Folder-type stash holds tabs, never items.
    // Items one carries — in a live reply or a cached row — are discarded
    // here, so they never enter the item store and the two view modes
    // cannot disagree about them. One warning per tab, naming the count.
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

    // Folder-tree D12: a tab listed under a folder's `children` should name
    // that folder as its own `folder`. A mismatch is logged, as an API-change
    // tripwire; no fallback is applied.
    void WarnMisfiledFolderChildren(const poe::StashTab &folder)
    {
        if ((folder.type != "Folder") || !folder.children) {
            return;
        }
        for (const auto &child : *folder.children) {
            if (!child.folder) {
                spdlog::warn("Folder '{}' ({}) lists tab '{}' ({}) whose folder id is missing",
                             folder.name,
                             folder.id,
                             child.name,
                             child.id);
            } else if (*child.folder != folder.id) {
                spdlog::warn("Folder '{}' ({}) lists tab '{}' ({}) whose folder id names "
                             "another folder ({})",
                             folder.name,
                             folder.id,
                             child.name,
                             child.id,
                             *child.folder);
            }
        }
    }

} // namespace

ItemsManagerWorker::ItemsManagerWorker(QSettings &settings,
                                       BuyoutManager &buyout_manager,
                                       PoeApiClient &api,
                                       QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_buyout_manager(buyout_manager)
    , m_api(api)
    , m_type(TabSelection::Checked)
    , m_update_all(false)
    , m_need_stash_list(false)
    , m_need_character_list(false)
    , m_has_stash_list(false)
    , m_has_character_list(false)
    , m_update_tab_contents(true)
{
    spdlog::trace("ItemsManagerWorker::ItemsManagerWorker() entered");

    m_realm = m_settings.value("realm").toString();
    m_league = m_settings.value("league").toString();
    m_account = m_settings.value("account").toString();
    spdlog::trace("ItemsManagerWorker: league = {}", m_league);
    spdlog::trace("ItemsManagerWorker: account = {}", m_account);
}

ItemsManagerWorker::~ItemsManagerWorker()
{
    m_shutdown.store(true);
    if (m_parser_thread && m_parser_thread->isRunning()) {
        m_parser_thread->wait();
    }
}

void ItemsManagerWorker::UpdateRequest(TabSelection type, const std::vector<ItemLocation> &locations)
{
    spdlog::trace("ItemsManagerWorker::UpdateRequest() entered");
    m_updateRequest = true;
    m_type = type;
    m_locations = locations;
}

void ItemsManagerWorker::OnRePoEReady()
{
    spdlog::trace("ItemsManagerWorker::OnRePoEReady() entered");
    StartParseThread();
}

void ItemsManagerWorker::StartParseThread()
{
    // Create a separate thread to load the items, which allows the UI to
    // update the status bar while items are being parsed. This operation
    // can take tens of seconds or longer depending on the nubmer of tabs
    // and items.
    const QFileInfo info(m_settings.fileName());
    const QString dataDir = info.absolutePath() + "/data/";
    m_shutdown.store(false);
    m_parser_thread = QThread::create([this, dataDir]() {
        auto result = ParseCachedItems(dataDir);
        if (m_shutdown.load()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, result = std::move(result)]() mutable { OnParseCompleted(std::move(result)); },
            Qt::QueuedConnection);
    });
    connect(m_parser_thread, &QThread::finished, m_parser_thread, &QThread::deleteLater);
    m_parser_thread->start();
}

ParseResult ItemsManagerWorker::ParseCachedItems(const QString &dataDir) const
{
    spdlog::trace("ItemsManagerWorker::ParseItemMods() entered");
    ParseResult result;
    auto sendStatusUpdate = [this](ProgramState state, const QString &status) {
        emit const_cast<ItemsManagerWorker *>(this)->StatusUpdate(state, status);
    };

    // Manual-testing aid: delay each cached tab so that mid-parse GUI actions
    // (quit, refresh-while-initializing) can be exercised by hand. Unset or
    // zero in normal use.
    const int parse_delay_ms = qEnvironmentVariableIntValue("ACQ_PARSE_DELAY_MS");
    if (parse_delay_ms > 0) {
        spdlog::warn("ACQ_PARSE_DELAY_MS is set: delaying {}ms per cached tab", parse_delay_ms);
    }

    // Create a datastore to get a connection to the database.
    // NOTE: we only need read access, but this isn't enforced or checked.
    QDir data_dir{dataDir};
    UserStore userstore{data_dir, m_account};

    // Get cached characters and stash tabs.
    const auto stashes = userstore.stashes().getStashList(m_realm, m_league);
    const auto characters = userstore.characters().getCharacterList(m_realm, m_league);

    // Do not process children from unique and map stashers.
    auto special = [](const poe::StashTab &stash) {
        return stash.parent && ((stash.type == "UniqueStash") || (stash.type == "MapStash"));
    };

    result.tabs.reserve(stashes.size() + characters.size());
    for (const auto &stash : stashes) {
        if (!special(stash)) {
            result.tabs.push_back(ItemLocation(stash));
        }
    }
    for (const auto &character : characters) {
        result.tabs.push_back(ItemLocation(character, int(result.tabs.size())));
    }
    result.tabs.shrink_to_fit();

    // Save location ids.
    spdlog::trace("ItemsManagerWorker::ParseItemMods() saving location ids");
    for (const auto &tab : result.tabs) {
        result.tab_id_index.emplace(tab.id());
    }

    // Get cached items
    spdlog::trace("ItemsManagerWorker::ParseItemMods() getting cached items");

    // Get stash items.
    for (size_t i = 0; i < stashes.size(); ++i) {
        if (m_shutdown.load()) {
            return result;
        }
        if (parse_delay_ms > 0) {
            QThread::msleep(parse_delay_ms);
        }
        const auto id = stashes[i].id;
        const auto stash = userstore.stashes().getStash(id, m_realm, m_league);
        if (!stash) {
            // The row was listed but its contents were never fetched, so
            // there is nothing cached to load for it here.
            continue;
        }
        sendStatusUpdate(ProgramState::Initializing,
                         QString("Parsing items from stash %1/%2: %3 '%4'")
                             .arg(QString::number(i),
                                  QString::number(stashes.size()),
                                  stash->id,
                                  stash->name));
        ItemLocation location;
        if (!special(*stash)) {
            location = ItemLocation{*stash};
        } else {
            // Items in special tabs should use their parent's ItemLocation.
            for (const auto &tab : result.tabs) {
                if (tab.id() == stash->parent) {
                    location = tab;
                    break;
                }
            }
            if (!location.IsValid()) {
                spdlog::error("ItemsManagerWorker: could not find stash parent");
                continue;
            }
            // The items display under the parent, but they were fetched from
            // (and are keyed for replacement by) this child stash.
            location.setFetchId(stash->id);
        }
        LoadItems(*stash, location, result);
    }

    // Get character items.
    for (size_t i = 0; i < characters.size(); ++i) {
        if (m_shutdown.load()) {
            return result;
        }
        if (parse_delay_ms > 0) {
            QThread::msleep(parse_delay_ms);
        }
        const auto name = characters[i].name;
        const auto character = userstore.characters().getCharacter(name, m_realm);
        if (!character) {
            // Listed but never fetched: nothing cached to load here.
            continue;
        }
        sendStatusUpdate(ProgramState::Initializing,
                         QString("Parsing items from character %1/%2: %3")
                             .arg(i)
                             .arg(characters.size())
                             .arg(name));
        ItemLocation tab{*character, int(stashes.size() + i)};
        LoadItems(*character, tab, result);
    }

    sendStatusUpdate(ProgramState::Ready,
                     QString("Parsed %1 items from %2 tabs")
                         .arg(result.items.size())
                         .arg(result.tabs.size()));

    return result;
}

void ItemsManagerWorker::OnParseCompleted(ParseResult result)
{
    m_tabs = std::move(result.tabs);
    m_items.ResetTo(std::move(result.items));
    m_tab_id_index = std::move(result.tab_id_index);
    m_state = WorkerState::Idle;
    // let ItemManager know that the retrieval of cached items/tabs has been completed (calls ItemsManager::OnItemsRefreshed method)
    spdlog::trace("ItemsManagerWorker::ParseItemMods() emitting ItemsRefreshed signal");
    emit ItemsRefreshed(m_items.Flat(), m_tabs, true);

    if (m_updateRequest) {
        spdlog::trace("ItemsManagerWorker::ParseItemMods() triggering requested update");
        m_updateRequest = false;
        Update(m_type, m_locations);
    }
}

void ItemsManagerWorker::LoadItems(const poe::StashTab &stash,
                                   ItemLocation location,
                                   ParseResult &result) const
{
    const auto items = stash.items;
    if (!items || DiscardFolderItems(stash)) {
        return;
    }
    result.items.reserve(result.items.size() + items->size());
    spdlog::debug("ItemManagerWorker: loading {} items from stash # {}: {} ({})",
                  items->size(),
                  stash.index.value_or(-1),
                  stash.id,
                  stash.name);
    for (const auto &item : *items) {
        result.items.push_back(std::make_shared<Item>(item, location));
    }
}

void ItemsManagerWorker::LoadItems(const poe::Character &character,
                                   ItemLocation location,
                                   ParseResult &result) const
{
    const std::array collections{
        std::make_pair("equipment", character.equipment),
        std::make_pair("inventory", character.inventory),
        std::make_pair("rucksack", character.rucksack),
        std::make_pair("jewels", character.jewels),
    };

    for (const auto &[name, items] : collections) {
        if (!items) {
            continue;
        }
        result.items.reserve(result.items.size() + items->size());
        spdlog::debug("ItemManagerWorker: loading {} items from character {} {} ({})",
                      items->size(),
                      name,
                      character.id,
                      character.name);
        for (const auto &item : *items) {
            result.items.push_back(std::make_shared<Item>(item, location));
        }
    }
}

void ItemsManagerWorker::Update(TabSelection type, const std::vector<ItemLocation> &locations)
{
    if (!isInitialized()) {
        // tell ItemsManagerWorker to run an Update() after it's finished updating mods
        UpdateRequest(type, locations);
        spdlog::debug("Update deferred until item mods parsing is complete");
        emit NotifyUser(
            "This items worker is still initializing, but an update request has been queued.");
        return;
    }

    if (isUpdating() || m_delivering_terminal) {
        // The delivering-terminal case (D4/R4-4): the worker is observably
        // Idle during the RefreshFinished fan-out, but accepting an update
        // there could nest update N+1's signals — including its own
        // terminal — inside N's fan-out. Refused exactly as if an update
        // were active; a chained restart queues to the next loop turn.
        spdlog::warn("ItemsManagerWorker: update called while updating");
        emit NotifyUser("An update is already in progress.");
        return;
    }

    spdlog::debug("ItemsManagerWorker: updating {} tabs", type);
    if (spdlog::should_log(spdlog::level::trace)) {
        QStringList character_names;
        QStringList stash_names;
        for (const auto &location : locations) {
            switch (location.type()) {
            case ItemLocationType::CHARACTER:
                character_names.append(location.character());
                break;
            case ItemLocationType::STASH:
                stash_names.append(location.tab_label());
                break;
            }
        }
        spdlog::trace("ItemsManagerWorker: characters = {}", character_names.join(", "));
        spdlog::trace("ItemsManagerWorker: stashes = {}", stash_names.join(", "));
    }
    m_state = WorkerState::Updating;
    // A fresh stop_source per update (D2): every facade call below takes this
    // update's token, and a terminal failure request_stop()s it. The previous
    // update's source (stopped or not) is dropped here, so the new update's
    // calls carry a distinct, unstopped token — which is what makes the token
    // the update's sole identity under the post-await check (W-IDENTITY).
    m_stop_source = std::stop_source{};
    m_request_failures = 0;
    m_update_tab_contents = (type != TabSelection::TabsOnly);

    // The terminal vocabulary resets at each accepted update (D4): the
    // first-error slot and the deterministic skip list belong to this
    // update alone.
    m_first_error = RateLimit::FetchError{};
    m_first_error_set = false;
    m_skipped_sources.clear();

    // Reset the completion counters here, once per update: this is their only
    // reset. Each lane's LaunchContent() accumulates into them (never resets),
    // so a fresh update must not inherit a previous one's totals.
    m_stashes_needed = 0;
    m_stashes_received = 0;
    m_characters_needed = 0;
    m_characters_received = 0;

    // Build the content selection. Nothing is culled here: tab entries are
    // reconciled against the fresh lists as they arrive, and each tab's
    // items are replaced atomically when its reply arrives, so a failed
    // update never leaves anything missing (F28).
    m_update_all = (type == TabSelection::All) || (type == TabSelection::TabsOnly);
    m_tabs_to_update.clear();
    switch (type) {
    case TabSelection::All:
        spdlog::debug("ItemsManagerWorker: updating all tabs and items.");
        break;
    case TabSelection::TabsOnly:
        spdlog::debug("ItemsManagerWorker: updating stash and character lists.");
        break;
    case TabSelection::Checked:
        // Use the buyout manager to determine which tabs are checked.
        // Folder-tree A2 (R9, D10): a folder id is never read from the
        // stored flags; a Folder tab joins the selection — and keeps being
        // fetched — when at least one of its member tabs is in it.
        spdlog::trace("ItemsManagerWorker: updating checked tabs.");
        for (auto const &tab : m_tabs) {
            if (tab.IsValid() && !tab.IsFolder() && m_buyout_manager.GetRefreshChecked(tab)) {
                m_tabs_to_update.emplace(tab.id());
                if (!tab.folder_id().isEmpty()) {
                    m_tabs_to_update.emplace(tab.folder_id());
                }
            }
        }
        break;
    case TabSelection::Selected:
        // Use the function argument to determine which tabs were selected.
        spdlog::trace("ItemsManagerWorker::Update() updating selected tabs");
        for (auto const &tab : locations) {
            if (tab.IsValid()) {
                m_tabs_to_update.emplace(tab.id());
            }
        }
        break;
    }

    // Fetch a list only if the selection needs it: always for full updates,
    // otherwise only for the location types present in the selection.
    if (m_update_all) {
        m_need_stash_list = true;
        m_need_character_list = true;
    } else {
        spdlog::debug("Updating {} tabs.", m_tabs_to_update.size());
        m_need_stash_list = false;
        m_need_character_list = false;
        for (auto const &tab : m_tabs) {
            if (m_tabs_to_update.count(tab.id()) == 0) {
                continue;
            }
            switch (tab.type()) {
            case ItemLocationType::STASH:
                m_need_stash_list = true;
                break;
            case ItemLocationType::CHARACTER:
                m_need_character_list = true;
                break;
            }
        }
    }

    m_has_stash_list = false;
    m_has_character_list = false;

    // Every per-update counter and selection flag above is initialized before
    // the root orchestration launches its first fetch: a ready/fail-fast future
    // can run its continuation synchronously during launch (S1-6/IR2), so nothing
    // it touches may still be uninitialized. Run the root orchestration last.
    RunUpdate();
}

void ItemsManagerWorker::RebaseItemLocations(ItemLocationType type)
{
    // After a list reconciliation m_tabs carries fresh metadata, but
    // surviving items still embed the ItemLocation they were parsed with.
    // Rebase them so a renamed or moved tab is fresh everywhere the UI
    // reads a location (search buckets, headers, forum codes), not just in
    // the tab list. Runs at the snapshot boundary only (FinishUpdate), so
    // walking every bucket's items is permitted; rebasing never touches the
    // key fields (type, id, fetch_id), so the bucket structure stays valid.
    std::unordered_map<QString, const ItemLocation *> fresh_tabs;
    for (const auto &tab : m_tabs) {
        if (tab.type() == type) {
            fresh_tabs.emplace(tab.id(), &tab);
        }
    }
    for (const auto &[key, bucket] : m_items.buckets()) {
        if (key.type != type) {
            continue;
        }
        const auto it = fresh_tabs.find(bucket.front()->location().id());
        if (it != fresh_tabs.end()) {
            for (const auto &item : bucket) {
                item->RebaseLocation(*it->second);
            }
        }
    }
}

void ItemsManagerWorker::RunUpdate()
{
    // The root orchestration (D6, rev. 10): it launches the update's required
    // list(s) without awaiting one another and returns; the per-fetch tasks
    // self-drive from there and the completion that reconciles the counters
    // finalizes the update. This is an ordinary synchronous method, not a
    // coroutine: it never co_awaits (the fan-out is counter-driven, not a linear
    // await), so there is no asynchronous lifetime for an owned task handle to
    // manage — only the per-fetch tasks suspend and are owned in m_fetch_tasks.
    //
    // The whole body is wrapped in a catch-all (R5-1): a throw in orchestration
    // itself aborts and finalizes the update rather than unwinding into the
    // caller (ItemsManager / the UI). A plain function's try/catch does this
    // exactly as a coroutine's would, since nothing here suspends.
    try {
        spdlog::trace("ItemsManagerWorker: starting the update");
        // Root orchestration fault site (test-only; no-op in production).
        FireFaultHook();
        // Launch every required list without awaiting one another (D6): with
        // ordinary pending futures both are concurrently outstanding, resolving
        // F56's one-lane serialization. Each submission is made unconditionally
        // even if the first ran a ready/fail-fast handler inline that already
        // stopped the token — the second list simply carries that stopped token
        // and completes through the ordinary cancellation path. Every per-update
        // counter was initialized in Update() before this launch (IR2/S1-6).
        if (m_need_stash_list) {
            SubmitStashListRequest();
        }
        if (m_need_character_list) {
            SubmitCharacterListRequest();
        }
        CheckUpdateFinished();
    } catch (const std::exception &e) {
        spdlog::error("ItemsManagerWorker: the update orchestration threw: {}", e.what());
        RecordFirstError(MakeInternalError(
            QString("the update orchestration threw: %1").arg(e.what())));
        AbortUpdate();
    } catch (...) {
        spdlog::error("ItemsManagerWorker: the update orchestration threw an unknown exception");
        RecordFirstError(MakeInternalError("the update orchestration threw an unknown exception"));
        AbortUpdate();
    }
}

void ItemsManagerWorker::AbortUpdate()
{
    // The single, idempotent terminal transition for a failed update. Every
    // failure — a list error, a content error, an exceptional future, a throw in
    // orchestration or a handler — routes here. It is deliberately INDEPENDENT of
    // the completion counters: the first terminal failure returns the worker to
    // idle immediately (D6), abandoning in-flight siblings as stopped stragglers
    // that resolve Canceled later. It must never rewrite the counters to force
    // CheckUpdateFinished()'s success-equality — doing so drove `needed` backward
    // below `received` and made the reported progress non-monotonic (P-STATUS).
    // Success uses the counter equality in CheckUpdateFinished(); failure uses
    // this direct path, so the two never contend.
    if (m_state != WorkerState::Updating) {
        // Already terminal: a later straggler or a second failure must not emit a
        // second Ready/failure transition (P-FAILURE) or reset a live update.
        return;
    }
    m_stop_source.request_stop();
    // A failed update aborts on the first failure, so the count is at least one;
    // a handler that recorded nothing still yields a truthful "1 failed".
    if (m_request_failures == 0) {
        ++m_request_failures;
    }
    emit StatusUpdate(ProgramState::Ready,
                      QString("Update failed: %1 requests failed")
                          .arg(QString::number(m_request_failures)));
    m_state = WorkerState::Idle;
    spdlog::debug("Update failed.");

    // The typed terminal event (D4): after the Idle transition, exactly
    // once — the already-terminal guard above keeps later stragglers and
    // second failures out. Every failure path routes an error through
    // RecordFirstError before reaching here; the fallback keeps the emit
    // total even if a new path ever forgets (a bug, surfaced as Internal).
    const RateLimit::FetchError error = m_first_error_set
                                            ? m_first_error
                                            : MakeInternalError(
                                                  "the update failed without a recorded error");
    m_delivering_terminal = true;
    emit RefreshFinished(RefreshOutcome{FailedRefresh{error}});
    m_delivering_terminal = false;
}

void ItemsManagerWorker::StopUpdateForFailure(const RateLimit::FetchError &error)
{
    // The error is recorded BEFORE the throwable test fault hook (D4): the
    // stopped-but-still-active window the catch-alls must recognize already
    // carries the update's first terminal error.
    RecordFirstError(error);
    m_stop_source.request_stop();
    FireFaultHook();
}

void ItemsManagerWorker::RecordFirstError(const RateLimit::FetchError &error)
{
    if (!m_first_error_set) {
        m_first_error = error;
        m_first_error_set = true;
    }
}

void ItemsManagerWorker::FireFaultHook()
{
    if (m_fault_hook) {
        // One-shot: consume before invoking so a re-entrant path cannot fire it
        // again, and so it never fires on the recovery update.
        auto hook = std::move(m_fault_hook);
        m_fault_hook = nullptr;
        hook();
    }
}

void ItemsManagerWorker::SubmitStashListRequest()
{
    spdlog::trace("ItemsManagerWorker: requesting the stash list");
    m_fetch_tasks.push_back(FetchStashList(m_stop_source.get_token()));
}

void ItemsManagerWorker::SubmitCharacterListRequest()
{
    spdlog::trace("ItemsManagerWorker: requesting the character list");
    m_fetch_tasks.push_back(FetchCharacterList(m_stop_source.get_token()));
}

// Each per-fetch task wraps its ENTIRE body in a catch-all (R5-1): nothing —
// not an exceptional facade future, not the OnX handler, not a nested launch,
// not a connected persistence slot — may escape into the unawaited task, where
// it would vanish silently (S1-8) and wedge the update on a completion that
// never counts. There are two layers:
//   * the inner try converts an exceptional facade future into an ordinary
//     Internal failure value, so it flows through the handler's failure branch;
//   * the outer try contains anything the handler (or a launch it triggers)
//     throws, and forces a terminal AbortUpdate() so the update cannot wedge.
// The post-await identity gate is centralized in ProcessIfLive (see above): each
// of the four fetch coroutines routes its handler through that one helper right
// after its co_await, so there is a single gate to protect rather than four
// copies, and one W-IDENTITY mutation regression-covers them all. A future can
// resolve an instant before request_stop(), and its consumer must not touch
// state that now belongs to a later update; the token IS the update's identity.
// The sweep is scheduled unconditionally on every exit so no completed handle
// leaks.
QCoro::Task<> ItemsManagerWorker::FetchStashList(std::stop_token token)
{
    try {
        Result<poe::StashListWrapper> result;
        try {
            // Move the payload out (S1-7): the worker is the single consumer.
            auto future = m_api.listStashes(m_realm, m_league, token);
            result = co_await qCoro(future).takeResult();
        } catch (...) {
            result = std::unexpected(MakeInternalError("the stash list fetch threw"));
        }
        ProcessIfLive(token, [&] { OnStashListReceived(result); });
    } catch (...) {
        spdlog::error("ItemsManagerWorker: a stash list continuation threw; aborting the update");
        // A handler that reached this catch ran because the token was live at the
        // post-await gate, so it belongs to the active update — even if its own
        // failure branch stopped the token before throwing. Abort it. A resumed
        // straggler is gated out above and never enters the handler, so it never
        // reaches here; and AbortUpdate() guards on WorkerState and is idempotent,
        // so an already-terminal update is left untouched. RecordFirstError is
        // first-wins, so a handler that already recorded its own error (the
        // fault-hook window) keeps it (D4).
        RecordFirstError(MakeInternalError("a stash list continuation threw"));
        AbortUpdate();
    }
    ScheduleSweep();
}

QCoro::Task<> ItemsManagerWorker::FetchCharacterList(std::stop_token token)
{
    try {
        Result<poe::CharacterListWrapper> result;
        try {
            auto future = m_api.listCharacters(m_realm, token);
            result = co_await qCoro(future).takeResult();
        } catch (...) {
            result = std::unexpected(MakeInternalError("the character list fetch threw"));
        }
        ProcessIfLive(token, [&] { OnCharacterListReceived(result); });
    } catch (...) {
        spdlog::error(
            "ItemsManagerWorker: a character list continuation threw; aborting the update");
        RecordFirstError(MakeInternalError("a character list continuation threw"));
        AbortUpdate();
    }
    ScheduleSweep();
}

QCoro::Task<> ItemsManagerWorker::FetchStash(ItemLocation location,
                                             QString stash_id,
                                             QString substash_id,
                                             std::stop_token token)
{
    try {
        Result<poe::StashPayload> result;
        try {
            auto future = m_api.getStash(m_realm, m_league, stash_id, substash_id, token);
            result = co_await qCoro(future).takeResult();
        } catch (...) {
            result = std::unexpected(MakeInternalError("the stash fetch threw"));
        }
        ProcessIfLive(token, [&] { OnStashReceived(result, location); });
    } catch (...) {
        spdlog::error("ItemsManagerWorker: a stash continuation threw; aborting the update");
        RecordFirstError(MakeInternalError("a stash continuation threw"));
        AbortUpdate();
    }
    ScheduleSweep();
}

QCoro::Task<> ItemsManagerWorker::FetchCharacter(ItemLocation location,
                                                 QString name,
                                                 std::stop_token token)
{
    try {
        Result<poe::CharacterPayload> result;
        try {
            auto future = m_api.getCharacter(m_realm, name, token);
            result = co_await qCoro(future).takeResult();
        } catch (...) {
            result = std::unexpected(MakeInternalError("the character fetch threw"));
        }
        ProcessIfLive(token, [&] { OnCharacterReceived(result, location); });
    } catch (...) {
        spdlog::error("ItemsManagerWorker: a character continuation threw; aborting the update");
        RecordFirstError(MakeInternalError("a character continuation threw"));
        AbortUpdate();
    }
    ScheduleSweep();
}

void ItemsManagerWorker::ProcessTab(const poe::StashTab &tab, std::vector<ItemsRequest> &batch)
{
    // The modern API documents stash ids as 10 hexadecimal digits; longer
    // ids were a legacy-API artifact. Warn if one ever shows up.
    if (tab.id.size() > 10) {
        spdlog::warn("Stash tab unique id is longer than 10 characters: {}", tab.name);
    }

    ItemLocation location(tab);

    // Guard against duplicates within one list (and re-processed children).
    if (m_tab_id_index.count(location.id()) > 0) {
        return;
    }

    // Add this tab with fresh metadata (name, colour, position).
    m_tabs.push_back(location);
    m_tab_id_index.insert(location.id());

    // Fetch this tab's contents only if it is part of the update selection.
    // A newly discovered tab (not in the selection) is added to the tab list
    // above but left unfetched: a partial refresh touches only what it was
    // asked to, so new tabs wait for a full refresh or an explicit selection
    // rather than turning a partial refresh into a full one (F55, revised).
    const bool selected = m_update_all || (m_tabs_to_update.count(location.id()) > 0);
    if (m_update_tab_contents && selected) {
        batch.push_back(ItemsRequest{StashFetch{location.id(), {}}, location});
    }

    // Process any children.
    WarnMisfiledFolderChildren(tab);
    if (tab.children) {
        // These children need to be added to the database, since they weren't included
        // in the original list.
        emit stashListReceived(*tab.children, m_realm, m_league);
        for (const auto &child : *tab.children) {
            ProcessTab(child, batch);
        }
    }
}

void ItemsManagerWorker::OnStashListReceived(const Result<poe::StashListWrapper> &result)
{
    spdlog::trace("ItemsManagerWorker::OnStashListReceived() entered");

    if (!result) {
        spdlog::warn("Aborting update because there was an error fetching the stash list ({}): {}",
                     RateLimit::ToString(result.error().kind),
                     result.error().message);
        // First-failure terminal (D2/D6): stop the token so any sibling launched
        // concurrently — the character list or its content — resolves Canceled,
        // then take the direct terminal path. AbortUpdate() returns the worker to
        // idle immediately without touching the progress counters, so a six-tab
        // batch reporting 0/6 does not lurch to 1/1 (P-STATUS).
        StopUpdateForFailure(result.error());
        AbortUpdate();
        return;
    }

    const auto &stashes = result->stashes;

    emit stashListReceived(stashes, m_realm, m_league);
    // Only a fresh top-level list may drive datastore deletion — never the
    // partial stashListReceived re-emits ProcessTab makes for folder
    // children (F53).
    emit stashListReplaced(stashes, m_realm, m_league);

    spdlog::debug("Received stash list, there are {} stash tabs", stashes.size());

    // The fresh list is authoritative for stash tabs: rebuild the stash
    // entries of m_tabs from it (fresh metadata, server-deleted tabs
    // dropped), keeping character entries untouched.
    std::vector<ItemLocation> kept_tabs;
    kept_tabs.reserve(m_tabs.size());
    for (auto const &tab : m_tabs) {
        if (tab.type() != ItemLocationType::STASH) {
            kept_tabs.push_back(tab);
        }
    }
    m_tabs = std::move(kept_tabs);
    m_tab_id_index.clear();
    for (auto const &tab : m_tabs) {
        m_tab_id_index.insert(tab.id());
    }

    // Accumulate this list's whole content batch (D6): ProcessTab appends each
    // selected tab — and, recursively, its folder children — in source
    // traversal order.
    std::vector<ItemsRequest> batch;
    for (const auto &tab : stashes) {
        // This will process tabs recursively.
        ProcessTab(tab, batch);
    }
    spdlog::debug("Requesting {} out of {} stash tabs", batch.size(), stashes.size());

    // Drop items belonging to stash tabs the server no longer lists. A
    // bucket's representative location stands for all its items (a child
    // bucket's stable id is its display parent's), so a deleted parent
    // takes its children's buckets with it — the same predicate the old
    // per-item pass applied.
    const size_t dropped = m_items.EraseSourcesIf(
        [this](const FetchSourceKey &key, const ItemLocation &location) {
            return (key.type == ItemLocationType::STASH)
                   && (m_tab_id_index.count(location.id()) == 0);
        });
    if (dropped > 0) {
        spdlog::debug("ItemsManagerWorker: dropped {} items from deleted stash tabs", dropped);
    }

    m_has_stash_list = true;

    // Launch this list's complete stash content batch now (D6): it is never
    // held behind the character list, which RunUpdate already requested and
    // whose own content launches from its handler. ProcessTab accumulated the
    // batch above in source traversal order; LaunchContent launches it all at
    // once.
    LaunchContent(std::move(batch));
    CheckUpdateFinished();
}

void ItemsManagerWorker::OnCharacterListReceived(const Result<poe::CharacterListWrapper> &result)
{
    spdlog::trace("ItemsManagerWorker::OnCharacterListReceived() entered");

    if (!result) {
        spdlog::warn("Aborting update because there was an error fetching the character list "
                     "({}): {}",
                     RateLimit::ToString(result.error().kind),
                     result.error().message);
        // First-failure terminal (D2/D6): stop the token so the concurrently
        // launched stash list or its content resolves Canceled, then take the
        // direct terminal path — see the stash-list branch above.
        StopUpdateForFailure(result.error());
        AbortUpdate();
        return;
    }

    const auto &characters = result->characters;

    emit characterListReceived(characters, m_realm);
    // The fresh list is authoritative for the datastore too (F53).
    emit characterListReplaced(characters, m_realm);

    // The fresh list is authoritative for characters: rebuild the character
    // entries of m_tabs from it, keeping stash entries untouched. This also
    // fixes the old skip-check, which compared character *names* against an
    // index keyed by character *ids* and so never matched (F48): a partial
    // update used to re-add and re-fetch every character in the league.
    std::vector<ItemLocation> kept_tabs;
    kept_tabs.reserve(m_tabs.size());
    for (auto const &tab : m_tabs) {
        if (tab.type() != ItemLocationType::CHARACTER) {
            kept_tabs.push_back(tab);
        }
    }
    m_tabs = std::move(kept_tabs);
    m_tab_id_index.clear();
    for (auto const &tab : m_tabs) {
        m_tab_id_index.insert(tab.id());
    }

    std::vector<ItemsRequest> batch;

    for (auto &character : characters) {
        const QString name = character.name;
        const QString realm = character.realm;
        const QString league = character.league.value_or("");
        if (realm != m_realm) {
            spdlog::trace("Skipping {} because this character is not in realm {}", name, m_realm);
            continue;
        }
        if (league != m_league) {
            spdlog::trace("Skipping {} because this character is not in league {}", name, m_league);
            continue;
        }
        ItemLocation location(character, int(m_tabs.size()));
        if (m_tab_id_index.count(location.id()) > 0) {
            spdlog::trace("Skipping {} because it appears twice in the character list.", name);
            continue;
        }
        m_tabs.push_back(location);
        m_tab_id_index.insert(location.id());

        // Fetch this character's items only if it is part of the update
        // selection (same rule as ProcessTab): a newly discovered character
        // waits for a full refresh or an explicit selection (F55, revised).
        const bool selected = m_update_all || (m_tabs_to_update.count(location.id()) > 0);
        if (m_update_tab_contents && selected) {
            batch.push_back(ItemsRequest{CharacterFetch{name}, location});
        }
    }
    spdlog::debug("There are {} characters to update in '{}'", batch.size(), m_league);

    // Drop items belonging to characters the server no longer lists.
    const size_t dropped = m_items.EraseSourcesIf(
        [this](const FetchSourceKey &key, const ItemLocation &location) {
            return (key.type == ItemLocationType::CHARACTER)
                   && (m_tab_id_index.count(location.id()) == 0);
        });
    if (dropped > 0) {
        spdlog::debug("ItemsManagerWorker: dropped {} items from deleted characters", dropped);
    }

    m_has_character_list = true;

    // Launch this list's complete character content batch now (D6): it is
    // independent of the stash lane, so it is never held behind the stash list.
    LaunchContent(std::move(batch));
    CheckUpdateFinished();
}

void ItemsManagerWorker::OnStashReceived(const Result<poe::StashPayload> &result,
                                         const ItemLocation &location)
{
    spdlog::trace("ItemsManagerWorker::OnStashReceived() entered");

    // Every failure arrives as one typed FetchError — including a 200 whose
    // reply lacked its stash sub-object, which the facade classifies as
    // Parse (F62, M2 D5): a successful payload always holds a stash.
    if (!result) {
        // D5: a Parse failure on a content fetch is deterministic — retrying
        // is futile, and one bad tab must not abort an hours-long update
        // (the 3.29 flags:[] incident class). Skip and continue: record the
        // skipped source, count the fetch as received so the counter join
        // still reconciles (P-STATUS), emit NO delta — the atomic replace
        // never ran, so this source's previous items survive, in memory and
        // datastore. Everything else stays first-failure terminal.
        if (result.error().kind == RateLimit::FetchError::Kind::Parse) {
            spdlog::warn("Skipping stash '{}' because its payload failed to parse: {}",
                         location.GetHeader(),
                         result.error().message);
            m_skipped_sources.push_back(
                SkippedSource{FetchSourceKey::ForLocation(location), result.error()});
            ++m_stashes_received;
            SendStatusUpdate();
            CheckUpdateFinished();
            return;
        }
        spdlog::warn("Aborting update because there was an error fetching the stash ({}): {}",
                     RateLimit::ToString(result.error().kind),
                     result.error().message);
        // First-failure terminal (D2/D6): stop the token and take the direct
        // terminal path. The progress counters are left untouched — this failed
        // fetch is not counted as received, and the batch's `needed` total is not
        // snapped down, so reported progress stays monotonic (P-STATUS).
        StopUpdateForFailure(result.error());
        AbortUpdate();
        return;
    }

    const auto &stash = result->stash;

    // The bytes ride along untouched (F62): the facade parsed this stash
    // from exactly this substring of the reply, and the datastore stores it.
    emit stashReceived(stash, result->bytes, m_realm, m_league);

    // Atomically replace whatever this request fetched last time: for a
    // normal tab that is the tab's items, for a child of a special tab it
    // is just that child's share of the parent location's items. One bucket
    // swap in the source-keyed store — O(replaced + delta), never a pass
    // over the collection (D3, post-M2-M2).
    Items delta;
    if (DiscardFolderItems(stash)) {
        // Folder-tree D12: nothing enters the store; the empty delta below
        // still carries the folder's metadata anchor (R9 keeps it fetched).
    } else if (stash.items) {
        const auto &items = *stash.items;
        if (items.size() > 0) {
            ParseItems(items, location, delta);
        } else {
            spdlog::debug("Stash 'items' does not contain any items: {}", location.GetHeader());
        }
    } else {
        spdlog::debug("Stash does not have an 'items' array: {}", location.GetHeader());
    }
    const size_t replaced = m_items.ReplaceSource(FetchSourceKey::ForLocation(location), delta);
    spdlog::debug("ItemsManagerWorker: replacing {} items fetched by '{}'",
                  replaced,
                  location.fetch_id());

    // Presentation delta (M2 D3): the exact replacement just applied for this
    // fetch source, sharing its shared_ptrs, emitted after the atomic replace
    // and before the counter increment.
    emit TabRefreshed(location, delta);

    ++m_stashes_received;
    SendStatusUpdate();

    bool get_children{false};

    if (stash.type == "MapStash") {
        get_children = m_settings.value("get_map_stashes", false).toBool();
    } else if (stash.type == "UniqueStash") {
        get_children = m_settings.value("get_unique_stashes", false).toBool();
    } else if (stash.type == "Folder") {
        get_children = true;
    }

    std::vector<ItemsRequest> batch;
    WarnMisfiledFolderChildren(stash);
    if (get_children && stash.children) {
        spdlog::debug("ItemsManagerWorker: getting children ({}) of {} '{}' ({})",
                      stash.children->size(),
                      stash.type,
                      stash.name,
                      stash.id);
        for (const auto &child : *stash.children) {
            // F49 tripwire: a child that is already a known tab may also
            // have been queued from the stash list, in which case this
            // request fetches it a second time (during a partial refresh a
            // known-but-unselected child is never queued from the list, so
            // this can be its only fetch). Warn (visible in the Event Log)
            // so a live account with folder tabs can confirm or refute the
            // finding.
            if (m_tab_id_index.count(child.id) > 0) {
                spdlog::warn("F49: child '{}' ({}) of {} '{}' is already a known tab; "
                             "its contents may be fetched twice this update",
                             child.name,
                             child.id,
                             stash.type,
                             stash.name);
            }
            // The child's items display under the parent's location, but
            // they are fetched (and atomically replaced) by the child's id.
            // Accumulate the whole child batch first; LaunchContent below counts
            // it into m_stashes_needed before launching any of it, so a ready
            // child future cannot complete against a half-tallied count.
            //
            // Folder-tree amendment (S1): a Folder's reply-discovered child
            // (defensive — unobserved live, F49) displays under its own tab,
            // as the folder's listed children and the cached rows of the
            // same children do, so no item ever displays under a folder.
            ItemLocation child_location = location;
            if (stash.type == "Folder") {
                child_location = ItemLocation(child);
            }
            child_location.setFetchId(child.id);
            batch.push_back(ItemsRequest{StashFetch{stash.id, child.id}, child_location});
        }
    }

    // The parent's reply is authoritative for its children: drop cached
    // items that display under this tab but were fetched from a child it
    // no longer lists — or whose fetching is disabled in the settings.
    // Their fetch ids are never re-fetched, so nothing else would ever
    // replace or remove them, not even a full refresh.
    if (location.fetch_id() == location.id()) {
        std::vector<FetchSourceKey> expected{{location.type(), location.id()}};
        QStringList child_ids;
        if (get_children && stash.children) {
            for (const auto &child : *stash.children) {
                expected.push_back({ItemLocationType::STASH, child.id});
                child_ids.append(child.id);
            }
        }
        // Mirror the reconcile into the datastore, but only for Map/Unique
        // parents — their child rows exist solely under parent replies
        // (F53). Folder children are governed by the top-level list, and a
        // folder's own reply carries no children in the live API (F49), so
        // keying off a folder reply would wipe its legitimate child rows.
        if ((stash.type == "MapStash") || (stash.type == "UniqueStash")) {
            emit stashChildrenReplaced(location.id(), child_ids, m_realm, m_league);
        }
        const std::set<FetchSourceKey> expected_keys(expected.begin(), expected.end());
        const size_t dropped = m_items.EraseSourcesIf(
            [&](const FetchSourceKey &key, const ItemLocation &item_location) {
                return (key.type == ItemLocationType::STASH)
                       && (item_location.id() == location.id())
                       && (expected_keys.count(key) == 0);
            });
        if (dropped > 0) {
            spdlog::debug("ItemsManagerWorker: dropped {} ghost child items from {}",
                          dropped,
                          location.GetHeader());
        }
        // Presentation-lane aggregate reconciliation (M2 D3, R5-2/R6-2):
        // emitted for every top-level parent reply — the same condition as
        // the ghost erase above, deliberately wider than the Map/Unique-only
        // condition gating the persistence-lane stashChildrenReplaced emit.
        // The expected set is authoritative, so consumers reconcile their own
        // baseline even where it diverged from worker memory.
        emit ChildrenReconciled(location, expected);
    }

    // Launch the child batch (D6): reply-discovered Map/Unique children go out
    // as one complete batch. An empty batch (a normal tab, or a parent whose
    // children are disabled) makes this a no-op.
    LaunchContent(std::move(batch));
    CheckUpdateFinished();
}

void ItemsManagerWorker::OnCharacterReceived(const Result<poe::CharacterPayload> &result,
                                             const ItemLocation &location)
{
    spdlog::trace("ItemsManagerWorker::OnCharacterReceived() entered");

    // As with stashes, a reply without its character sub-object is a facade
    // Parse error (F62, M2 D5), so one typed check covers every failure.
    if (!result) {
        // D5 skip-and-continue for deterministic Parse failures, exactly as
        // in OnStashReceived: no delta, previous items survive, the counter
        // join still reconciles.
        if (result.error().kind == RateLimit::FetchError::Kind::Parse) {
            spdlog::warn("Skipping character '{}' because its payload failed to parse: {}",
                         location.GetHeader(),
                         result.error().message);
            m_skipped_sources.push_back(
                SkippedSource{FetchSourceKey::ForLocation(location), result.error()});
            ++m_characters_received;
            SendStatusUpdate();
            CheckUpdateFinished();
            return;
        }
        spdlog::warn("Aborting update because there was an error fetching the character "
                     "({}): {}",
                     RateLimit::ToString(result.error().kind),
                     result.error().message);
        // First-failure terminal (D2/D6): stop the token and take the direct
        // terminal path, leaving the progress counters untouched (P-STATUS).
        StopUpdateForFailure(result.error());
        AbortUpdate();
        return;
    }

    const auto &character = result->character;

    emit characterReceived(character, result->bytes, m_realm);

    // Atomically replace this character's items: one bucket swap, as in
    // OnStashReceived.
    Items delta;
    const auto collections = {character.equipment,
                              character.inventory,
                              character.rucksack,
                              character.jewels};

    for (const auto &items : collections) {
        if (items) {
            ParseItems(*items, location, delta);
        }
    }
    const size_t replaced = m_items.ReplaceSource(FetchSourceKey::ForLocation(location), delta);
    spdlog::debug("ItemsManagerWorker: replacing {} items fetched by '{}'",
                  replaced,
                  location.fetch_id());

    // Presentation delta (M2 D3), as in OnStashReceived: after the atomic
    // replace, before the counter increment. A character reply discovers no
    // children, so there is no ChildrenReconciled here.
    emit TabRefreshed(location, delta);

    ++m_characters_received;
    SendStatusUpdate();

    // No follow-up launch (D6): a character reply discovers no children, and its
    // whole content batch went out at once when the character list arrived.
    CheckUpdateFinished();
}

void ItemsManagerWorker::LaunchContent(std::vector<ItemsRequest> batch)
{
    // The whole batch goes out at once (D6, F56): no worker-side pacing, no
    // one-at-a-time submission. The caller owns the batch as a local vector, so a
    // ready fetch whose handler discovers and launches its own child batch inline
    // builds a separate local batch — there is no shared queue for the inline
    // re-entry to mutate mid-iteration.
    if (batch.empty()) {
        // A TabsOnly update (no content) or a lane with nothing to fetch.
        return;
    }

    // Initialize the entire batch's needed counters BEFORE the first launch
    // (IR2/S1-6): a ready or fail-fast future runs its completion inline in the
    // launch loop below, and it must never observe a needed count still being
    // tallied. These accumulate across the update's lanes — the stash list, the
    // character list, and each parent reply's child batch each add their own
    // and never reset a sibling lane's progress (D6).
    for (const auto &request : batch) {
        switch (request.location.type()) {
        case ItemLocationType::STASH:
            ++m_stashes_needed;
            break;
        case ItemLocationType::CHARACTER:
            ++m_characters_needed;
            break;
        }
    }
    SendStatusUpdate();
    spdlog::debug("Launching a batch of {} item requests.", batch.size());

    const std::stop_token token = m_stop_source.get_token();

    // Each entry names the resource, not how to fetch it; launch an owned
    // per-fetch task per entry, in the batch's source traversal order (D6's
    // per-lane FIFO). Every handle lives in m_fetch_tasks — no fire-and-forget.
    for (const auto &request : batch) {
        const ItemLocation location = request.location;
        std::visit(
            [&](const auto &what) {
                using What = std::decay_t<decltype(what)>;
                if constexpr (std::is_same_v<What, StashFetch>) {
                    m_fetch_tasks.push_back(
                        FetchStash(location, what.stash_id, what.substash_id, token));
                } else {
                    m_fetch_tasks.push_back(FetchCharacter(location, what.name, token));
                }
            },
            request.what);
    }
}

void ItemsManagerWorker::ScheduleSweep()
{
    // Coalesce: many completions in one event-loop turn queue a single sweep.
    if (m_sweep_scheduled) {
        return;
    }
    m_sweep_scheduled = true;
    if (m_sweep_observer) {
        ++m_sweep_observer->scheduled;
    }
    // Queued, so the sweep runs on a later turn — outside the completing
    // coroutine whose final_suspend has not yet released its own handle
    // (D6/R5-1): finalization must never destroy the frame it is running in.
    QMetaObject::invokeMethod(this, [this]() { SweepTasks(); }, Qt::QueuedConnection);
}

void ItemsManagerWorker::SweepTasks()
{
    m_sweep_scheduled = false;
    int destroyed = 0;
    // Destroy completed handles only. isReady() is true for a finished (or
    // empty) coroutine; a suspended straggler is not ready and survives —
    // destroying its handle would detach the frame, not stop it (S1-1/S1-4),
    // and it would still resume later.
    std::erase_if(m_fetch_tasks, [&destroyed](const QCoro::Task<> &task) {
        if (task.isReady()) {
            ++destroyed;
            return true;
        }
        return false;
    });
    if (m_sweep_observer) {
        ++m_sweep_observer->executed;
        m_sweep_observer->destroyed += destroyed;
        m_sweep_observer->live_after = m_fetch_tasks.size();
    }
}

void ItemsManagerWorker::SendStatusUpdate()
{
    spdlog::trace("ItemsManagerWorker::SendStatusUpdate() entered");

    QString message;
    if ((m_stashes_needed > 0) && (m_characters_needed > 0)) {
        message = QString("Receieved %1/%2 stash tabs and %3/%4 character locations")
                      .arg(QString::number(m_stashes_received),
                           QString::number(m_stashes_needed),
                           QString::number(m_characters_received),
                           QString::number(m_characters_needed));
    } else if (m_stashes_needed > 0) {
        message = QString("Received %1/%2 stash tabs")
                      .arg(QString::number(m_stashes_received), QString::number(m_stashes_needed));
    } else if (m_characters_needed > 0) {
        message = QString("Received %1/%2 character locations")
                      .arg(QString::number(m_characters_received),
                           QString::number(m_characters_needed));
    } else if (!m_update_tab_contents) {
        message = "Received tab lists";
    } else {
        message = "Received nothing; needed nothing.";
    }
    emit StatusUpdate(ProgramState::Busy, message);
}

void ItemsManagerWorker::ParseItems(const std::vector<poe::Item> &items,
                                    const ItemLocation &base_location,
                                    Items &out) const
{
    for (auto &item : items) {
        const ItemLocation location = base_location.getItemLocation(item);
        out.push_back(std::make_shared<Item>(item, location));
        if (item.socketedItems) {
            ParseItems(*item.socketedItems, location, out);
        }
    }
}

void ItemsManagerWorker::CheckUpdateFinished()
{
    // The SUCCESS terminal only. A failed update never reaches its counter
    // equality — the first failure routes through AbortUpdate() and sets Idle
    // directly (D6) — so by the time this runs while still Updating, no failure
    // has occurred. It is called after every completion that could reconcile the
    // counters; when they line up with both required lists received, the update
    // succeeded.
    if (m_state != WorkerState::Updating) {
        return;
    }

    const bool lists_received = (!m_need_stash_list || m_has_stash_list)
                                && (!m_need_character_list || m_has_character_list);
    const bool items_received = (m_stashes_received == m_stashes_needed)
                                && (m_characters_received == m_characters_needed);
    if (!lists_received || !items_received) {
        return;
    }

    spdlog::trace("ItemsManagerWorker::CheckUpdateFinished() finishing update");
    FinishUpdate();
}

void ItemsManagerWorker::FinishUpdate()
{
    spdlog::trace("ItemsManagerWorker::FinishUpdate() entered");

    // It's possible that we receive character vs stash tabs out of order, or users
    // move items around in a tab and we get them in a different order. For
    // consistency we want to present the tab data in a deterministic way to the rest
    // of the application.  Especially so we don't try to update shop when nothing actually
    // changed.  So sort m_items here before emitting and then generate
    // item list as strings.

    QString message;
    if ((m_stashes_received > 0) && (m_characters_received > 0)) {
        message = QString("Received %1 stash tabs and %2 character locations")
                      .arg(QString::number(m_stashes_received),
                           QString::number(m_characters_received));
    } else if (m_stashes_received > 0) {
        message = QString("Received %1 stash tabs").arg(QString::number(m_stashes_received));
    } else if (m_characters_received > 0) {
        message = QString("Received %1 character locations")
                      .arg(QString::number(m_characters_received));
    } else if (!m_update_tab_contents) {
        message = "Received tab lists";
    } else {
        message = "Received nothing";
    }
    // Skips are user-visible (D5/R1-1): a completed-with-skips refresh must
    // never be indistinguishable from a clean one. Each skipped source was
    // already named in the log at warn level when it was skipped.
    if (!m_skipped_sources.empty()) {
        message += QString(", %1 skipped").arg(QString::number(m_skipped_sources.size()));
    }

    emit StatusUpdate(ProgramState::Ready, message);

    // Rebase surviving items' locations onto the fresh tab metadata only
    // now that the update has succeeded: the emitted Items share Item
    // objects with ItemsManager and the UI, so rebasing any earlier would
    // mutate the already-published snapshot mid-update — and a terminal
    // failure would leave it mutated, with no emit to rebuild the search
    // buckets around the new metadata.
    RebaseItemLocations(ItemLocationType::STASH);
    RebaseItemLocations(ItemLocationType::CHARACTER);

    // Sort tabs.
    std::sort(begin(m_tabs), end(m_tabs));

    // Sort items (the snapshot's deterministic presentation order; the flat
    // view is rebuilt here if any delta dirtied it).
    m_items.SortFlat([](const std::shared_ptr<Item> &a, const std::shared_ptr<Item> &b) {
        return *a < *b;
    });

    // Let everyone know the update is done.
    spdlog::trace("ItemsManagerWorker::FinishUpdate() emitting ItemsRefreshed");
    emit ItemsRefreshed(m_items.Flat(), m_tabs, false);

    m_state = WorkerState::Idle;
    spdlog::debug("Update finished.");

    // The typed terminal event (D4), pinned ordering: final ItemsRefreshed,
    // then Idle, then RefreshFinished — the fan-out observes an idle worker
    // but a reentrant Update() is refused via the delivering flag (R4-4).
    m_delivering_terminal = true;
    emit RefreshFinished(RefreshOutcome{CompletedRefresh{m_skipped_sources}});
    m_delivering_terminal = false;
}
