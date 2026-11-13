// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QCloseEvent>
#include <QDir>
#include <QItemSelectionModel>
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
#include "sessionidentity.h"
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
struct PriceTargets;
struct BuyoutChangeSet;

namespace Ui {
    class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QSettings &m_settings,
                        const SessionIdentity &identity,
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
    // Applies a price the editor committed to the current selection.
    // Applies a price the editor committed to the rows it was typed for.
    void OnBuyoutCommitted(const PriceTargets &targets, const Buyout &bo);
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
    // One delta's view of the selection (M3 R1-3 as amended, #282). The
    // current item follows R1-3's intent (below). Every other selected row
    // stays selected across a refresh while its item stays in its tab; an
    // item that leaves its tab loses its selection. Nothing is kept for
    // those rows from one delta to the next.
    struct SelectionCapture
    {
        // The bucket the delta touches, walked to restore the selection;
        // nullopt (the final snapshot) for every bucket.
        std::optional<LocationInventory::Key> bucket;
        // Each selected item in that bucket, by stable id and the tab it
        // came from.
        std::set<std::pair<QString, LocationInventory::Key>> items;
        // Whether the current row was selected.
        bool current_selected{false};
    };
    SelectionCapture CaptureSelection(std::optional<LocationInventory::Key> bucket) const;
    // The bucket a delta for `location` touches: its tab's in By-Tab, the
    // flat bucket in By-Item.
    LocationInventory::Key TouchedBucket(const ItemLocation &location) const;
    // Selects again, by one walk of the captured bucket, the captured
    // items it still holds from the same tab.
    void RestoreSelection(const SelectionCapture &capture);
    void FinishDeltaApplication(bool processed,
                                bool model_changed,
                                const std::vector<int> &inserted_bucket_rows,
                                const SelectionCapture &capture);
    // R1-3: reconciles the current row with the stable-id intent after
    // row operations — a removed current row lapses but keeps the intent;
    // a visible item with the intent's id re-adopts the current row
    // through the global identity index. It never clears or replaces the
    // rest of the selection, and selects the row only if it was selected
    // (as `capture` records for a delta) (#282).
    void ReconcileSelectionIntent(const SelectionCapture *capture = nullptr);
    // Settles the current row (ReconcileSelectionIntent) and, after a
    // delta or the final snapshot, the selection (RestoreSelection), then
    // shows the price editor the result once. The only caller of both.
    void SettleSelection(const SelectionCapture *capture);
    // Records whether the current row is selected, while it is present.
    void NoteCurrentRowSelection();
    // Makes `index` the current row, applying `flags` to the selection,
    // without the view expanding its collapsed ancestors: a reselection
    // made for the user never undoes the user's collapse (folder-tree D9).
    void SelectWithoutExpanding(const QModelIndex &index, QItemSelectionModel::SelectionFlags flags);

    // `capture`, taken before the refilter, is the selection it restores
    // (the rows still shown stay selected); without one, only the current
    // row is selected again.
    void ModelViewRefresh(const SelectionCapture *capture = nullptr);
    // The active search re-filters after a price change marked it (#268),
    // once, after the command or pass that changed the price has returned.
    void RefilterAfterPriceChange();
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
    void OnSelectionChanged();
    // The selected rows that can take a price.
    PriceTargets SelectedPriceTargets() const;
    void ResetBuyoutWidgets();
    void NewSearch();
    void InitializeRateLimitDialog();
    void InitializeLogging();
    void InitializeSearchForm();
    void InitializeUi();
    bool eventFilter(QObject *o, QEvent *e);
    void UpdateShopMenu();
    void closeEvent(QCloseEvent *event);
    void CheckSelected(bool value);
    bool ConfirmLegacyBuyoutWrite();

    QSettings &m_settings;
    // Who the session runs as, decided at login (#299).
    const SessionIdentity m_identity;
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
    // Whether the intent's row was selected when it last left: its
    // re-adoption selects it again only then, and never touches the rest
    // of the selection (the R1-3 amendment, #282). Kept current by
    // NoteCurrentRowSelection before every pass that can remove the row.
    bool m_selection_intent_selected{true};
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
    QMetaObject::Connection m_selection_conn;
    QMetaObject::Connection m_model_reset_conn;
    // Set while SettleSelection settles the current row and the selection;
    // the price editor is updated once at its end.
    bool m_defer_price_editor{false};
    bool m_price_refilter_queued{false};
    RateLimitDialog *m_rate_limit_dialog;
    bool m_quitting;
};
