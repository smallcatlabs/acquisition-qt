// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include "item.h"

#include <QDateTime>
#include <QObject>
#include <QString>

#include <set>
#include <unordered_map>

#include "buyout.h"

class Item;
class ItemLocation;
class DataStore;
class BuyoutRepo;

// The affected scope of one outer buyout batch (items-pipeline-m3.md D1
// rule 4 / R1-6): stable ids whose buyout lookup results changed. Item ids
// render in Price/Date cells and sort keys; tab ids render in bucket
// headers.
struct BuyoutChangeSet
{
    std::set<QString> item_ids;
    std::set<QString> tab_ids;
    // ReloadBuyouts() rewrites the whole lookup state; consumers treat
    // every visible row as affected.
    bool everything{false};

    bool IsEmpty() const { return !everything && item_ids.empty() && tab_ids.empty(); }
};

class BuyoutManager : public QObject
{
    Q_OBJECT
public:
    explicit BuyoutManager(DataStore &data, BuyoutRepo &repo);
    ~BuyoutManager();

    // M3 S2 batching (the five R1-6 rules): batches nest, and only the
    // outermost EndBatch emits BuyoutsChanged — a pass or command boundary
    // reached inside an enclosing batch emits nothing of its own (R3-3).
    // A mutation outside any batch emits immediately as its own batch.
    // Prefer the scoped BuyoutBatch guard to calling these directly.
    void BeginBatch();
    void EndBatch();

    void Set(const Item &item, const Buyout &buyout);
    void SetTab(const ItemLocation &location, const Buyout &buyout);

    Buyout Get(const Item &item) const;
    Buyout GetTab(const ItemLocation &location) const;

    void CompressTabBuyouts();

    void SetRefreshChecked(const ItemLocation &tab, bool value);
    bool GetRefreshChecked(const ItemLocation &tab) const;

    bool GetRefreshLocked(const ItemLocation &tab) const;
    void SetRefreshLocked(const ItemLocation &tab);
    void ClearRefreshLocks();

    void SetStashTabLocations(const std::vector<ItemLocation> &tabs);
    const std::vector<ItemLocation> &GetStashTabLocations() const;

    Buyout StringToBuyout(QString);

    void Save();
    void ReloadBuyouts();

    // Did the buyouts load? False when the store was not usable or either
    // read failed (#250): the maps then do not reflect the user's prices,
    // and nothing may post them to the forum.
    bool BuyoutsLoaded() const { return m_loaded; }

signals:
    void SetItemBuyout(const Buyout &buyout, const Item &item);
    void SetLocationBuyout(const Buyout &buyout, const ItemLocation &location);

    // One emission per outer batch boundary with a non-empty scope (M3 D1
    // rule 4): the model layer's cue to repaint affected Price/Date cells
    // and, when a buyout-dependent column sorts the view, reorder once.
    void BuyoutsChanged(const BuyoutChangeSet &changes);

private:
    // The single M3 choke point (D1 rule 4, R1-6): every mutation of the
    // buyout lookup state (m_buyouts / m_tab_buyouts) reports here — item
    // and tab set-and-clear, the tab compression the pricing passes run,
    // and a reload. Refresh-check state is not buyout lookup state and
    // does not report.
    enum class ChangeScope { Item, Tab, Everything };
    void RecordChange(ChangeScope scope, const QString &id);
    // Construction-time only (no model exists yet, nothing to notify);
    // private so the D1 rule 4 enumeration has no public non-reporting
    // mutation of the lookup state (M3 S8 design review).
    void Load();
    // Reads both buyout tables into the maps; returns whether the store was
    // usable and both reads succeeded. A failed read leaves its map as is.
    bool LoadBuyouts();
    void EmitPendingChanges();

    BuyoutType StringToBuyoutType(QString bo_str) const;

    QString Serialize(const std::unordered_map<QString, bool> &obj);
    void Deserialize(const QString &data, std::unordered_map<QString, bool> &obj);

    DataStore &m_data;
    BuyoutRepo &m_repo;

    std::unordered_map<QString, Buyout> m_buyouts;
    std::unordered_map<QString, Buyout> m_tab_buyouts;
    // Buyouts persist through BuyoutRepo signals. Refresh-check state is DataStore JSON in
    // Save()/Load(); this split is intentional (see F22).
    std::unordered_map<QString, bool> m_refresh_checked;
    std::set<QString> m_refresh_locked;
    bool m_save_needed;
    bool m_loaded{false};
    std::vector<ItemLocation> m_tabs;

    int m_batch_depth{0};
    BuyoutChangeSet m_pending_changes;

    static const std::unordered_map<QString, BuyoutType> m_string_to_buyout_type;
};

// Scoped batch boundary (M3 S2): user commands and pricing passes hold one
// across their mutations; nested guards coalesce into the outermost.
class BuyoutBatch
{
public:
    explicit BuyoutBatch(BuyoutManager &manager)
        : m_manager(manager)
    {
        m_manager.BeginBatch();
    }
    ~BuyoutBatch() { m_manager.EndBatch(); }
    BuyoutBatch(const BuyoutBatch &) = delete;
    BuyoutBatch &operator=(const BuyoutBatch &) = delete;

private:
    BuyoutManager &m_manager;
};
