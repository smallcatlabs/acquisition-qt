// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QString>
#include <QTimer>

#include <vector>

#include "fetchsourcekey.h"
#include "item.h"
#include "itemlocation.h"
#include "itemsmanagerworker.h"
#include "locationinventory.h"
#include "sourcekeyeditems.h"
#include "util/programstate.h"
#include "util/util.h"

class QSettings;

class BuyoutManager;
class Shop;

/*
 * ItemsManager manages an ItemsManagerWorker (which lives in a separate thread)
 * and glues it to the rest of Acquisition.
 * (No longer true as of v0.10.0, but we will see if this causes performance issues).
 */
class ItemsManager : public QObject
{
    Q_OBJECT
public:
    explicit ItemsManager(QSettings &settings, BuyoutManager &buyout_manager);
    ~ItemsManager();
    //bool isInitialized() const { return m_worker ? m_worker->isInitialized() : false; }
    //bool isUpdating() const { return m_worker ? m_worker->isUpdating() : false; }
    // Creates and starts the worker
    void Start();
    void Update(TabSelection type, const std::vector<ItemLocation> &tab_names = {});
    void SetAutoUpdateInterval(int minutes);
    void SetAutoUpdate(bool update);
    // The published collection as a flat vector, lazily rebuilt from the
    // source-keyed store after a delta (M2 D3, post-M2-M2): snapshot and
    // tick consumers only — nothing on the per-reply path calls this.
    const Items &items() const { return m_items.Flat(); }
    // The freshest tab metadata seen per stable display key (M2 D6): fed by
    // every delta's location anchor and reset by every snapshot. Search
    // buckets render through it.
    const LocationInventory &locationInventory() const { return m_location_inventory; }
    void ApplyAutoTabBuyouts();
    void ApplyAutoItemBuyouts();
    void PropagateTabBuyouts();
public slots:
    void OnAutoRefreshTimer();
    void OnStatusUpdate(ProgramState state, const QString &status);
    void OnItemsRefreshed(const Items &items,
                          const std::vector<ItemLocation> &tabs,
                          bool initial_refresh);
    // Streaming delta application (items-pipeline M2, D3/D6): each worker
    // delta is applied to the published copy — one predicate-only erase by
    // FetchSourceKey (the permitted linear pass, R1-2) plus an append —
    // priced for the delta's items only (D7), and re-emitted for the UI.
    void OnTabRefreshed(const ItemLocation &location, const Items &items);
    // An aggregate child reconciliation applies the authoritative expected
    // set to OUR OWN baseline (R5-2/R6-2): items under the parent's stable
    // id whose key is outside the set are erased — correct even where the
    // published copy diverged from worker memory across a failed update.
    void OnChildrenReconciled(const ItemLocation &parent,
                              const std::vector<FetchSourceKey> &expected);
signals:
    void UpdateSignal(Util::TabSelection type, const std::vector<ItemLocation> &tab_names = {});
    void ItemsRefreshed(bool initial_refresh);
    void TabRefreshed(const ItemLocation &location, const Items &items);
    void ChildrenReconciled(const ItemLocation &parent, const std::vector<FetchSourceKey> &expected);
    // The worker's typed terminal event, forwarded verbatim (M2 D4):
    // exactly once per accepted update, after every delta of that update.
    void RefreshFinished(const RefreshOutcome &outcome);
    void StatusUpdate(ProgramState state, const QString &status);
    void UpdateModListSignal();

private:
    // Scoped per-delta pricing (M2 D7): item-local, fail-safe on both
    // outcomes, O(delta items). The final whole-collection pass at
    // ItemsRefreshed stays authoritative.
    void ApplyScopedPricing(const Items &delta_items);

    QSettings &m_settings;
    BuyoutManager &m_buyout_manager;

    std::unique_ptr<QTimer> m_auto_update_timer;
    // Source-keyed published copy (M2 D3): the M2-M2 measurement fired the
    // spec's storage conditional, so the per-delta replace and the child
    // reconciliation are bucket operations, never O(all-items) passes.
    SourceKeyedItems m_items;
    LocationInventory m_location_inventory;
};
