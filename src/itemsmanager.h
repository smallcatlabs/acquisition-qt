// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QString>
#include <QTimer>

#include <vector>

#include "item.h"
#include "itemlocation.h"
#include "itemsmanagerworker.h"
#include "util/programstate.h"
#include "util/util.h"

class QSettings;

class BuyoutManager;
class DataStore;
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
    explicit ItemsManager(QSettings &settings, BuyoutManager &buyout_manager, DataStore &datastore);
    ~ItemsManager();
    //bool isInitialized() const { return m_worker ? m_worker->isInitialized() : false; }
    //bool isUpdating() const { return m_worker ? m_worker->isUpdating() : false; }
    // Creates and starts the worker
    void Start();
    void Update(TabSelection type, const std::vector<ItemLocation> &tab_names = {});
    void SetAutoUpdateInterval(int minutes);
    void SetAutoUpdate(bool update);
    const Items &items() const { return m_items; }
    void ApplyAutoTabBuyouts();
    void ApplyAutoItemBuyouts();
    void PropagateTabBuyouts();

    // SPIKE(S1-M2): apply one simulated per-tab delta — replace this fetch
    // source's items in the collection and emit TabRefreshed. This stands in
    // for the M2 worker's streamed delta; D7 pricing is deliberately out of
    // the spike's scope.
    void SpikeApplyTabDelta(const ItemLocation &tab, const Items &tab_items);
public slots:
    void OnAutoRefreshTimer();
    void OnStatusUpdate(ProgramState state, const QString &status);
    void OnItemsRefreshed(const Items &items,
                          const std::vector<ItemLocation> &tabs,
                          bool initial_refresh);
signals:
    void UpdateSignal(Util::TabSelection type, const std::vector<ItemLocation> &tab_names = {});
    void ItemsRefreshed(bool initial_refresh);

    // SPIKE(S1-M2): prototype of the M2 streamed per-tab delta signal.
    void TabRefreshed(const ItemLocation &location, const Items &delta_items);
    void StatusUpdate(ProgramState state, const QString &status);
    void UpdateModListSignal();

private:
    void MigrateBuyouts();

    QSettings &m_settings;
    BuyoutManager &m_buyout_manager;
    DataStore &m_datastore;

    std::unique_ptr<QTimer> m_auto_update_timer;
    Items m_items;
};
