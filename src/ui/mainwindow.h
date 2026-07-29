// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QCloseEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

#include <spdlog/spdlog.h>

#include "filters/filterspec.h"
#include "item.h"
#include "itemlocation.h"
#include "util/programstate.h"

class QNetworkReply;
class QSettings;
class QVBoxLayout;

class BuyoutManager;
class CurrencyDialog;
class CurrencyManager;
class DataStore;
class ImageCache;
class Item;
class ItemLocation;
class ItemsManager;
class LogPanel;
class NetworkManager;
class RateLimiter;
class RateLimitDialog;
class Search;
class SearchForm;
class Shop;
class UpdateChecker;

struct Buyout;

namespace Ui {
    class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // SPIKE(S1-M2): D9 throttle prototype — injectable S, reset-latency
    // record, and the pending-tick state, exposed for the harness panel.
    struct SpikeResetRecord
    {
        qint64 total_ms = 0;   // capture -> restore complete
        qint64 filter_ms = 0;  // FilterItems (filter loop + model reset)
        qint64 restore_ms = 0; // total - filter: includes the whole-model
                               // re-sort (logged separately) plus expansion,
                               // reselect, scroll, and form/buyout bookkeeping
        int visible_items = 0;
        int buckets = 0;
    };
    void SpikeSetThrottleInterval(int ms);
    int SpikeThrottleInterval() const;
    bool SpikeThrottlePending() const;
    int SpikeThrottleRemainingMs() const;

    // SPIKE(S1-M2): the tracked selection, exposed so tests can assert
    // stable-identity reselection on the GGG item id and details-panel
    // adoption of the replacement object (not just display text).
    std::shared_ptr<Item> SpikeCurrentItem() const { return m_current_item; }

    explicit MainWindow(QSettings &m_settings,
                        NetworkManager &network_manager,
                        RateLimiter &rate_limiter,
                        DataStore &datastore,
                        ItemsManager &items_mangaer,
                        BuyoutManager &buyout_manager,
                        CurrencyManager &currency_manager,
                        Shop &shop,
                        ImageCache &image_cache);
    ~MainWindow();
    void LoadSettings();

signals:
    // SPIKE(S1-M2): one record per throttled reset, for the harness panel.
    void SpikeResetApplied(const MainWindow::SpikeResetRecord &record);

    void UpdateCheckRequested();
    void SetSessionId(const QString &poesessid);
    void SetTheme(const QString &theme);
    void GetImage(const QString &url);
public slots:
    // SPIKE(S1-M2): consumer of the prototype streamed delta (D9 rules 1-2).
    void OnTabRefreshed(const ItemLocation &location, const Items &delta_items);

    void OnCurrentItemChanged(const QModelIndex &current, const QModelIndex &previous);
    void OnSearchFormChange();
    void OnDelayedSearchFormChange();
    void OnTabChange(int index);
    void OnImageFetched(const QString &url);
    void OnItemsRefreshed();
    void OnStatusUpdate(ProgramState state, const QString &status);
    void OnNotifyUser(const QString &message);
    void OnShopWarning(const QString &message);
    void OnBuyoutChange();
    void ResizeTreeColumns();
    void ScheduleResizeTreeColumns();
    void OnExpandAll();
    void OnCollapseAll();
    void OnCheckAll();
    void OnUncheckAll();
    void OnCheckSelected() { CheckSelected(true); }
    void OnUncheckSelected() { CheckSelected(false); }
    void OnRenameTabClicked(int index);
    void OnDeleteTabClicked(int index);
    void OnRefreshSelected();
    void OnUpdateAvailable();
    void OnUploadFinished();

private slots:
    // Tabs menu actions
    void OnFetchTabsList();
    void OnRefreshCheckedTabs();
    void OnRefreshAllTabs();
    void OnSetAutomaticTabRefresh();
    void OnSetTabRefreshInterval();

    // Shop menu actions
    void OnSetShopThreads();
    void OnEditShopTemplate();
    void OnCopyShopToClipboard();
    void OnUpdateShops();
    void OnSetAutomaticShopUpdate();
    void OnShowPOESESSID();

    // Currency menu actions
    void OnListCurrency();
    void OnExportCurrency();

    // Theme submenu actions
    void OnSetDarkTheme(bool toggle);
    void OnSetLightTheme(bool toggle);
    void OnSetDefaultTheme(bool toggle);

    // Logging submenu actions
    void OnSetLogging(spdlog::level::level_enum level);

    // Tooltip buttons
    void OnCopyForPOB();
    void OnUploadToImgur();

private:
    // SPIKE(S1-M2)
    void SpikeThrottleFired();
    bool SpikeDeltaIntersectsCurrent(const ItemLocation &location, const Items &delta_items) const;
    struct SpikeScrollAnchor
    {
        QString bucket_key;
        QString item_id;
        int scrollbar_value = 0;
    };
    SpikeScrollAnchor SpikeCaptureScroll() const;
    void SpikeRestoreScroll(const SpikeScrollAnchor &anchor);

    void ModelViewRefresh();
    void ReselectCurrentItem();
    void ReselectCurrentBucket();
    void FlushPendingSearchFormChange();
    void SaveViewExpansion(Search &search);
    void RestoreViewExpansion(Search &search);
    void ClearCurrentItem();
    void UpdateCurrentBucket();
    void UpdateCurrentItem();
    void UpdateCurrentBuyout();
    void ResetBuyoutWidgets();
    void NewSearch();
    void InitializeRateLimitDialog();
    void InitializeLogging();
    void InitializeSearchForm();
    void InitializeUi();
    bool eventFilter(QObject *o, QEvent *e);
    void UpdateShopMenu();
    void UpdateBuyoutWidgets(const Buyout &bo);
    void closeEvent(QCloseEvent *event);
    void CheckSelected(bool value);

    QSettings &m_settings;
    NetworkManager &m_network_manager;
    RateLimiter &m_rate_limiter;
    DataStore &m_datastore;
    ItemsManager &m_items_manager;
    BuyoutManager &m_buyout_manager;
    CurrencyManager &m_currency_manager;
    Shop &m_shop;
    ImageCache &m_image_cache;

    // Application owns BuyoutManager and outlives MainWindow. Keep m_searches
    // declared after m_filter_catalog so reverse destruction destroys searches
    // before the catalog they reference.
    FilterCatalog m_filter_catalog;

    Ui::MainWindow *ui;
    CurrencyDialog *m_currency_dialog;

    std::shared_ptr<Item> m_current_item;
    std::optional<ItemLocation> m_current_bucket_location;
    std::vector<std::unique_ptr<Search>> m_searches;
    Search *m_current_search;
    QTabBar *m_tab_bar;
    LogPanel *m_log_panel;
    std::unique_ptr<SearchForm> m_search_form;
    int m_search_count;

    QLabel *m_status_bar_label;
    QVBoxLayout *m_search_form_layout;
    QMenu m_context_menu;
    QPushButton m_update_button;
    QPushButton m_refresh_button;
    QTimer m_delayed_update_current_item;
    QTimer m_delayed_search_form_change;
    QTimer m_delayed_resize_columns;
    QMetaObject::Connection m_current_item_conn;
    RateLimitDialog *m_rate_limit_dialog;
    bool m_quitting;

    // SPIKE(S1-M2): the current search's pending tick — a non-resetting
    // trailing throttle with period S (D9 rule 2).
    QTimer m_spike_throttle;
    qint64 m_spike_expansion_ms = 0;
    qint64 m_spike_sort_ms = 0;
};
