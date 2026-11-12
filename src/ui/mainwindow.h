// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QTimer>

#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <spdlog/spdlog.h>

#include "fetchsourcekey.h"
#include "filters/filterspec.h"
#include "item.h"
#include "itemlocation.h"
#include "locationinventory.h"
#include "refreshoutcome.h"
#include "util/programstate.h"

class QNetworkReply;
class QSettings;
class QVBoxLayout;

class BuyoutManager;
class BuyoutRepo;
class CharacterRepo;
class CurrencyDialog;
class CurrencyManager;
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
class StashRepo;
class UpdateChecker;

struct Buyout;
struct BuyoutChangeSet;

namespace Ui {
    class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QSettings &m_settings,
                        NetworkManager &network_manager,
                        RateLimiter &rate_limiter,
                        ItemsManager &items_mangaer,
                        BuyoutManager &buyout_manager,
                        BuyoutRepo &buyout_repo,
                        StashRepo &stash_repo,
                        CharacterRepo &character_repo,
                        CurrencyManager &currency_manager,
                        Shop &shop,
                        ImageCache &image_cache,
                        const QDir &app_data_dir);
    ~MainWindow();
    void LoadSettings();

signals:
    void UpdateCheckRequested();
    void SetSessionId(const QString &poesessid);
    void SetTheme(const QString &theme);
    void GetImage(const QString &url);
