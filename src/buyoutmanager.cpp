// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "buyoutmanager.h"

#include <QRegularExpression>
#include <QVariant>

#include <utility>

#include "datastore/buyoutrepo.h"
#include "datastore/datastore.h"
#include "item.h"
#include "itemlocation.h"
#include "util/glaze_qt.h"  // IWYU pragma: keep
#include "util/spdlog_qt.h" // IWYU pragma: keep

const std::unordered_map<QString, BuyoutType> BuyoutManager::m_string_to_buyout_type = {
    {"~gb/o", Buyout::BUYOUT_TYPE_BUYOUT},
    {"~b/o", Buyout::BUYOUT_TYPE_BUYOUT},
    {"~c/o", Buyout::BUYOUT_TYPE_CURRENT_OFFER},
    {"~price", Buyout::BUYOUT_TYPE_FIXED},
};

BuyoutManager::BuyoutManager(DataStore &data, BuyoutRepo &repo)
    : m_data(data)
    , m_repo(repo)
    , m_save_needed(false)
{
    Load();
}

BuyoutManager::~BuyoutManager()
{
    Save();
}

void BuyoutManager::BeginBatch()
{
    ++m_batch_depth;
}

void BuyoutManager::EndBatch()
{
    if (m_batch_depth <= 0) {
        spdlog::error("BuyoutManager: EndBatch() without a matching BeginBatch()");
        m_batch_depth = 0;
        return;
    }
    // Only the outermost boundary emits (R3-3): an inner pass or command
    // boundary leaves its accumulated scope for the enclosing batch.
    if (--m_batch_depth == 0) {
        EmitPendingChanges();
    }
}

void BuyoutManager::RecordChange(ChangeScope scope, const QString &id)
{
    switch (scope) {
    case ChangeScope::Item:
        m_pending_changes.item_ids.insert(id);
        break;
    case ChangeScope::Tab:
        m_pending_changes.tab_ids.insert(id);
        break;
    case ChangeScope::Everything:
        m_pending_changes.everything = true;
        break;
    }
    // A mutation with no batch open is its own outer boundary: immediacy
    // is the default; batching is opted into by commands and passes.
    if (m_batch_depth == 0) {
        EmitPendingChanges();
    }
}

void BuyoutManager::EmitPendingChanges()
{
    if (m_pending_changes.IsEmpty()) {
        return;
    }
    const BuyoutChangeSet changes = std::exchange(m_pending_changes, {});
    emit BuyoutsChanged(changes);
}

bool BuyoutManager::Set(const Item &item, const Buyout &buyout)
{
    if (buyout.type == Buyout::BUYOUT_TYPE_CURRENT_OFFER) {
        spdlog::warn("BuyoutManager: tried to set an obsolete 'current offer' buyout for {}: {}",
                     item.PrettyName(),
                     buyout.AsText());
    }

    // Nothing is written while the buyouts are not loaded (#257): the map
    // then lacks the user's prices, and a pricing pass that saw "no price"
    // would overwrite one it could not read.
    if (!m_loaded) {
        spdlog::debug("BuyoutManager: not saving a buyout for {}: buyouts are not loaded",
                      item.id());
        return false;
    }

    const auto it = m_buyouts.find(item.id());
    if (buyout.IsNull()) {
        // Only touch the database when the manager holds an entry to clear:
        // PropagateTabBuyouts clears nearly every item on every refresh, which
        // used to issue one no-op DELETE per item (F52).
        if (it == m_buyouts.end()) {
            return true;
        }
        // The map entry is erased only after a successful delete so that a
        // failed delete is retried the next time this entry is cleared,
        // instead of leaving the row to resurrect at the next Load().
        if (!m_repo.removeItemBuyout(item)) {
            return false;
        }
        m_buyouts.erase(it);
        RecordChange(ChangeScope::Item, item.id());
        return true;
    }

    if ((it != m_buyouts.end()) && (it->second == buyout)) {
        return true;
    }
    // Likewise the map takes the new price only once the row is saved, so
    // it never shows a price the database does not hold.
    if (!m_repo.saveItemBuyout(buyout, item)) {
        return false;
    }
    m_buyouts[item.id()] = buyout;
    RecordChange(ChangeScope::Item, item.id());
    return true;
}