public slots:
    // Streamed-delta consumers (items-pipeline M3, D3/D4): the active
    // search applies each delta immediately and stays clean (R1-7) —
    // bucket-scoped row operations in By-Tab, the flat sorted merge in
    // By-Item (S5); background searches keep M2 D9 rule 1 verbatim.
    // Aggregate reconciliations apply as row removals scoped to the
    // parent's rows.
    void OnTabRefreshed(const ItemLocation &location, const Items &items);
    void OnChildrenReconciled(const ItemLocation &parent,
                              const std::vector<FetchSourceKey> &expected);
    // The typed terminal event (M2 D4): closes the selection-intent
    // window on every outcome, terminal failure included (R1-3/R2-1).
    void OnRefreshFinished(const RefreshOutcome &outcome);

    void OnCurrentItemChanged(const QModelIndex &current, const QModelIndex &previous);
    void OnSearchFormChange();
    void OnDelayedSearchFormChange();
    void OnTabChange(int index);
    void OnImageFetched(const QString &url);
    // The final snapshot (M2 D8 signal, M3 S6 semantics): background
    // searches are flagged items-dirty and refilter on their own next
    // activation (rule 1); the active search performs the R1-2
    // authoritative row reconciliation — row operations only, never a
    // reset. Initial population keeps its reset (D6: nothing to
    // preserve).
    void OnItemsRefreshed(bool initial_refresh = false);
    void OnStatusUpdate(ProgramState state, const QString &status);
    void OnNotifyUser(const QString &message);
    void OnShopWarning(const QString &message);
    void OnBuyoutChange();
    // The M3 S2 buyout batch response: one per outer batch boundary
    // (BuyoutManager::BuyoutsChanged).
    void OnBuyoutsChanged(const BuyoutChangeSet &changes);
    void ResizeTreeColumns();
    void ScheduleResizeTreeColumns();
    // The delta-path arming (S7 review round 1): a non-resetting
    // single-shot debounce, so a refresh burst pays one column resize
    // per interval instead of one per applied delta. Any immediate
    // resize supersedes a pending debounced one (ResizeTreeColumns
    // stops the timer).
    void ScheduleDeltaResizeTreeColumns();
    void OnExpandAll();
    void OnCollapseAll();
    void OnCheckAll();
    void OnUncheckAll();
    void OnCheckSelected() { CheckSelected(true); }
    void OnUncheckSelected() { CheckSelected(false); }
    void OnRenameTabClicked(int index);
    void OnDeleteTabClicked(int index);
    void OnRefreshSelected();
    void OnUpdateAvailable(bool available);
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

    // Buyouts menu actions
    void OnImportLegacyBuyouts();
    void OnImportLegacyBuyoutPlan();
    void OnLegacyBuyoutsImported();

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
    // S4 delta-application tail: dirty-flag adjudication (R1-7), view-side
    // default-expansion of an inserted bucket, caption refresh, and the
    // selection-intent pass. `model_changed` (any model operation, not
    // just item rows — S6 review round 1) gates the column resize, so a
    // metadata-only delta or a new empty bucket resizes too.
    // The view's expanded/collapsed signals (folder-tree D9): the
    // materialization hooks for bucket headers, and the user-collapsed
    // folder set of the current search.
    void OnTreeExpansionChanged(const QModelIndex &index, bool expanded);
    // The view-side tail for buckets a delta or the snapshot inserted:
    // folders by the user-collapsed set, tabs by the default-expanded rule.
    void ExpandInsertedBuckets(const std::vector<int> &bucket_rows);
    void FinishDeltaApplication(bool processed,
                                bool model_changed,
                                const std::vector<int> &inserted_bucket_rows);
    // R1-3: reconciles the view's selection with the stable-id intent
    // after row operations — a removed selected row lapses visually but
    // keeps the intent; a visible item with the intent's id re-adopts the
    // selection through the global identity index.
    void ReconcileSelectionIntent();
    // R1-3 as amended (#282): records the selected rows before row
    // operations run, and selects again afterwards every one of them that
    // is visible — one walk per bucket that holds an intended item.
    void CaptureSelectedRows();
    void ReselectCapturedRows();
    // Makes `index` the current, selected row without the view expanding
    // its collapsed ancestors: a reselection made for the user never undoes
    // the user's collapse (folder-tree D9).
    void SelectWithoutExpanding(const QModelIndex &index);

    void ModelViewRefresh();
    void ReselectCurrentItem();
    void ReselectCurrentBucket();
    void FlushPendingSearchFormChange();
    void SaveViewExpansion(Search &search);
    void RestoreViewExpansion(Search &search);
    // R6-3 scroll preservation: a top-row anchor (bucket stable key +
    // item stable id) with the raw scrollbar value as the fallback when
    // the anchored row no longer exists after the reset.
    void SaveViewScroll(Search &search);
    void RestoreViewScroll(Search &search);
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
    bool ConfirmLegacyBuyoutWrite();

    QSettings &m_settings;
    NetworkManager &m_network_manager;
    RateLimiter &m_rate_limiter;
    ItemsManager &m_items_manager;
    BuyoutManager &m_buyout_manager;
    BuyoutRepo &m_buyout_repo;
    StashRepo &m_stash_repo;
    CharacterRepo &m_character_repo;
    CurrencyManager &m_currency_manager;
    Shop &m_shop;
    ImageCache &m_image_cache;
    const QDir m_app_data_dir;

    // Application owns BuyoutManager and outlives MainWindow. Keep m_searches
    // declared after m_filter_catalog so reverse destruction destroys searches
    // before the catalog they reference.
    FilterCatalog m_filter_catalog;

    Ui::MainWindow *ui;
    CurrencyDialog *m_currency_dialog;

    std::shared_ptr<Item> m_current_item;
    std::optional<ItemLocation> m_current_bucket_location;
    // The selection intent (M3 R1-3): the selected item's stable id, held
    // independently of row existence. During an active refresh (first
    // delta to the terminal outcome) a removal keeps it alive and a later
    // insertion re-adopts it; every RefreshFinished outcome closes the
    // window (R2-1). A user selection overwrites it at any time.
    QString m_selection_intent_id;
    // The rest of the selection intent (R1-3 as amended, #282): the stable
    // ids of every selected item and the stable keys of every selected
    // header, captured before each row-operation pass of a refresh and
    // re-applied after it. An entry whose row is not visible stays until
    // the refresh ends; a user's selection of a new current row drops the
    // entries that are not visible.
    std::set<QString> m_selected_item_ids;
    std::set<LocationInventory::Key> m_selected_bucket_keys;
    // Set while the refresh selects a row on the user's behalf, so that
    // OnCurrentItemChanged can tell it from the user's own selection.
    bool m_reselecting{false};
    bool m_refresh_active{false};
    // Row operations make the view shuffle its current index (Qt moves
    // current off a removed row); while set, OnCurrentItemChanged ignores
    // those shifts so they cannot overwrite the intent.
    bool m_applying_delta{false};
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
    QTimer m_delayed_update_current_item;
    QTimer m_delayed_search_form_change;
    QTimer m_delayed_resize_columns;
    QTimer m_delta_resize_debounce;
    QMetaObject::Connection m_current_item_conn;
    RateLimitDialog *m_rate_limit_dialog;
    bool m_quitting;
};