Buyout BuyoutManager::Get(const Item &item) const
{
    const auto &it = m_buyouts.find(item.id());
    if (it != m_buyouts.end()) {
        Buyout buyout = it->second;
        if (buyout.type == Buyout::BUYOUT_TYPE_CURRENT_OFFER) {
            spdlog::warn("BuyoutManager: detected an obsolete 'current offer' buyout for {}: {}",
                         item.PrettyName(),
                         buyout.AsText());
        }
        return buyout;
    }
    return Buyout();
}

Buyout BuyoutManager::GetTab(const ItemLocation &location) const
{
    const QString tab = location.id();
    const auto &it = m_tab_buyouts.find(tab);
    if (it != m_tab_buyouts.end()) {
        Buyout buyout = it->second;
        if (buyout.type == Buyout::BUYOUT_TYPE_CURRENT_OFFER) {
            spdlog::warn(
                "BuyoutManager: detected an obsolete 'current offer' tab buyout for {}: {}",
                tab,
                buyout.AsText());
        }
        return buyout;
    }
    return Buyout();
}

bool BuyoutManager::SetTab(const ItemLocation &location, const Buyout &buyout)
{
    if (buyout.type == Buyout::BUYOUT_TYPE_CURRENT_OFFER) {
        spdlog::warn(
            "BuyoutManager: tried to set an obsolete 'current offer' tab buyout for {}: {}",
            location.id(),
            buyout.AsText());
    }

    // Same rules as Set(): no writes unless loaded (#257), no no-op deletes
    // (F52), and the map changes only after the database did.
    if (!m_loaded) {
        spdlog::debug("BuyoutManager: not saving a tab buyout for {}: buyouts are not loaded",
                      location.id());
        return false;
    }

    const auto it = m_tab_buyouts.find(location.id());
    if (buyout.IsNull()) {
        if (it == m_tab_buyouts.end()) {
            return true;
        }
        if (!m_repo.removeLocationBuyout(location)) {
            return false;
        }
        m_tab_buyouts.erase(it);
        RecordChange(ChangeScope::Tab, location.id());
        return true;
    }

    if ((it != m_tab_buyouts.end()) && (it->second == buyout)) {
        return true;
    }
    if (!m_repo.saveLocationBuyout(buyout, location)) {
        return false;
    }
    m_tab_buyouts[location.id()] = buyout;
    RecordChange(ChangeScope::Tab, location.id());
    return true;
}

void BuyoutManager::CompressTabBuyouts()
{
    // When tabs are renamed we end up with stale tab buyouts that aren't deleted.
    // This function is to remove buyouts associated with tab names that don't
    // currently exist.
    std::set<QString> tmp;
    for (const auto &loc : m_tabs) {
        tmp.emplace(loc.id());
    }

    // One compression is one batch even when no caller encloses it: the
    // erasures coalesce into a single boundary that emits only after every
    // mutation is complete — a consumer must never observe pre-erase state.
    const BuyoutBatch batch(*this);
    for (auto it = m_tab_buyouts.begin(); it != m_tab_buyouts.end();) {
        if (tmp.count(it->first) == 0) {
            m_save_needed = true;
            const QString id = it->first;
            it = m_tab_buyouts.erase(it);
            RecordChange(ChangeScope::Tab, id);
        } else {
            ++it;
        }
    }
}

void BuyoutManager::SetRefreshChecked(const ItemLocation &loc, bool value)
{
    m_save_needed = true;
    m_refresh_checked[loc.id()] = value;
}

bool BuyoutManager::GetRefreshChecked(const ItemLocation &loc) const
{
    auto it = m_refresh_checked.find(loc.id());
    bool refresh_checked = (it != m_refresh_checked.end()) ? it->second : true;
    return (refresh_checked || GetRefreshLocked(loc));
}

bool BuyoutManager::GetRefreshLocked(const ItemLocation &loc) const
{
    return m_refresh_locked.count(loc.id());
}

void BuyoutManager::SetRefreshLocked(const ItemLocation &loc)
{
    m_refresh_locked.emplace(loc.id());
}

void BuyoutManager::ClearRefreshLocks()
{
    m_refresh_locked.clear();
}

QString BuyoutManager::Serialize(const std::unordered_map<QString, bool> &obj)
{
    const auto result = glz::write_json(obj);
    if (!result) {
        const auto msg = glz::format_error(result.error());
        spdlog::error("Error serializing boolean buyout map: {}", msg);
        return QString();
    }
    return QString::fromStdString(*result);
}

void BuyoutManager::Deserialize(const QString &data, std::unordered_map<QString, bool> &obj)
{
    obj.clear();

    // if data is empty (on first use) we shouldn't make user panic by showing ERROR messages
    if (data.isEmpty()) {
        return;
    }

    const QByteArray bytes{data.toUtf8()};
    const std::string_view sv{bytes.constData(), size_t(bytes.size())};
    const auto result = glz::read_json<std::unordered_map<QString, bool>>(sv);
    if (!result) {
        const auto msg = glz::format_error(result.error());
        spdlog::error("Error deserializing boolean buyout map: {}", msg);
        return;
    }

    for (const auto &[key, value] : *result) {
        obj[key] = value;
    }
}

void BuyoutManager::Save()
{
    if (m_save_needed) {
        m_data.Set("refresh_checked_state", Serialize(m_refresh_checked));
        m_save_needed = false;
    }
}

void BuyoutManager::Load()
{
    m_loaded = LoadBuyouts();
    Deserialize(m_data.Get("refresh_checked_state"), m_refresh_checked);
}

void BuyoutManager::ReloadBuyouts()
{
    m_loaded = LoadBuyouts();
    RecordChange(ChangeScope::Everything, {});
}

bool BuyoutManager::LoadBuyouts()
{
    auto items = m_repo.loadItemBuyouts();
    auto tabs = m_repo.loadLocationBuyouts();
    if (items) {
        m_buyouts = std::move(*items);
    }
    if (tabs) {
        m_tab_buyouts = std::move(*tabs);
    }
    const bool loaded = m_repo.storeUsable() && items && tabs;
    if (!loaded) {
        spdlog::error("BuyoutManager: buyouts did not load (store usable: {}, item read: {}, "
                      "tab read: {}); the forum shop will not be updated",
                      m_repo.storeUsable(),
                      items.has_value(),
                      tabs.has_value());
    }
    return loaded;
}

void BuyoutManager::SetStashTabLocations(const std::vector<ItemLocation> &tabs)
{
    m_tabs = tabs;
}

const std::vector<ItemLocation> &BuyoutManager::GetStashTabLocations() const
{
    return m_tabs;
}

BuyoutType BuyoutManager::StringToBuyoutType(QString bo_str) const
{
    const auto &it = m_string_to_buyout_type.find(bo_str);
    if (it != m_string_to_buyout_type.end()) {
        return it->second;
    }
    return Buyout::BUYOUT_TYPE_INHERIT;
}

Buyout BuyoutManager::StringToBuyout(QString format)
{
    // Parse format string and initialize buyout object, if string does not match any known format
    // then the buyout object will not be valid (IsValid will return false).
    static const QRegularExpression exp("(~\\S+)\\s+(\\d+\\.?\\d*)\\s+(\\w+)");

    Buyout tmp;
    // regex_search allows for stuff before ~ and after currency type.  We only want to honor the formats
    // that POE trade also accept so this may need to change if it's too generous
    QRegularExpressionMatch m = exp.match(format);
    if (m.hasMatch()) {
        tmp.type = StringToBuyoutType(m.captured(1));
        tmp.value = m.captured(2).toDouble();
        tmp.currency = Currency::FromString(m.captured(3));
        tmp.source = Buyout::BUYOUT_SOURCE_GAME;
        tmp.last_update = QDateTime::currentDateTime();
    }
    return tmp;
}
