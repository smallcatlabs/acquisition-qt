// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/mainwindow.h"
#include "ui_mainwindow.h"

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QImageReader>
#include <QInputDialog>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QVersionNumber>

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "buyoutmanager.h"
#include "currencymanager.h"
#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/stashrepo.h"
#include "imagecache.h"
#include "item.h"
#include "itemconstants.h"
#include "itemlocation.h"
#include "items_model.h"
#include "itemsmanager.h"
#include "legacy/legacybuyoutimporter.h"
#include "modelprobes.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitdialog.h"
#include "ratelimit/ratelimiter.h"
#include "replytimeout.h"
#include "search.h"
#include "shop.h"
#include "ui/currencydialog.h"
#include "ui/itemtooltip.h"
#include "ui/logpanel.h"
#include "ui/mainwindowhelpers.h"
#include "ui/searchform.h"
#include "ui/verticalscrollarea.h"
#include "util/networkmanager.h"
#include "util/spdlog_qt.h"
#include "util/updatechecker.h"
#include "util/util.h"
#include "version_defines.h"

constexpr const char *POE_WEBCDN
    = "http://webcdn.pathofexile.com"; // Should be updated to https://web.poecdn.com ?

constexpr int CURRENT_ITEM_UPDATE_DELAY_MS = 100;
constexpr int SEARCH_UPDATE_DELAY_MS = 350;
// The delta-path column-resize debounce (S7 review round 1): each
// ResizeTreeColumns pass costs ~10 ms regardless of scale, so a
// refresh burst pays at most one per interval instead of one per
// applied delta. Non-resetting, so a sustained burst still resizes
// this often (bounded staleness AND bounded work).
constexpr int DELTA_RESIZE_DEBOUNCE_MS = 250;
// How long a "Price not saved" notice covers the status line (#257).
constexpr int PRICE_NOT_SAVED_TIMEOUT_MS = 10000;

namespace {

    QString legacyBuyoutAuditPath(const QDir &data_dir)
    {
        const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd'T'HHmmss'Z'");
        const QString stem = "buyout-import-" + timestamp;
        QString path = data_dir.filePath(stem + ".xlsx");
        int suffix = 2;
        while (QFileInfo::exists(path)) {
            path = data_dir.filePath(QString("%1-%2.xlsx").arg(stem).arg(suffix++));
        }
        return path;
    }

    bool copyPlanFile(const QString &source_path, const QString &destination_path, QString &error)
    {
        QFile source(source_path);
        if (!source.open(QIODevice::ReadOnly)) {
            error = source.errorString();
            return false;
        }
        const QByteArray contents = source.readAll();
        if (source.error() != QFileDevice::NoError) {
            error = source.errorString();
            return false;
        }
        source.close();

        QSaveFile destination(destination_path);
        if (!destination.open(QIODevice::WriteOnly)) {
            error = destination.errorString();
            return false;
        }
        if (destination.write(contents) != contents.size() || !destination.commit()) {
            error = destination.errorString();
            return false;
        }
        return true;
    }

    bool showActionPrompt(QWidget *parent,
                          QMessageBox::Icon icon,
                          const QString &title,
                          const QString &text,
                          const QString &informative_text,
                          const QString &accept_text)
    {
        QMessageBox dialog(parent);
        dialog.setWindowTitle(title);
        dialog.setIcon(icon);
        dialog.setText(text);
        dialog.setInformativeText(informative_text);
        QPushButton *const accept = dialog.addButton(accept_text, QMessageBox::AcceptRole);
        accept->setMinimumWidth(accept->sizeHint().width());
        QPushButton *const cancel = dialog.addButton(QMessageBox::Cancel);
        dialog.setDefaultButton(cancel);
        dialog.exec();
        return dialog.clickedButton() == accept;
    }

    // An import writes buyouts, which nothing may do while they are not
    // loaded (#257). Returns true, after telling the user, when it must not
    // start.
    bool refuseImportWithoutBuyouts(QWidget *parent, const BuyoutManager &buyout_manager)
    {
        if (buyout_manager.BuyoutsLoaded()) {
            return false;
        }
        QMessageBox::warning(parent,
                             QObject::tr("Legacy buyout import"),
                             QObject::tr("Acquisition cannot save prices in this session, so "
                                         "it cannot import buyouts.\n\nThe log file says why."),
                             QMessageBox::Ok);
        return true;
    }

} // namespace

MainWindow::MainWindow(QSettings &settings,
                       NetworkManager &network_manager,
                       RateLimiter &rate_limiter,
                       ItemsManager &items_manager,
                       BuyoutManager &buyout_manager,
                       BuyoutRepo &buyout_repo,
                       StashRepo &stash_repo,
                       CharacterRepo &character_repo,
                       CurrencyManager &currency_manager,
                       Shop &shop,
                       ImageCache &image_cache,
                       const QDir &app_data_dir)
    : m_settings(settings)
    , m_network_manager(network_manager)
    , m_rate_limiter(rate_limiter)
    , m_items_manager(items_manager)
    , m_buyout_manager(buyout_manager)
    , m_buyout_repo(buyout_repo)
    , m_stash_repo(stash_repo)
    , m_character_repo(character_repo)
    , m_currency_manager(currency_manager)
    , m_shop(shop)
    , m_image_cache(image_cache)
    , m_app_data_dir(app_data_dir)
    , m_filter_catalog(BuildFilterCatalog(buyout_manager))
    , ui(new Ui::MainWindow)
    , m_currency_dialog(nullptr)
    , m_current_search(nullptr)
    , m_log_panel(nullptr)
    , m_search_count(0)
    , m_rate_limit_dialog(nullptr)
    , m_quitting(false)
{
    connect(qApp, &QCoreApplication::aboutToQuit, this, [&]() { m_quitting = true; });

    InitializeUi();
    InitializeRateLimitDialog();
    InitializeLogging();
    InitializeSearchForm();

    const QString title = QString("Acquisition [%1] - %2 League [%3]")
                              .arg(QString(APP_VERSION_STRING),
                                   m_settings.value("league").toString(),
                                   m_settings.value("account").toString());
    setWindowTitle(title);
    setWindowIcon(QIcon(":/icons/icon.svg"));

    m_delayed_update_current_item.setInterval(CURRENT_ITEM_UPDATE_DELAY_MS);
    m_delayed_update_current_item.setSingleShot(true);
    connect(&m_delayed_update_current_item, &QTimer::timeout, this, &MainWindow::UpdateCurrentItem);

    m_delayed_search_form_change.setInterval(SEARCH_UPDATE_DELAY_MS);
    m_delayed_search_form_change.setSingleShot(true);
    connect(&m_delayed_search_form_change, &QTimer::timeout, this, &MainWindow::OnSearchFormChange);

    m_delayed_resize_columns.setInterval(0);
    m_delayed_resize_columns.setSingleShot(true);
    connect(&m_delayed_resize_columns, &QTimer::timeout, this, &MainWindow::ResizeTreeColumns);

    m_delta_resize_debounce.setInterval(DELTA_RESIZE_DEBOUNCE_MS);
    m_delta_resize_debounce.setSingleShot(true);
    connect(&m_delta_resize_debounce, &QTimer::timeout, this, &MainWindow::ResizeTreeColumns);

    // The M3 buyout batch response (S2): one model update per outer batch
    // boundary, delivered synchronously at the emitting mutation's end.
    connect(&m_buyout_manager, &BuyoutManager::BuyoutsChanged, this, &MainWindow::OnBuyoutsChanged);

    LoadSettings();
    NewSearch();

    // A session that starts without its prices says so once (#257). Posted,
    // so the box opens over the shown window rather than before it.
    if (!m_buyout_manager.BuyoutsLoaded()) {
        QTimer::singleShot(0, this, [this] {
            auto *box = new QMessageBox(QMessageBox::Warning,
                                        tr("Acquisition"),
                                        tr("Acquisition cannot save prices in this session."),
                                        QMessageBox::Ok,
                                        this);
            box->setInformativeText(
                tr("Its price database could not be fully used; the log file says why. Prices "
                   "may still be shown, but changes are not saved and the forum shop will not "
                   "be updated.\n\nThe log file is:\n%1")
                    .arg(QDir::toNativeSeparators(m_app_data_dir.filePath("log.txt"))));
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        });
    }
}

MainWindow::~MainWindow()
{
    // Detach the log panel's sinks while its widgets are still alive: child
    // cleanup destroys the panel's QTextEdit before ~LogPanel would run, and
    // a worker thread logging in that window would write to a dead widget
    // (F42).
    if (m_log_panel) {
        m_log_panel->DetachSinks();
    }
    m_search_form.reset();
    delete ui;
    m_rate_limit_dialog->close();
    m_rate_limit_dialog->deleteLater();
}

void MainWindow::InitializeRateLimitDialog()
{
    m_rate_limit_dialog = new RateLimitDialog(this, &m_rate_limiter);
    auto *const button = new QPushButton(this);
    button->setFlat(false);
    button->setText("Rate Limit Status");
    connect(button, &QPushButton::clicked, m_rate_limit_dialog, &RateLimitDialog::show);
    // The limiter reports 0 once when the last pause ends (#284).
    connect(&m_rate_limiter, &RateLimiter::Paused, this, [=](int pause) {
        if (pause > 0) {
            button->setText("Rate limited for " + QString::number(pause) + " seconds");
            button->setStyleSheet("font-weight: bold; color: red");
        } else {
            button->setText("Rate Limit Status");
            button->setStyleSheet("");
        }
    });
    statusBar()->addPermanentWidget(button);
}

void MainWindow::InitializeLogging()
{
    m_log_panel = new LogPanel(this, ui);
#if defined(_DEBUG)
    // display warnings here so it's more visible
    spdlog::warn("Maintainer: This is a debug build");
#endif
}

void MainWindow::InitializeUi()
{
    ui->setupUi(this);

    m_status_bar_label = new QLabel("Ready");
    statusBar()->addWidget(m_status_bar_label);
    ui->itemLayout->setAlignment(Qt::AlignTop);
    ui->itemLayout->setAlignment(ui->minimapLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->nameLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->imageLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->locationLabel, Qt::AlignHCenter);

    m_tab_bar = new QTabBar;
    m_tab_bar->installEventFilter(this);
    m_tab_bar->setExpanding(false);
    m_tab_bar->addTab("+");
    m_tab_bar->setSelectionBehaviorOnRemove(QTabBar::SelectLeftTab);
    connect(m_tab_bar, &QTabBar::currentChanged, this, &MainWindow::OnTabChange);
    ui->mainLayout->insertWidget(0, m_tab_bar);

    Util::PopulateBuyoutTypeComboBox(ui->buyoutTypeComboBox);
    Util::PopulateBuyoutCurrencyComboBox(ui->buyoutCurrencyComboBox);

    connect(ui->buyoutCurrencyComboBox, &QComboBox::activated, this, &MainWindow::OnBuyoutChange);
    connect(ui->buyoutTypeComboBox, &QComboBox::activated, this, &MainWindow::OnBuyoutChange);
    connect(ui->buyoutValueLineEdit, &QLineEdit::textEdited, this, &MainWindow::OnBuyoutChange);

    ui->viewComboBox->addItems({"By Tab", "By Item"});
    connect(ui->viewComboBox, &QComboBox::activated, this, [&](int n) {
        // activated() also fires when the user re-selects the current mode;
        // save/restore must not run then (restore would force-expand rows the
        // user collapsed under a filtered or By Item view).
        const auto mode = static_cast<Search::ViewMode>(n);
        if (mode != m_current_search->GetViewMode()) {
            SaveViewExpansion(*m_current_search);
            m_current_search->SetViewMode(mode);
            if (m_current_search->itemsDirty()) {
                // R1-7 fail-safe at a D6 boundary: a search left
                // items-dirty (application was skipped) refilters NOW so
                // the arriving mode never renders un-applied state.
                // Unreachable in normal operation since S5 — the delta
                // path applies immediately in both view modes.
                m_current_search->SetRefreshReason(RefreshReason::ItemsChanged);
                ModelViewRefresh();
            } else {
                RestoreViewExpansion(*m_current_search);
            }
        }
        // Restoring expansion schedules a resize via the expanded/collapsed
        // signals. Also schedule one here because column contents change
        // between modes even when the expansion state does not.
        ScheduleResizeTreeColumns();
    });

    ui->buyoutTypeComboBox->setEnabled(false);
    ui->buyoutValueLineEdit->setEnabled(false);
    ui->buyoutCurrencyComboBox->setEnabled(false);

    m_search_form_layout = new QVBoxLayout;
    m_search_form_layout->setAlignment(Qt::AlignTop);
    m_search_form_layout->setContentsMargins(0, 0, 0, 0);

    auto search_form_container = new QWidget;
    search_form_container->setLayout(m_search_form_layout);

    auto scroll_area = new VerticalScrollArea;
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setWidgetResizable(true);
    scroll_area->setWidget(search_form_container);
    scroll_area->setMinimumWidth(150); // TODO(xyz): remove magic numbers
    scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    ui->scrollArea->setFrameShape(QFrame::NoFrame);
    ui->scrollArea->setWidgetResizable(true);

    ui->horizontalLayout_2->insertWidget(0, scroll_area);
    search_form_container->show();

    ui->horizontalLayout_2->setStretchFactor(0, 2);
    ui->horizontalLayout_2->setStretchFactor(1, 5);
    ui->horizontalLayout_2->setStretchFactor(2, 0);

    ui->treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui->treeView->setSortingEnabled(true);

    m_context_menu.addAction("Refresh Selected", this, &MainWindow::OnRefreshSelected);
    m_context_menu.addAction("Check Selected", this, &MainWindow::OnCheckSelected);
    m_context_menu.addAction("Uncheck Selected", this, &MainWindow::OnUncheckSelected);
    m_context_menu.addSeparator();
    m_context_menu.addAction("Check All", this, &MainWindow::OnCheckAll);
    m_context_menu.addAction("Uncheck All", this, &MainWindow::OnUncheckAll);
    m_context_menu.addSeparator();
    m_context_menu.addAction("Expand All", this, &MainWindow::OnExpandAll);
    m_context_menu.addAction("Collapse All", this, &MainWindow::OnCollapseAll);

    connect(ui->treeView, &QTreeView::customContextMenuRequested, this, [&](const QPoint &pos) {
        m_context_menu.popup(ui->treeView->viewport()->mapToGlobal(pos));
    });

    m_update_button.setText("Update available");
    m_update_button.setStyleSheet("color: blue; font-weight: bold;");
    m_update_button.setFlat(true);
    m_update_button.hide();
    statusBar()->addPermanentWidget(&m_update_button);
    connect(&m_update_button, &QPushButton::clicked, this, [=, this]() {
        emit UpdateCheckRequested();
    });

    // The D2 materialization hooks (M3 S3): expanding a bucket sorts it
    // first if its flag is invalid (keys built then resident — D1);
    // collapsing evicts its keys while order and flag persist. Programmatic
    // expansion (RestoreViewExpansion, expandToDepth) emits the same
    // signals, so restored expansions sort exactly the restored buckets.
    //
    // The same signals maintain the search's set of user-collapsed folders
    // (folder-tree D9): programmatic expansion follows the set, so only a
    // user's collapse or expand changes it.
    connect(ui->treeView, &QTreeView::expanded, this, [this](const QModelIndex &index) {
        OnTreeExpansionChanged(index, true);
    });
    connect(ui->treeView, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
        OnTreeExpansionChanged(index, false);
    });

    // Resize columns when a tab is expanded/collapsed. Programmatic expansion
    // (e.g. RestoreViewExpansion after a model reset) emits these signals once
    // per index, so coalesce them into a single deferred resize.
    connect(ui->treeView, &QTreeView::collapsed, this, &MainWindow::ScheduleResizeTreeColumns);
    connect(ui->treeView, &QTreeView::expanded, this, &MainWindow::ScheduleResizeTreeColumns);

    ui->propertiesLabel->setStyleSheet(
        "QLabel { background-color: black; color: #7f7f7f; padding: 10px; font-size: 17px; }");
    ui->propertiesLabel->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameFirstLine->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameSecondLine->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameFirstLine->setAlignment(Qt::AlignCenter);
    ui->itemNameSecondLine->setAlignment(Qt::AlignCenter);

    ui->itemTextTooltip->setStyleSheet(
        "QLabel { background-color: black; color: #7f7f7f; padding: 3px; }");

    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    // Make sure the right logging level menu item is checked.
    OnSetLogging(spdlog::get_level());

    connect(ui->itemInfoTypeTabs, &QTabWidget::currentChanged, this, [=, this](int idx) {
        auto tabs = ui->itemInfoTypeTabs;
        for (int i = 0; i < tabs->count(); i++) {
            if (i != idx) {
                tabs->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            }
        }
        auto widget = tabs->widget(idx);
        widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        widget->resize(tabs->widget(idx)->minimumSizeHint());
        widget->adjustSize();
        m_settings.setValue("tooltip_tab", idx);
    });

    // Connect the Tabs menu.
    connect(ui->actionFetchTabsList, &QAction::triggered, this, &MainWindow::OnFetchTabsList);
    connect(ui->actionRefreshCheckedTabs,
            &QAction::triggered,
            this,
            &MainWindow::OnRefreshCheckedTabs);
    connect(ui->actionRefreshAllTabs, &QAction::triggered, this, &MainWindow::OnRefreshAllTabs);
    connect(ui->actionSetAutomaticTabRefresh,
            &QAction::triggered,
            this,
            &MainWindow::OnSetAutomaticTabRefresh);
    connect(ui->actionSetTabRefreshInterval,
            &QAction::triggered,
            this,
            &MainWindow::OnSetTabRefreshInterval);

    connect(ui->actionGetMapStashes, &QAction::triggered, this, [this](bool checked) {
        m_settings.setValue("get_map_stashes", checked);
    });
    connect(ui->actionGetUniqueStashes, &QAction::triggered, this, [this](bool checked) {
        m_settings.setValue("get_unique_stashes", checked);
    });

    // Connect the Shop menu.
    connect(ui->actionSetShopThreads, &QAction::triggered, this, &MainWindow::OnSetShopThreads);
    connect(ui->actionEditShopTemplate, &QAction::triggered, this, &MainWindow::OnEditShopTemplate);
    connect(ui->actionCopyShopToClipboard,
            &QAction::triggered,
            this,
            &MainWindow::OnCopyShopToClipboard);
    connect(ui->actionUpdateShopPOESESSID, &QAction::triggered, this, &MainWindow::OnShowPOESESSID);
    connect(ui->actionUpdateShops, &QAction::triggered, this, &MainWindow::OnUpdateShops);
    connect(ui->actionSetAutomaticallyShopUpdate,
            &QAction::triggered,
            this,
            &MainWindow::OnSetAutomaticShopUpdate);
    // The shop can turn automatic updates off itself (a rejected
    // POESESSID); the menu item follows the setting, not the last click.
    connect(&m_shop,
            &Shop::AutoUpdateChanged,
            ui->actionSetAutomaticallyShopUpdate,
            &QAction::setChecked);

    // Connect the Theme submenu. The group keeps exactly one theme checked:
    // clicking the active theme leaves it checked.
    auto *const theme_group = new QActionGroup(this);
    theme_group->setExclusionPolicy(QActionGroup::ExclusionPolicy::Exclusive);
    theme_group->addAction(ui->actionSetDarkTheme);
    theme_group->addAction(ui->actionSetLightTheme);
    theme_group->addAction(ui->actionSetDefaultTheme);
    connect(ui->actionSetDarkTheme, &QAction::triggered, this, &MainWindow::OnSetDarkTheme);
    connect(ui->actionSetLightTheme, &QAction::triggered, this, &MainWindow::OnSetLightTheme);
    connect(ui->actionSetDefaultTheme, &QAction::triggered, this, &MainWindow::OnSetDefaultTheme);

    // Connect the Logging submenu.
    connect(ui->actionLoggingOFF, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::off);
    });
    connect(ui->actionLoggingFATAL, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::critical);
    });
    connect(ui->actionLoggingERROR, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::err);
    });
    connect(ui->actionLoggingWARN, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::warn);
    });
    connect(ui->actionLoggingINFO, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::info);
    });
    connect(ui->actionLoggingDEBUG, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::debug);
    });
    connect(ui->actionLoggingTRACE, &QAction::triggered, this, [=, this]() {
        OnSetLogging(spdlog::level::trace);
    });

    // Connect the POESESSID submenu.
    connect(ui->actionShowPOESESSID, &QAction::triggered, this, &MainWindow::OnShowPOESESSID);

    // Help menu
    connect(ui->actionOpenUserGuide, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(
            QUrl("https://github.com/smallcatlabs/acquisition-qt/blob/master/docs/user/README.md"));
    });
    connect(ui->actionReportIssue, &QAction::triggered, this, [] {
        QDesktopServices::openUrl(QUrl("https://github.com/smallcatlabs/acquisition-qt/issues"));
    });

    // Connect the Tooltip tab buttons
    connect(ui->uploadTooltipButton, &QPushButton::clicked, this, &MainWindow::OnUploadToImgur);
    connect(ui->pobTooltipButton, &QPushButton::clicked, this, &MainWindow::OnCopyForPOB);

    // Connect the currency actions.
    connect(ui->actionListCurrency, &QAction::triggered, this, &MainWindow::OnListCurrency);
    connect(ui->actionExportCurrency, &QAction::triggered, this, &MainWindow::OnExportCurrency);

    // Connect the Buyouts menu.
    connect(ui->actionImportLegacyBuyouts,
            &QAction::triggered,
            this,
            &MainWindow::OnImportLegacyBuyouts);
    connect(ui->actionImportLegacyBuyoutPlan,
            &QAction::triggered,
            this,
            &MainWindow::OnImportLegacyBuyoutPlan);
}

void MainWindow::LoadSettings()
{
    // Make sure the theme button is checked.
    const QString theme = m_settings.value("theme", "default").toString().toLower();
    ui->actionSetDarkTheme->setChecked(theme == "dark");
    ui->actionSetLightTheme->setChecked(theme == "light");
    ui->actionSetDefaultTheme->setChecked(theme == "default");
    ui->actionGetMapStashes->setChecked(m_settings.value("get_map_stashes", false).toBool());
    ui->actionGetUniqueStashes->setChecked(m_settings.value("get_unique_stashes", false).toBool());
    ui->actionSetAutomaticTabRefresh->setChecked(m_settings.value("autoupdate").toBool());
    UpdateShopMenu();

    ui->itemInfoTypeTabs->setCurrentIndex(m_settings.value("tooltip_tab").toInt());
}

void MainWindow::OnTreeExpansionChanged(const QModelIndex &index, bool expanded)
{
    auto *model = qobject_cast<ItemsModel *>(ui->treeView->model());
    if (!model) {
        return;
    }
    const ItemsModel::RowRef ref = model->Locate(index);
    if (!ref.isBucket()) {
        return;
    }
    if (m_current_search && (model == &m_current_search->model())
        && m_current_search->isFolderBucket(ref.bucket_row)) {
        m_current_search->setFolderCollapsed(LocationInventory::KeyFor(
                                                 m_current_search->bucket(ref.bucket_row).location()),
                                             !expanded);
    }
    if (expanded) {
        model->OnBucketExpanded(ref.bucket_row);
    } else {
        model->OnBucketCollapsed(ref.bucket_row);
    }
}

void MainWindow::ExpandInsertedBuckets(const std::vector<int> &bucket_rows)
{
    // A folder inserted into any search expands unless its user collapsed
    // it (folder-tree D9); a tab bucket inserted into a default-expanded
    // search expands now (today's rule) — the expand signal materializes
    // and sorts it (D2 rule 2).
    const ItemsModel &model = m_current_search->model();
    for (const int row : bucket_rows) {
        if (m_current_search->isFolderBucket(row)) {
            if (!m_current_search->folderCollapsed(
                    LocationInventory::KeyFor(m_current_search->bucket(row).location()))) {
                ui->treeView->expand(model.BucketIndex(row));
            }
        } else if (m_current_search->defaultExpanded()) {
            ui->treeView->expand(model.BucketIndex(row));
        }
    }
}

void MainWindow::OnExpandAll()
{
    spdlog::trace("MainWindow::OnExpandAll() entered");
    // Every bucket header — folders, top-level and nested tabs, characters
    // (folder-tree D9) — and never an item row: expandToDepth(1) would
    // store expansion state for the items of every top-level tab.
    //
    // One pass for the top level, as before folders: expandToDepth(0) lays
    // out every top-level header at once, where expanding them one at a
    // time measured about three times slower on a 2,000-tab account. The
    // nested tab headers follow one by one. expandToDepth() first drops
    // every deeper expansion, so a nested tab that was already expanded is
    // collapsed and expanded again here; its order and sorted flag persist
    // through that (M3 D2 rule 3), so nothing re-sorts.
    //
    // The expanded() signals emitted during this operation coalesce into a
    // single deferred column resize. Folders leave the user-collapsed set.
    setCursor(Qt::WaitCursor);
    ItemsModel &model = m_current_search->model();
    const int bucket_count = static_cast<int>(m_current_search->buckets().size());
    if (m_current_search->GetViewMode() == Search::ViewMode::ByTab) {
        // Every tab is about to be materialized, so mark them all and
        // establish their order in one pass first (as FilterItems does for
        // a default-expanded search): the expansions below then find valid
        // flags and sort nothing. Left to sort-on-expand, each nested tab's
        // sort would leave the view a pending relayout that the next
        // visible expansion performs — a full relayout per nested tab.
        bool unsorted = false;
        for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
            if (!m_current_search->isFolderBucket(bucket_row)) {
                m_current_search->MarkBucketExpanded(bucket_row);
                unsorted = unsorted || !m_current_search->bucket(bucket_row).sorted();
            }
        }
        if (unsorted) {
            model.Resort();
        }
    }
    ui->treeView->expandToDepth(0);
    for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
        if (m_current_search->isFolderBucket(bucket_row)) {
            m_current_search->setFolderCollapsed(LocationInventory::KeyFor(
                                                     m_current_search->bucket(bucket_row).location()),
                                                 false);
        } else if (m_current_search->parentBucketRow(bucket_row) >= 0) {
            ui->treeView->expand(model.BucketIndex(bucket_row));
        }
    }
    ScheduleResizeTreeColumns();
    unsetCursor();
}

void MainWindow::OnCollapseAll()
{
    spdlog::trace("MainWindow::OnCollapseAll() entered");
    // There is no depth-based collapse method, so manuall looping
    // over rows can be much faster than collapseAll() under some
    // conditions, possibly beecause those funcitons check every
    // element in the tree, which in our case will include all items.
    //
    // The collapsed() signals emitted during the loop coalesce into a
    // single deferred column resize.
    //
    // Every bucket header, folders included (folder-tree D9), in reverse
    // tree order so a nested tab is collapsed while its folder still shows
    // it. collapse() emits nothing for a header it cannot see, so the
    // materialization mark and the user-collapsed folder set are synced
    // explicitly (both idempotent).
    setCursor(Qt::WaitCursor);
    ItemsModel &model = m_current_search->model();
    const int bucket_count = static_cast<int>(m_current_search->buckets().size());
    for (int bucket_row = bucket_count - 1; bucket_row >= 0; --bucket_row) {
        ui->treeView->collapse(model.BucketIndex(bucket_row));
        if (m_current_search->isFolderBucket(bucket_row)) {
            m_current_search->setFolderCollapsed(LocationInventory::KeyFor(
                                                     m_current_search->bucket(bucket_row).location()),
                                                 true);
        } else {
            model.OnBucketCollapsed(bucket_row);
        }
    }
    ScheduleResizeTreeColumns();
    unsetCursor();
}

void MainWindow::OnCheckAll()
{
    spdlog::trace("MainWindow::OnCheckAll() entered");
    // Every bucket shown except folders: a folder id is never written
    // (D10). The store refuses the By-Item view's header row and
    // refresh-locked tabs.
    for (auto const &bucket : m_current_search->buckets()) {
        if (!bucket.location().IsFolder()) {
            m_buyout_manager.SetRefreshChecked(bucket.location(), true);
        }
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::OnUncheckAll()
{
    spdlog::trace("MainWindow::OnUncheckAll() entered");
    for (auto const &bucket : m_current_search->buckets()) {
        if (!bucket.location().IsFolder()) {
            m_buyout_manager.SetRefreshChecked(bucket.location(), false);
        }
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::OnRefreshSelected()
{
    spdlog::trace("MainWindow::OnRefreshSelected()");
    // Get names of tabs to refresh
    std::vector<ItemLocation> locations;
    const ItemsModel &model = m_current_search->model();
    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (auto const &index : selected_rows) {
        // The By-Item header row names no tab.
        if (const ItemLocation location = m_current_search->GetTabLocation(index);
            location.IsValid()) {
            locations.push_back(location);
        }
        // Folder-tree R6 / A1: a folder row sends the folder (R9) and its
        // members present in this search whose refresh state is checked.
        const ItemsModel::RowRef ref = model.Locate(index);
        if (ref.isBucket() && m_current_search->isFolderBucket(ref.bucket_row)) {
            for (int n = 0; n < m_current_search->childBucketCount(ref.bucket_row); ++n) {
                const ItemLocation &member
                    = m_current_search->bucket(m_current_search->childBucketRow(ref.bucket_row, n))
                          .location();
                if (m_buyout_manager.GetRefreshChecked(member)) {
                    locations.push_back(member);
                }
            }
        }
    }
    // Nothing to fetch: an update would only run to a clean completed
    // refresh, which an automatic shop submission waits for.
    if (locations.empty()) {
        return;
    }
    m_items_manager.Update(TabSelection::Selected, locations);
}

void MainWindow::CheckSelected(bool value)
{
    spdlog::trace("MainWindow::CheckSelected() entered");
    ItemsModel &model = m_current_search->model();
    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (auto const &index : selected_rows) {
        // A folder row behaves as a click on its checkbox (D10).
        const ItemsModel::RowRef ref = model.Locate(index);
        if (ref.isBucket() && m_current_search->isFolderBucket(ref.bucket_row)) {
            model.setData(model.BucketIndex(ref.bucket_row),
                          value ? Qt::Checked : Qt::Unchecked,
                          Qt::CheckStateRole);
            continue;
        }
        m_buyout_manager.SetRefreshChecked(m_current_search->GetTabLocation(index), value);
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::ResizeTreeColumns()
{
    spdlog::trace("MainWindow::ResizeTreeColumns() entered");
    // Any actual resize supersedes a pending debounced one — the widths
    // it would have refreshed are refreshed now.
    m_delta_resize_debounce.stop();
    auto &probes = ModelProbes::instance();
    if (probes.enabled) {
        ++probes.column_resizes;
    }
    for (int i = 0; i < ui->treeView->header()->count(); ++i) {
        ui->treeView->resizeColumnToContents(i);
    }
}

void MainWindow::ScheduleResizeTreeColumns()
{
    // Supersede at scheduling time, not only when the resize runs: an
    // already-expired debounce timeout can be queued ahead of the 0 ms
    // timer and would otherwise fire first — two passes.
    m_delta_resize_debounce.stop();
    m_delayed_resize_columns.start();
}

void MainWindow::ScheduleDeltaResizeTreeColumns()
{
    // Non-resetting: an armed timer keeps its deadline, so a burst
    // arriving faster than the interval cannot starve the resize.
    if (!m_delta_resize_debounce.isActive()) {
        m_delta_resize_debounce.start();
    }
}

void MainWindow::OnBuyoutChange()
{
    spdlog::trace("MainWindow::OnBuyoutChange() entered");

    const std::optional<double> value = ParseBuyoutValue(ui->buyoutValueLineEdit->text());
    Buyout bo;
    bo.type = Buyout::IndexAsBuyoutType(ui->buyoutTypeComboBox->currentIndex());
    bo.currency = Currency::FromIndex(ui->buyoutCurrencyComboBox->currentIndex());
    bo.value = value.value_or(0.0);
    bo.last_update = QDateTime::currentDateTime();

    if (bo.IsPriced()) {
        ui->buyoutCurrencyComboBox->setEnabled(true);
        ui->buyoutValueLineEdit->setEnabled(true);
    } else {
        ui->buyoutCurrencyComboBox->setEnabled(false);
        ui->buyoutValueLineEdit->setEnabled(false);
    }

    if (!bo.IsValid()) {
        spdlog::trace("MainWindow::OnBuyoutChange() buyout is invalid");
        return;
    }

    // A priced buyout needs a number greater than zero in the value box:
    // empty or unreadable text (a ratio, "1,000", a lone ".") writes nothing
    // and leaves the box as typed.
    if (bo.IsPriced() && !value) {
        spdlog::trace("MainWindow::OnBuyoutChange() the value is not a price");
        return;
    }

    m_shop.ExpireShopData();

    // User commands batch at command scope (M3 R2-5): the loop over the
    // selection plus the trailing propagation pass is one outer batch —
    // one model update at command end, never one per Set. The propagation
    // pass's own boundary nests inside this one and emits nothing (R3-3).
    const BuyoutBatch batch(m_buyout_manager);
    bool saved = true;

    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (const auto &index : selected_rows) {
        const ItemsModel::RowRef ref = m_current_search->model().Locate(index);
        if (ref.isBucket() && m_current_search->isFolderBucket(ref.bucket_row)) {
            continue; // no buyouts on folders, no cascade (D11, R7)
        }
        const auto location = m_current_search->GetTabLocation(index);
        if (ref.isBucket() && !location.IsValid()) {
            continue; // the By-Item header row has no location to price
        }
        const auto tab = location.id();

        // Don't allow users to manually update locked tabs (game priced)
        if (m_buyout_manager.GetTab(location).IsGameSet()) {
            spdlog::trace("MainWindow::OnBuyoutChange() refusing to update locked tab: {}", tab);
            continue;
        }
        if (ref.isBucket()) {
            saved = m_buyout_manager.SetTab(location, bo) && saved;
        } else {
            const int bucket_row = ref.bucket_row;
            if (ref.isItem()) {
                const Bucket &bucket = m_current_search->bucket(bucket_row);
                const int item_row = ref.item_row;
                if (bucket.has_item(item_row)) {
                    const Item &item = *bucket.item(item_row);
                    // Don't allow users to manually update locked items (game priced per item in note section)
                    if (m_buyout_manager.Get(item).IsGameSet()) {
                        spdlog::trace(
                            "MainWindow::OnBuyoutChange() refusing to update locked item: {}",
                            item.name());
                        continue;
                    }
                    saved = m_buyout_manager.Set(item, bo) && saved;
                } else {
                    spdlog::error("OnBuyoutChange(): bucket {} does not have {} items",
                                  bucket_row,
                                  item_row);
                }
            } else {
                spdlog::error("OnBuyoutChange(): bucket {} does not exist", bucket_row);
            }
        }
    }
    m_items_manager.PropagateTabBuyouts();
    ScheduleResizeTreeColumns();
    // Refused (the buyouts did not load, #257) or rejected by the database:
    // the old price stays, so the edit must not look applied. Only a
    // rejection is logged per edit (as an error), so only then does the
    // notice point at the log. It is a temporary status message, so it
    // never replaces refresh progress for long, and the next edit that
    // saves clears it.
    if (saved) {
        statusBar()->clearMessage();
    } else {
        statusBar()
            ->showMessage(m_buyout_manager.BuyoutsLoaded()
                              ? tr("Price not saved. Check the log file for details.")
                              : tr("Price not saved: prices cannot be saved in this session."),
                          PRICE_NOT_SAVED_TIMEOUT_MS);
    }
}

void MainWindow::OnBuyoutsChanged(const BuyoutChangeSet &changes)
{
    spdlog::trace("MainWindow::OnBuyoutsChanged() entered");
    // The batch response under residency (M3 D1 rule 4, R1-6/R3-2),
    // column-gated at the batch boundary: affected Price/Date cells
    // repaint under any sort column (rule 5); with Price or Date active,
    // the affected items' entries in every resident key vector rebuild
    // BEFORE the batch's one reorder, so a re-sort never runs on stale
    // resident keys, and the per-bucket flags scope that reorder to the
    // affected materialized buckets alone.
    for (const auto &search : m_searches) {
        ItemsModel &model = search->model();
        const auto &columns = search->columns();
        const int sort_column = model.GetSortColumn();
        const bool buyout_ordered = (sort_column >= 0)
                                    && (sort_column < static_cast<int>(columns.size()))
                                    && columns[sort_column]->buyoutDependent();
        std::vector<int> resort_rows;
        if (buyout_ordered) {
            // Rebuild resident entries and clear affected flags — for a
            // background search this touches flags only (its keys were
            // evicted at deactivation, R2-4). The returned rows are the
            // affected materialized buckets: the exact reorder scope.
            resort_rows = search->InvalidateBuyoutOrder(changes, sort_column);
        }
        if (search.get() == m_current_search) {
            if (auto &probes = ModelProbes::instance(); probes.enabled) {
                ++probes.model_updates;
            }
            model.RepaintBuyoutCells(changes);
            // The layout operation matches the affected scope (S3 review
            // round 1): none — no reorder, no layout signals, no
            // persistent-index walk (a scoped pricing pass touching only
            // collapsed buckets rides the delta path for free); one — the
            // whole dance scopes to that bucket; several — one view-wide
            // pass beats per-bucket signal storms.
            if (buyout_ordered && !resort_rows.empty()) {
                if (resort_rows.size() == 1) {
                    model.ResortBucket(resort_rows.front());
                } else {
                    model.Resort();
                }
            }
        } else if (buyout_ordered && !resort_rows.empty()) {
            // A background search pays nothing now: its next activation's
            // indicator pass re-sorts exactly the invalidated buckets.
            // Its cells always render fresh (they read the manager), so
            // only the order can go stale — and only when a materialized
            // bucket was actually affected; collapsed buckets' cleared
            // flags already defer their sort to expansion.
            model.SetSorted(false);
        }
    }
}

void MainWindow::OnStatusUpdate(ProgramState state, const QString &message)
{
    QString status;
    switch (state) {
    case ProgramState::Initializing:
        status = "Initializing";
        break;
    case ProgramState::Ready:
        status = "Ready";
        break;
    case ProgramState::Busy:
        status = "Busy";
        break;
    case ProgramState::Waiting:
        status = "Waiting";
        break;
    case ProgramState::Unknown:
        status = "Unknown State";
        break;
    }
    if (!message.isEmpty()) {
        status += ": " + message;
    }
    m_status_bar_label->setText(status);
    m_status_bar_label->update();
}

void MainWindow::OnNotifyUser(const QString &message)
{
    QMessageBox::information(this, "Acquisition", message);
}

void MainWindow::OnShopWarning(const QString &message)
{
    QMessageBox::warning(this, "Acquisition Shop Manager", message);
}

void MainWindow::OnListCurrency()
{
    if (m_currency_dialog == nullptr) {
        m_currency_dialog = new CurrencyDialog(m_settings, m_currency_manager, this);
        connect(&m_currency_manager,
                &CurrencyManager::Updated,
                m_currency_dialog,
                &CurrencyDialog::Update);
    }
    m_currency_dialog->show();
    m_currency_dialog->raise();
    m_currency_dialog->activateWindow();
}

void MainWindow::OnExportCurrency()
{
    const QString file_name = QFileDialog::getSaveFileName(
        this,
        tr("Save Export file"),
        QDir::toNativeSeparators(QDir::homePath() + "/" + "acquisition_export_currency.csv"));
    if (file_name.isEmpty()) {
        return;
    }
    m_currency_manager.ExportCurrency(file_name);
}

bool MainWindow::ConfirmLegacyBuyoutWrite()
{
    return showActionPrompt(this,
                            QMessageBox::Warning,
                            tr("Confirm legacy buyout import"),
                            tr("Import buyouts into the current account?"),
                            tr("This will write the selected buyouts to the current account's data "
                               "file. Are you sure you want to continue?"),
                            tr("Import buyouts"));
}

void MainWindow::OnImportLegacyBuyouts()
{
    if (refuseImportWithoutBuyouts(this, m_buyout_manager)) {
        return;
    }
    if (!showActionPrompt(
            this,
            QMessageBox::Information,
            tr("Recover legacy buyouts"),
            tr("Select a data file containing legacy buyouts."),
            tr("Choose an Acquisition data file from before version 0.16 that contains "
               "the buyouts you want to recover."),
            tr("Choose data file…"))) {
        return;
    }

    const QString file_name = QFileDialog::getOpenFileName(this,
                                                           tr("Open Acquisition data file"),
                                                           QDir::toNativeSeparators(
                                                               m_app_data_dir.absolutePath()),
                                                           tr("Acquisition database files (*)"));
    if (file_name.isEmpty()) {
        return;
    }

    const QString plan_path = legacyBuyoutAuditPath(m_app_data_dir);
    LegacyBuyoutImporter importer(m_buyout_repo,
                                  m_stash_repo,
                                  m_character_repo,
                                  m_settings.value("realm").toString(),
                                  m_settings.value("league").toString());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto restore_cursor = qScopeGuard([] { QApplication::restoreOverrideCursor(); });
    const LegacyBuyoutPlanReport plan = importer.createPlan(file_name, plan_path);
    if (!plan.success) {
        spdlog::info("Legacy buyout planning failed for '{}': {}", file_name, plan.error);
        QMessageBox::warning(this, tr("Legacy buyout import"), plan.error);
        return;
    }
    QApplication::restoreOverrideCursor();
    restore_cursor.dismiss();

    QMessageBox choice(this);
    choice.setWindowTitle(tr("Legacy buyout import"));
    choice.setIcon(QMessageBox::Information);
    choice.setText(plan.summary());
    choice.setInformativeText(
        tr("Import works best immediately after a full refresh of stashes and characters.\n\n"
           "The audit plan is saved at:\n%1")
            .arg(QDir::toNativeSeparators(plan_path)));
    QPushButton *const import_now = choice.addButton(tr("Import now"), QMessageBox::AcceptRole);
    QPushButton *const save_plan = choice.addButton(tr("Save plan for review…"),
                                                    QMessageBox::ActionRole);
    save_plan->setMinimumWidth(save_plan->sizeHint().width());
    QPushButton *const cancel = choice.addButton(QMessageBox::Cancel);
    choice.setDefaultButton(cancel);
    choice.exec();

    if (choice.clickedButton() == save_plan) {
        const QString destination
            = QFileDialog::getSaveFileName(this,
                                           tr("Save legacy buyout plan"),
                                           QDir::toNativeSeparators(
                                               QFileInfo(file_name).dir().filePath(
                                                   QFileInfo(plan_path).fileName())),
                                           tr("Excel workbooks (*.xlsx)"));
        if (destination.isEmpty()) {
            return;
        }
        QString copy_error;
        if (!copyPlanFile(plan_path, destination, copy_error)) {
            QMessageBox::warning(this,
                                 tr("Legacy buyout import"),
                                 tr("Could not save the plan: %1").arg(copy_error));
            return;
        }
        QMessageBox::information(this,
                                 tr("Legacy buyout import"),
                                 tr("The editable plan was saved to:\n%1")
                                     .arg(QDir::toNativeSeparators(destination)));
        return;
    }
    if (choice.clickedButton() != import_now) {
        return;
    }
    if (!ConfirmLegacyBuyoutWrite()) {
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto restore_apply_cursor = qScopeGuard([] { QApplication::restoreOverrideCursor(); });
    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    if (!report.success) {
        spdlog::error("Legacy buyout plan apply failed for '{}': {}", plan_path, report.error);
        QMessageBox::warning(this,
                             tr("Legacy buyout import"),
                             report.error + "\n\n" + report.summary());
        return;
    }
    if (report.imported > 0) {
        OnLegacyBuyoutsImported();
    }
    QApplication::restoreOverrideCursor();
    restore_apply_cursor.dismiss();
    QString log_summary = report.summary();
    log_summary.replace('\n', ", ");
    spdlog::info("Legacy buyout import from '{}' via '{}': {}", file_name, plan_path, log_summary);
    if (report.warning.isEmpty()) {
        QMessageBox::information(this, tr("Legacy buyout import"), report.summary());
    } else {
        spdlog::warn("Legacy buyout import: {}", report.warning);
        QMessageBox::warning(this,
                             tr("Legacy buyout import"),
                             report.summary() + "\n\n" + report.warning);
    }
}

void MainWindow::OnImportLegacyBuyoutPlan()
{
    if (refuseImportWithoutBuyouts(this, m_buyout_manager)) {
        return;
    }
    if (!showActionPrompt(this,
                          QMessageBox::Information,
                          tr("Import buyout plan"),
                          tr("Select a previously exported or edited buyout plan."),
                          tr("Choose an Excel workbook previously exported by Acquisition, either "
                             "unchanged or edited."),
                          tr("Choose plan…"))) {
        return;
    }

    const QString plan_path = QFileDialog::getOpenFileName(this,
                                                           tr("Open legacy buyout plan"),
                                                           QDir::toNativeSeparators(
                                                               m_app_data_dir.absolutePath()),
                                                           tr("Excel workbooks (*.xlsx)"));
    if (plan_path.isEmpty()) {
        return;
    }

    if (!ConfirmLegacyBuyoutWrite()) {
        return;
    }

    LegacyBuyoutImporter importer(m_buyout_repo);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto restore_cursor = qScopeGuard([] { QApplication::restoreOverrideCursor(); });
    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    if (!report.success) {
        spdlog::error("Legacy buyout plan apply failed for '{}': {}", plan_path, report.error);
        QMessageBox::warning(this,
                             tr("Legacy buyout import"),
                             report.error + "\n\n" + report.summary());
        return;
    }
    if (report.imported > 0) {
        OnLegacyBuyoutsImported();
    }
    QApplication::restoreOverrideCursor();
    restore_cursor.dismiss();
    QString log_summary = report.summary();
    log_summary.replace('\n', ", ");
    spdlog::info("Legacy buyout plan applied from '{}': {}", plan_path, log_summary);
    if (report.warning.isEmpty()) {
        QMessageBox::information(this, tr("Legacy buyout import"), report.summary());
    } else {
        spdlog::warn("Legacy buyout import: {}", report.warning);
        QMessageBox::warning(this,
                             tr("Legacy buyout import"),
                             report.summary() + "\n\n" + report.warning);
    }
}

void MainWindow::OnLegacyBuyoutsImported()
{
    const BuyoutBatch batch(m_buyout_manager);
    m_buyout_manager.ReloadBuyouts();
    m_items_manager.PropagateTabBuyouts();
    m_shop.ExpireShopData();
    ScheduleResizeTreeColumns();
}

bool MainWindow::eventFilter(QObject *o, QEvent *e)
{
    if ((o == m_tab_bar) && (e->type() == QEvent::MouseButtonPress)) {
        QMouseEvent *mouse_event = static_cast<QMouseEvent *>(e);
        int index = m_tab_bar->tabAt(mouse_event->pos());
        if ((index >= 0) && (index < m_tab_bar->count() - 1)) {
            if (mouse_event->button() == Qt::MiddleButton) {
                OnDeleteTabClicked(index);
                return true;
            } else if (mouse_event->button() == Qt::RightButton) {
                QMenu menu;
                menu.addAction("Rename Tab", this, [=, this]() { OnRenameTabClicked(index); });
                menu.addAction("Delete Tab", this, [=, this]() { OnDeleteTabClicked(index); });
                menu.exec(QCursor::pos());
            }
        }
    }
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::OnRenameTabClicked(int index)
{
    bool ok;
    QString name
        = QInputDialog::getText(this, "Rename Tab", "Rename Tab here", QLineEdit::Normal, "", &ok);

    if (ok && !name.isEmpty()) {
        m_searches[index]->RenameCaption(name);
        m_tab_bar->setTabText(index, m_searches[index]->GetCaption());
    }
}

void MainWindow::OnDeleteTabClicked(int index)
{
    // If the user is deleting the last search, create a new
    // one to replace it, because the UI breaks without at
    // least one search.
    if (m_searches.size() == 1) {
        NewSearch();
    }

    // Delete the search.
    auto &search = m_searches[index];
    m_search_form->unbind(*search);
    if (m_current_search == search.get()) {
        m_current_search = nullptr;
    }
    m_searches.erase(m_searches.begin() + index);

    // removeTab emits currentChanged synchronously, re-entering OnTabChange.
    // When the deleted search was current, FlushPendingSearchFormChange is a
    // no-op because m_current_search is null, and the view receives its new
    // model before repaint can touch the destroyed search.
    m_tab_bar->removeTab(index);
}

void MainWindow::OnTabRefreshed(const ItemLocation &location, const Items &items)
{
    // Background searches keep M2 D9 rule 1 verbatim (R1-7): every delta
    // marks them items-dirty, and their next activation refilters.
    for (const auto &search : m_searches) {
        if (search.get() != m_current_search) {
            search->setItemsDirty(true);
        }
    }
    if (!m_current_search) {
        return;
    }
    // The first delta opens the selection-intent window (R1-3).
    m_refresh_active = true;
    const SelectionCapture capture = CaptureSelection(TouchedBucket(location));
    // The active search applies the delta now: bucket-scoped row
    // operations in By-Tab (D3), the flat sorted merge in By-Item (D4,
    // S5 — the D9 throttled fallback is gone).
    m_applying_delta = true;
    const auto result = m_current_search->ApplyTabDelta(location, items);
    m_applying_delta = false;
    FinishDeltaApplication(result.processed,
                           result.model_changed,
                           result.inserted_bucket_rows,
                           capture);
}

void MainWindow::OnChildrenReconciled(const ItemLocation &parent,
                                      const std::vector<FetchSourceKey> &expected)
{
    // Aggregate reconciliations are first-class delta inputs (R5-2/R6-2);
    // background searches keep rule 1, and the active By-Tab search
    // applies the erase as row removals scoped to the parent's bucket (D3).
    for (const auto &search : m_searches) {
        if (search.get() != m_current_search) {
            search->setItemsDirty(true);
        }
    }
    if (!m_current_search) {
        return;
    }
    m_refresh_active = true; // the intent window covers every delta form
    const SelectionCapture capture = CaptureSelection(TouchedBucket(parent));
    m_applying_delta = true;
    const auto result = m_current_search->ApplyChildReconciliation(parent, expected);
    m_applying_delta = false;
    FinishDeltaApplication(result.processed,
                           result.model_changed,
                           result.inserted_bucket_rows,
                           capture);
}

void MainWindow::FinishDeltaApplication(bool processed,
                                        bool model_changed,
                                        const std::vector<int> &inserted_bucket_rows,
                                        const SelectionCapture &capture)
{
    if (!processed) {
        // Fail-safe direction (R1-7): a skipped application leaves the
        // flag dirty; the next activation or the final snapshot pays.
        m_current_search->setItemsDirty(true);
        return;
    }
    ExpandInsertedBuckets(inserted_bucket_rows);
    // The caption renders the maintained count.
    for (size_t i = 0; i < m_searches.size(); ++i) {
        if (m_searches[i].get() == m_current_search) {
            m_tab_bar->setTabText(static_cast<int>(i), m_current_search->GetCaption());
            break;
        }
    }
    if (model_changed) {
        // Debounced, not immediate: per-delta width refresh is redundant
        // for most replacements and was ~10 ms per reply (S7 record).
        ScheduleDeltaResizeTreeColumns();
    }
    ReconcileSelectionIntent(&capture);
    RestoreSelection(capture);
}

LocationInventory::Key MainWindow::TouchedBucket(const ItemLocation &location) const
{
    return LocationInventory::KeyFor(
        (m_current_search->GetViewMode() == Search::ViewMode::ByItem) ? ItemLocation() : location);
}

MainWindow::SelectionCapture MainWindow::CaptureSelection(
    std::optional<LocationInventory::Key> bucket) const
{
    SelectionCapture capture;
    capture.bucket = bucket;
    if (!m_current_search || (ui->treeView->model() != &m_current_search->model())) {
        return capture;
    }
    const ItemsModel &model = m_current_search->model();
    const QItemSelectionModel &selection = *ui->treeView->selectionModel();
    capture.current_selected = selection.isSelected(selection.currentIndex());
    QModelIndex only_parent;
    if (bucket) {
        const int bucket_row = m_current_search->rowForKey(*bucket);
        if (bucket_row < 0) {
            return capture; // the bucket is not shown, so nothing in it is selected
        }
        only_parent = model.BucketIndex(bucket_row);
    }
    // The selection's ranges, read only for the delta's bucket: rows
    // selected in other buckets are not examined (M3 D3).
    auto &probes = ModelProbes::instance();
    for (const QItemSelectionRange &range : selection.selection()) {
        if (bucket && (range.parent() != only_parent)) {
            continue;
        }
        const ItemsModel::RowRef ref = model.Locate(range.parent());
        if (!ref.isBucket()) {
            continue; // header rows
        }
        const Bucket &parent = m_current_search->bucket(ref.bucket_row);
        for (int row = range.top(); row <= range.bottom(); ++row) {
            if (probes.enabled) {
                ++probes.selection_restore_steps;
            }
            if (parent.has_item(row) && !parent.item(row)->id().isEmpty()) {
                capture.item_ids.insert(parent.item(row)->id());
            }
        }
    }
    return capture;
}

void MainWindow::RestoreSelection(const SelectionCapture &capture)
{
    if ((capture.item_ids.empty() && m_carried_ids.empty()) || !m_current_search
        || (ui->treeView->model() != &m_current_search->model())) {
        return;
    }
    std::vector<int> bucket_rows;
    if (capture.bucket) {
        if (const int row = m_current_search->rowForKey(*capture.bucket); row >= 0) {
            bucket_rows.push_back(row);
        }
    } else {
        for (int row = 0; row < static_cast<int>(m_current_search->buckets().size()); ++row) {
            bucket_rows.push_back(row);
        }
    }
    // One walk of the delta's bucket. A carried item can only come back
    // through a delta that inserts it, so this bucket is where it lands.
    const ItemsModel &model = m_current_search->model();
    QItemSelectionModel &selection = *ui->treeView->selectionModel();
    QItemSelection rows;
    std::set<QString> found;
    auto &probes = ModelProbes::instance();
    for (const int bucket_row : bucket_rows) {
        const QModelIndex parent = model.BucketIndex(bucket_row);
        const Items &items = m_current_search->bucket(bucket_row).items();
        for (size_t row = 0; row < items.size(); ++row) {
            if (probes.enabled) {
                ++probes.selection_restore_steps;
            }
            const QString id = items[row]->id();
            if ((capture.item_ids.count(id) == 0) && (m_carried_ids.count(id) == 0)) {
                continue;
            }
            found.insert(id);
            const QModelIndex index = model.index(static_cast<int>(row), 0, parent);
            if (!selection.isSelected(index)) {
                rows.select(index, index);
            }
        }
    }
    if (!rows.isEmpty()) {
        m_reselecting = true;
        selection.select(rows, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        m_reselecting = false;
    }
    for (const QString &id : found) {
        m_carried_ids.erase(id);
    }
    for (const QString &id : capture.item_ids) {
        if ((found.count(id) == 0) && !m_current_search->visibleItemById(id)) {
            m_carried_ids.insert(id);
        }
    }
}

void MainWindow::SelectWithoutExpanding(const QModelIndex &index,
                                        QItemSelectionModel::SelectionFlags flags)
{
    // Making a row current has the view scroll to it, and QTreeView's
    // scrollTo first expands every collapsed ancestor of the row. A
    // reselection the refresh makes on the user's behalf must not undo a
    // collapse the user made (folder-tree D9: a collapsed folder stays
    // collapsed; the same holds for a collapsed tab) — an expanded signal
    // here would also drop the folder from the user-collapsed set. The
    // view only expands while its items are expandable, so that is switched
    // off for the one call: a row hidden under a collapsed ancestor becomes
    // current and selected where it is, and a visible row is scrolled to as
    // before.
    const bool expandable = ui->treeView->itemsExpandable();
    ui->treeView->setItemsExpandable(false);
    m_reselecting = true;
    ui->treeView->selectionModel()->setCurrentIndex(index, flags);
    m_reselecting = false;
    ui->treeView->setItemsExpandable(expandable);
}

void MainWindow::ReconcileSelectionIntent(const SelectionCapture *capture)
{
    if (!m_current_search) {
        return;
    }
    if (m_current_item) {
        const QModelIndex index = m_current_search->index(m_current_item);
        if (index.isValid()) {
            // The row survived (possibly moved). Qt's selection model can
            // drop a child row's visual selection through a top-level
            // move even though its persistent index stays valid —
            // re-assert the selection at the surviving index, when the
            // row was selected before the delta (the user may have
            // deselected the current row).
            if (!ui->treeView->selectionModel()->isSelected(index)
                && (!capture || capture->current_selected)) {
                SelectWithoutExpanding(index,
                                       QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
            // The details pane's location line renders canonical
            // metadata (S4 review round 1): a metadata delta can rename
            // the selected item's tab without replacing the item, and
            // the reset-reselect cycle that used to refresh the pane is
            // gone from the delta path.
            ui->locationLabel->setText(m_items_manager.locationInventory()
                                           .Canonical(m_current_item->location())
                                           .GetHeader());
        } else {
            // The current row left mid-refresh: the intent stays alive,
            // the row's selection lapses (R1-3); the other selected rows
            // keep theirs. The view moved current to a neighbor, without
            // selecting it, when the row was removed — clear that under
            // the guard so it cannot overwrite the intent.
            m_current_item = nullptr;
            m_current_bucket_location.reset();
            m_applying_delta = true;
            ui->treeView->selectionModel()->setCurrentIndex(QModelIndex(),
                                                            QItemSelectionModel::NoUpdate);
            m_applying_delta = false;
            ClearCurrentItem();
            ResetBuyoutWidgets();
        }
    }
    if (!m_current_item && m_current_bucket_location) {
        // A selected bucket header follows its stable key (S4 review
        // round 1): a metadata delta renames/moves/recolors it in place,
        // so the stored location and the details pane refresh here — the
        // reset-reselect cycle used to do this implicitly. A bucket the
        // filtered-empty convergence removed clears the selection.
        const auto key = LocationInventory::KeyFor(*m_current_bucket_location);
        const int bucket_row = m_current_search->rowForKey(key);
        if (bucket_row >= 0) {
            const ItemLocation &fresh = m_current_search->bucket(bucket_row).location();
            const bool rendered_changed = !fresh.RendersSameAs(*m_current_bucket_location);
            if (rendered_changed) {
                m_current_bucket_location = fresh;
                UpdateCurrentBucket();
                UpdateCurrentBuyout();
            }
        } else {
            m_current_bucket_location.reset();
            m_applying_delta = true;
            ui->treeView->selectionModel()->setCurrentIndex(QModelIndex(),
                                                            QItemSelectionModel::NoUpdate);
            m_applying_delta = false;
            ClearCurrentItem();
            ResetBuyoutWidgets();
        }
    }
    if (!m_current_item && m_refresh_active && !m_selection_intent_id.isEmpty()) {
        // Re-adoption through the global identity index: any delta
        // inserting an item with the intent's id — any bucket — makes it
        // current again via the normal selection path. On the delta path
        // its selection is RestoreSelection's: selected again only if it
        // was selected when it left.
        if (const auto adopted = m_current_search->visibleItemById(m_selection_intent_id)) {
            const QModelIndex index = m_current_search->index(adopted);
            if (index.isValid()) {
                SelectWithoutExpanding(index,
                                       capture ? QItemSelectionModel::NoUpdate
                                               : (QItemSelectionModel::Select
                                                  | QItemSelectionModel::Rows));
            }
        }
    }
}

void MainWindow::OnRefreshFinished(const RefreshOutcome &outcome)
{
    Q_UNUSED(outcome);
    // R2-1: every terminal outcome closes the intent window. On success
    // the final snapshot's row reconciliation has already reselected or
    // cleared (R1-2, S6); on failure —
    // which emits no final snapshot — the absence check runs here, so a
    // stale intent can never survive one refresh and reselect an item in
    // a later one. The immediate delta path keeps the current search's
    // indexes fresh (S5: no fallback can leave them stale), so the check
    // adjudicates honestly at the terminal event itself — the S4-era
    // deferral machinery died with the seam.
    m_refresh_active = false;
    m_carried_ids.clear();
    if (m_selection_intent_id.isEmpty() || !m_current_search) {
        return;
    }
    if (!m_current_search->visibleItemById(m_selection_intent_id)) {
        m_selection_intent_id.clear();
    }
}

void MainWindow::OnSearchFormChange()
{
    spdlog::trace("MainWindow::OnSearchFormChange() entered");
    // ModelViewRefresh captures expansion and scroll itself now (R6-3): the
    // view is showing this search's model on the form-change path.
    m_current_search->SetRefreshReason(RefreshReason::SearchFormChanged);
    ModelViewRefresh();
}

void MainWindow::SaveViewExpansion(Search &search)
{
    ++ModelProbes::instance().expansion_captures;
    // Expansion is keyed by the stable (type, id) display key (M2 R6-3):
    // header text mutates when a delta renames a tab, which would orphan a
    // header-keyed save exactly when a restore (D6 user refilter, mode
    // switch) needs it.
    // Folders are not saved here: their state is the search's
    // user-collapsed set, maintained from the view's signals (D9).
    std::set<LocationInventory::Key> expanded;
    if (!search.defaultExpanded()) {
        const int bucket_count = static_cast<int>(search.buckets().size());
        for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
            const QModelIndex index = search.model().BucketIndex(bucket_row);
            if (index.isValid() && !search.isFolderBucket(bucket_row)
                && ui->treeView->isExpanded(index)) {
                expanded.emplace(LocationInventory::KeyFor(search.bucket(bucket_row).location()));
            }
        }
    }
    search.setExpandedKeys(std::move(expanded));
}

void MainWindow::RestoreViewExpansion(Search &search)
{
    ++ModelProbes::instance().expansion_restores;
    // Folders follow the user-collapsed set (folder-tree D9, R3); tab
    // buckets keep today's rule (A3) — every one expanded in a
    // default-expanded search, the saved keys otherwise. Bucket headers
    // only: nothing stores expansion state for item rows.
    const bool default_expanded = search.defaultExpanded();
    const auto &keys = search.expandedKeys();
    const int bucket_count = static_cast<int>(search.buckets().size());
    if (default_expanded) {
        // One pass for the top level, as before folders: expandToDepth(0)
        // lays out every top-level header at once (depth 0 — never an item
        // row). Its expanded signals would clear the user-collapsed folder
        // set, so the set is kept aside and restored, then folders in it
        // collapse and nested tabs expand below.
        const auto collapsed = search.collapsedFolderKeys();
        ui->treeView->expandToDepth(0);
        search.setCollapsedFolderKeys(collapsed);
        for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
            const QModelIndex index = search.model().BucketIndex(bucket_row);
            if (search.isFolderBucket(bucket_row)) {
                if (search.folderCollapsed(
                        LocationInventory::KeyFor(search.bucket(bucket_row).location()))) {
                    ui->treeView->collapse(index);
                }
            } else if (search.parentBucketRow(bucket_row) >= 0) {
                ui->treeView->expand(index);
            }
        }
        return;
    }
    for (int bucket_row = 0; bucket_row < bucket_count; ++bucket_row) {
        const QModelIndex index = search.model().BucketIndex(bucket_row);
        const auto key = LocationInventory::KeyFor(search.bucket(bucket_row).location());
        if (search.isFolderBucket(bucket_row)) {
            if (search.folderCollapsed(key)) {
                ui->treeView->collapse(index);
            } else {
                ui->treeView->expand(index);
            }
            continue;
        }
        if (keys.count(key) > 0) {
            ui->treeView->expand(index);
        } else {
            ui->treeView->collapse(index);
            // collapse() emits no signal for an already-collapsed row —
            // the usual state after a reset — so sync the materialization
            // mark explicitly (idempotent; evicts any stale keys).
            search.model().OnBucketCollapsed(bucket_row);
        }
    }
}

void MainWindow::ModelViewRefresh()
{
    spdlog::trace("MainWindow::ModelViewRefresh() entered");
    disconnect(m_current_item_conn);
    disconnect(m_selection_changed_conn);
    disconnect(m_tree_pressed_conn);

    m_buyout_manager.Save();

    // Capture expansion and scroll immediately before every reset (M2
    // R6-3) — including the refresh paths that used to restore without
    // saving, which replayed stale state on every throttled tick. Capture
    // only when the view is actually showing this search's model: during a
    // tab switch it still shows the outgoing search, whose state OnTabChange
    // already saved.
    ItemsModel &model = m_current_search->model();
    if (ui->treeView->model() == &model) {
        SaveViewExpansion(*m_current_search);
        SaveViewScroll(*m_current_search);
    }

    spdlog::trace("MainWindow::ModelViewRefresh() activating current search");
    m_search_form->saveTo(*m_current_search);
    m_current_search->FilterItems(m_items_manager.items());
    ui->treeView->setSortingEnabled(false);
    if (ui->treeView->model() != &model) {
        // setModel() creates a new selection model and leaves the old one to
        // the caller (QAbstractItemView docs); nothing else shares it.
        QItemSelectionModel *old_selection = ui->treeView->selectionModel();
        ui->treeView->setModel(&model);
        if (old_selection) {
            old_selection->deleteLater();
        }
    }
    ui->treeView->header()->setSortIndicator(model.GetSortColumn(), model.GetSortOrder());
    ui->treeView->setSortingEnabled(true);
    // The R3-1 eager carve-out (S5), with dirtiness already decided: a
    // dirty search refiltered above and the indicator pass's sort
    // supplied its keys; a clean By-Item activation hydrates the flat
    // bucket's keys now, so no delta ever meets a keyless flat bucket
    // (D4 rule 1). No-op in By-Tab mode and when keys are resident.
    m_current_search->HydrateFlatBucketKeys();
    RestoreViewExpansion(*m_current_search);
    ScheduleResizeTreeColumns();

    // This updates the item information when current item changes.
    m_current_item_conn = connect(ui->treeView->selectionModel(),
                                  &QItemSelectionModel::currentChanged,
                                  this,
                                  &MainWindow::OnCurrentItemChanged);
    // Any change of the selection the refresh did not make is the user's,
    // and so is a click on a row even when it changes nothing visible (a
    // click on the only selected row, which was current): a user's
    // selection wins (R1-3), so the rows a delta removed are no longer
    // carried.
    const auto user_selected = [this] {
        if (!m_applying_delta && !m_reselecting) {
            m_carried_ids.clear();
        }
    };
    m_selection_changed_conn = connect(ui->treeView->selectionModel(),
                                       &QItemSelectionModel::selectionChanged,
                                       this,
                                       user_selected);
    m_tree_pressed_conn = connect(ui->treeView, &QAbstractItemView::pressed, this, user_selected);

    ui->viewComboBox->setCurrentIndex(static_cast<int>(m_current_search->GetViewMode()));

    // During a tab-switch flush the tab bar already points at the destination
    // tab while m_current_search is still the outgoing search, so resolve the
    // caption's tab from the search list rather than the bar (F41).
    for (size_t i = 0; i < m_searches.size(); ++i) {
        if (m_searches[i].get() == m_current_search) {
            m_tab_bar->setTabText(static_cast<int>(i), m_current_search->GetCaption());
            break;
        }
    }

    ReselectCurrentItem();
    RestoreViewScroll(*m_current_search);

    // The intent pass runs on refilter paths too (S4 review round 1): an
    // id that reappeared only in a refilter's fresh result re-adopts
    // here — the contract covers the delta path and the refilter alike.
    ReconcileSelectionIntent();
}

void MainWindow::SaveViewScroll(Search &search)
{
    ++ModelProbes::instance().scroll_captures;
    Search::ScrollAnchor anchor;
    anchor.scrollbar_value = ui->treeView->verticalScrollBar()->value();
    const QModelIndex top = ui->treeView->indexAt(QPoint(0, 0));
    const ItemsModel::RowRef ref = search.model().Locate(top);
    if (ref.isItem()) {
        // The top row is an item: anchor on its bucket key and stable
        // item id so the same item returns to the top after the reset.
        const Bucket &bucket = search.bucket(ref.bucket_row);
        anchor.bucket_key = LocationInventory::KeyFor(bucket.location());
        if (bucket.has_item(ref.item_row)) {
            anchor.item_id = bucket.item(ref.item_row)->id();
        }
    } else if (ref.isBucket()) {
        // The top row is a bucket header; an empty item_id records that.
        anchor.bucket_key = LocationInventory::KeyFor(search.bucket(ref.bucket_row).location());
    }
    search.setScrollAnchor(std::move(anchor));
}

void MainWindow::RestoreViewScroll(Search &search)
{
    ++ModelProbes::instance().scroll_restores;
    const Search::ScrollAnchor &anchor = search.scrollAnchor();
    if (anchor.bucket_key) {
        const auto &buckets = search.buckets();
        for (size_t row = 0; row < buckets.size(); ++row) {
            const Bucket &bucket = buckets[row];
            if (LocationInventory::KeyFor(bucket.location()) != *anchor.bucket_key) {
                continue;
            }
            const QModelIndex bucket_index = search.model().BucketIndex(static_cast<int>(row));
            if (anchor.item_id.isEmpty()) {
                // The anchor was the bucket header itself.
                ui->treeView->scrollTo(bucket_index, QAbstractItemView::PositionAtTop);
                return;
            }
            const auto &items = bucket.items();
            for (size_t n = 0; n < items.size(); ++n) {
                if (items[n]->id() == anchor.item_id) {
                    ui->treeView->scrollTo(search.model().index(static_cast<int>(n),
                                                                0,
                                                                bucket_index),
                                           QAbstractItemView::PositionAtTop);
                    return;
                }
            }
            // The anchored item was removed: fall through to the raw value —
            // never scroll the anchor's bucket header to the top (R6-3
            // post-freeze amendment).
            break;
        }
    }
    ui->treeView->verticalScrollBar()->setValue(anchor.scrollbar_value);
}

void MainWindow::OnCurrentItemChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    spdlog::trace("MainWindow::OnCurrentItemChange() entered");
    // Row operations make the view shuffle its current index (removing
    // the current row moves current to a neighbor); those shifts are not
    // selections and must not overwrite the intent (R1-3) — the
    // application's own reconcile pass settles selection afterward.
    if (m_applying_delta) {
        return;
    }
    m_buyout_manager.Save();

    if (!current.isValid()) {
        return;
    }

    const ItemsModel::RowRef ref = m_current_search->model().Locate(current);
    if (!ref.isBucket()) {
        // Clicked on an item. The warning branches reset the stored selection
        // instead of keeping the previous item/bucket pair, so the panel
        // clears rather than rendering stale state (F44).
        const int bucket_row = ref.bucket_row;
        if (ref.isItem()) {
            const Bucket &bucket = m_current_search->bucket(bucket_row);
            const int item_row = ref.item_row;
            if (bucket.has_item(item_row)) {
                m_current_item = bucket.item(item_row);
                m_current_bucket_location.reset();
                // A selection wins at any time (R1-3): the intent follows
                // the selected item's stable id.
                m_selection_intent_id = m_current_item->id();
                m_delayed_update_current_item.start();
            } else {
                spdlog::warn("OnCurrentItemChanged(): parent bucket {} does not have {} rows",
                             bucket_row,
                             item_row);
                m_current_item = nullptr;
                m_current_bucket_location.reset();
                ClearCurrentItem();
            }
        } else {
            spdlog::warn("OnCurrentItemChanged(): parent bucket {} does not exist", bucket_row);
            m_current_item = nullptr;
            m_current_bucket_location.reset();
            ClearCurrentItem();
        }
    } else {
        // Clicked on a bucket
        m_current_item = nullptr;
        m_selection_intent_id.clear();
        m_current_bucket_location = m_current_search->bucket(ref.bucket_row).location();
        UpdateCurrentBucket();
    }
    if (m_current_item || m_current_bucket_location) {
        UpdateCurrentBuyout();
    } else {
        ResetBuyoutWidgets();
    }
}

void MainWindow::ReselectCurrentItem()
{
    ++ModelProbes::instance().reselects;
    spdlog::trace("MainWindow::ReselectCurrentItem() entered");

    // A bucket (stash tab header row) can be the current selection too (F43).
    if (m_current_item == nullptr) {
        if (m_current_bucket_location) {
            ReselectCurrentBucket();
        } else {
            spdlog::trace("MainWindow::ReselectCurrentItem() nothing was selected");
        }
        return;
    }

    // Global stable-identity reselection (M2 R6-3): the refilter replaced
    // this item's object if its tab streamed a delta, and may have filed it
    // under another tab if it moved mid-refresh. Adopt the current object
    // for the selection AND the details panel — the old pointer is exactly
    // what a streamed replacement invalidates. Items without a server id
    // fall back to pointer identity below.
    if (const auto adopted = m_current_search->visibleItemById(m_current_item->id())) {
        if (adopted != m_current_item) {
            m_current_item = adopted;
            // Re-render the details panel from the replacement object, the
            // same deferred path a user selection takes.
            m_delayed_update_current_item.start();
        }
    }

    // Look for the new index of the currently selected item.
    const QModelIndex index = m_current_search->index(m_current_item);

    if (!index.isValid()) {
        // The previously selected item is no longer in search results.
        spdlog::trace("MainWindow::ReselectCurrentItem() the previously selected item is gone");
        m_current_item = nullptr;
        m_current_bucket_location.reset();
        ClearCurrentItem();
    } else {
        // Reselect the item in the updated layout.
        spdlog::trace("MainWindow::ReselectCurrentItem() reselecting the previous item");
        ui->treeView->selectionModel()->select(index,
                                               QItemSelectionModel::Current
                                                   | QItemSelectionModel::Select
                                                   | QItemSelectionModel::Rows);
    }
}

void MainWindow::ReselectCurrentBucket()
{
    const auto &buckets = m_current_search->buckets();
    for (size_t row = 0; row < buckets.size(); ++row) {
        if (buckets[row].location() == *m_current_bucket_location) {
            spdlog::trace("MainWindow::ReselectCurrentBucket() reselecting the previous bucket");
            const QModelIndex index = m_current_search->model().BucketIndex(static_cast<int>(row));
            ui->treeView->selectionModel()->select(index,
                                                   QItemSelectionModel::Current
                                                       | QItemSelectionModel::Select
                                                       | QItemSelectionModel::Rows);
            return;
        }
    }
    // The previously selected bucket is no longer in the search results.
    spdlog::trace("MainWindow::ReselectCurrentBucket() the previously selected bucket is gone");
    m_current_bucket_location.reset();
    ClearCurrentItem();
}

void MainWindow::OnDelayedSearchFormChange()
{
    m_delayed_search_form_change.start();
}

void MainWindow::FlushPendingSearchFormChange()
{
    // A debounced form change belongs to the search that was current when the
    // edit was made. Apply it before the form is rebound, or the timer fires
    // against the wrong search and the edited one is left with stale buckets
    // and caption.
    if (m_current_search && m_delayed_search_form_change.isActive()) {
        m_delayed_search_form_change.stop();
        OnSearchFormChange();
    }
}

void MainWindow::OnTabChange(int index)
{
    FlushPendingSearchFormChange();
    if (m_current_search) {
        SaveViewExpansion(*m_current_search);
        // Scroll is captured here too (R6-3): this is the last moment the
        // view still shows the outgoing search's model. When the dirty
        // search is reactivated, ModelViewRefresh's capture gate correctly
        // skips it (the view shows the other search), so THIS save is what
        // its restore replays.
        SaveViewScroll(*m_current_search);
        m_current_search->setCurrentItem(m_current_item);
        m_current_search->setCurrentBucket(m_current_bucket_location);
        // R2-4: residency is scoped to the active search. Deactivation
        // evicts every key vector; orders and flags persist, so a clean
        // search reactivates without a refilter and rehydrates lazily.
        m_current_search->EvictResidentKeys();
    }
    if (static_cast<size_t>(index) == m_searches.size()) {
        // "+" clicked
        NewSearch();
    } else {
        m_current_search = m_searches[index].get();
        m_current_item = m_current_search->currentItem();
        m_current_bucket_location = m_current_search->currentBucket();
        // The intent follows the window's visible selection: it re-anchors
        // to the incoming search's saved selection (R1-3).
        m_selection_intent_id = m_current_item ? m_current_item->id() : QString();
        m_carried_ids.clear();
        m_current_search->SetRefreshReason(RefreshReason::TabChanged);
        m_search_form->loadFrom(*m_current_search);
        ModelViewRefresh();
        UpdateCurrentItem();
        if (m_current_bucket_location) {
            UpdateCurrentBucket();
        }
        if (m_current_item || m_current_bucket_location) {
            UpdateCurrentBuyout();
        } else {
            ResetBuyoutWidgets();
        }
    }
}

void MainWindow::InitializeSearchForm()
{
    const FilterCallbacks callbacks{
        this,
        [this] { OnSearchFormChange(); },
        [this] { OnDelayedSearchFormChange(); },
    };
    m_search_form = std::make_unique<SearchForm>(*m_search_form_layout, m_filter_catalog, callbacks);
}

void MainWindow::NewSearch()
{
    spdlog::trace("MainWindow::NewSearch() entered");

    ++m_search_count;

    QString caption = QString("Search %1").arg(m_search_count);

    spdlog::trace("MainWindow::NewSearch() adding tab");
    m_tab_bar->setTabText(m_tab_bar->count() - 1, caption);
    m_tab_bar->addTab("+");

    spdlog::trace("MainWindow::NewSearch() setting current search: {}", caption);
    auto search = std::make_unique<Search>(m_buyout_manager,
                                           caption,
                                           m_filter_catalog,
                                           &m_items_manager.locationInventory());
    m_current_search = search.get();
    m_current_item = m_current_search->currentItem();
    m_current_bucket_location = m_current_search->currentBucket();
    m_selection_intent_id.clear();
    m_carried_ids.clear();
    m_current_search->SetRefreshReason(RefreshReason::TabCreated);

    // this can't be done in ctor because it'll call OnSearchFormChange slot
    // and remove all previous search data
    spdlog::trace("MainWindow::NewSearch() reseting search form and adding the search");
    m_search_form->reset();
    m_searches.push_back(std::move(search));

    spdlog::trace("MainWindow::NewSearch() triggering model view refresh");
    ModelViewRefresh();
    UpdateCurrentItem();
    ResetBuyoutWidgets();
}

void MainWindow::ClearCurrentItem()
{
    spdlog::trace("MainWindow::ClearCurrentItem() entered");
    // A pending details update belongs to an item that is no longer shown.
    m_delayed_update_current_item.stop();
    ui->imageLabel->hide();
    ui->minimapLabel->hide();
    ui->locationLabel->hide();
    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    ui->nameLabel->setText("Select an item");
    ui->nameLabel->show();

    ui->pobTooltipButton->setEnabled(false);
}

void MainWindow::UpdateCurrentBucket()
{
    spdlog::trace("MainWindow::UpdateCurrentBucket() entered");
    // A pending item details update would overwrite the bucket's.
    m_delayed_update_current_item.stop();
    ui->imageLabel->hide();
    ui->minimapLabel->hide();
    ui->locationLabel->hide();
    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    ui->nameLabel->setText(m_current_bucket_location->GetHeader());
    ui->nameLabel->show();

    ui->pobTooltipButton->setEnabled(false);
}

// The url an item's image is fetched from: a relative icon is served by the CDN.
static QString itemImageUrl(const Item &item)
{
    const QString icon = item.icon();
    if (icon.startsWith('/')) {
        return POE_WEBCDN + icon;
    }
    return icon;
}

void MainWindow::UpdateCurrentItem()
{
    spdlog::trace("MainWindow::UpdateCurrentItem() entered");
    if (m_current_item == nullptr) {
        ClearCurrentItem();
        return;
    }

    ui->imageLabel->show();
    ui->minimapLabel->show();
    ui->locationLabel->show();
    ui->itemTooltipWidget->show();
    ui->itemButtonsWidget->show();
    ui->nameLabel->hide();

    ui->imageLabel->setText("Loading...");
    ui->imageLabel->setStyleSheet("QLabel { background-color : rgb(12, 12, 43); color: white }");
    ui->imageLabel->setFixedSize(QSize(m_current_item->w(), m_current_item->h()) * PIXELS_PER_SLOT);

    // Everything except item image now lives in itemtooltip.cpp
    // in future should move everything tooltip-related there
    UpdateItemTooltip(*m_current_item, ui);

    // The location line renders through the canonical inventory (S4
    // review round 1): a metadata delta renames a tab without replacing
    // the items fetched from it, so the embedded location can be stale.
    ui->locationLabel->setText(
        m_items_manager.locationInventory().Canonical(m_current_item->location()).GetHeader());
    ui->pobTooltipButton->setEnabled(m_current_item->Wearable());

    emit GetImage(itemImageUrl(*m_current_item));
}

void MainWindow::OnImageFetched(const QString &url)
{
    if (m_current_item) {
        if (url == itemImageUrl(*m_current_item)) {
            const QImage image = m_image_cache.load(url);
            if (image.isNull()) {
                ui->imageLabel->setText("No image");
            } else {
                const QPixmap pixmap = GenerateItemIcon(*m_current_item, image);
                ui->imageLabel->setPixmap(pixmap);
            }
        }
    }
}

void MainWindow::UpdateBuyoutWidgets(const Buyout &bo)
{
    spdlog::trace("MainWindow::UpdateBuyoutWidgets() entered");
    ui->buyoutTypeComboBox->setCurrentIndex(bo.type);
    ui->buyoutTypeComboBox->setEnabled(!bo.IsGameSet());
    ui->buyoutCurrencyComboBox->setEnabled(false);
    ui->buyoutValueLineEdit->setEnabled(false);

    if (bo.IsPriced()) {
        ui->buyoutCurrencyComboBox->setCurrentIndex(bo.currency.type);
        ui->buyoutValueLineEdit->setText(QString::number(bo.value, 'g', 15));
        if (!bo.IsGameSet()) {
            ui->buyoutCurrencyComboBox->setEnabled(true);
            ui->buyoutValueLineEdit->setEnabled(true);
        }
    } else {
        ui->buyoutValueLineEdit->setText("");
    }
}

void MainWindow::UpdateCurrentBuyout()
{
    spdlog::trace("MainWindow::UpdateCurrentBuyout() entered");
    if (m_current_item) {
        UpdateBuyoutWidgets(m_buyout_manager.Get(*m_current_item));
    } else if (m_current_bucket_location
               && (m_current_bucket_location->IsFolder() || !m_current_bucket_location->IsValid())) {
        // No buyouts on folders (D11, R7): the widgets are disabled, and a
        // tab buyout an earlier version stored against the folder id is
        // not rendered. The By-Item view's single header row has no
        // location at all and is treated the same way.
        ResetBuyoutWidgets();
    } else if (m_current_bucket_location) {
        UpdateBuyoutWidgets(m_buyout_manager.GetTab(*m_current_bucket_location));
    } else {
        ResetBuyoutWidgets();
    }
}

void MainWindow::ResetBuyoutWidgets()
{
    UpdateBuyoutWidgets(Buyout());
    ui->buyoutTypeComboBox->setEnabled(false);
    ui->buyoutCurrencyComboBox->setCurrentIndex(Currency::CURRENCY_NONE);
}

void MainWindow::OnItemsRefreshed(bool initial_refresh)
{
    spdlog::trace("MainWindow::OnItemsRefreshed() entered");
    // Background searches keep rule 1 at the snapshot boundary too
    // (R1-7): the snapshot mutates published state no delta expressed
    // (deleted tabs, new listings, the location rebase), so every
    // background search is flagged now and its own next activation
    // refilters — never an eager background refilter here.
    for (const auto &search : m_searches) {
        if (search.get() != m_current_search) {
            search->setItemsDirty(true);
        }
    }
    if (!m_current_search) {
        m_refresh_active = false;
        return;
    }
    if (initial_refresh) {
        // Initial population (D6): nothing to preserve — this is the one
        // refresh boundary where the reset path stays legitimate.
        m_current_search->SetRefreshReason(RefreshReason::ItemsChanged);
        ModelViewRefresh();
        m_refresh_active = false;
        return;
    }

    // R1-2 (S6): the active search performs one authoritative row
    // reconciliation against the post-snapshot published state — row
    // operations only, never a reset (noModelResetDuringRefresh).
    const SelectionCapture capture = CaptureSelection(std::nullopt);
    m_applying_delta = true;
    const auto result = m_current_search->ReconcileFinalSnapshot(m_items_manager.items());
    m_applying_delta = false;
    // Buckets the reconciliation inserted expand by the same rules as a
    // delta's inserted buckets.
    ExpandInsertedBuckets(result.inserted_bucket_rows);
    for (size_t i = 0; i < m_searches.size(); ++i) {
        if (m_searches[i].get() == m_current_search) {
            m_tab_bar->setTabText(static_cast<int>(i), m_current_search->GetCaption());
            break;
        }
    }
    if (result.model_changed) {
        ScheduleResizeTreeColumns();
    }
    ReconcileSelectionIntent(&capture);
    RestoreSelection(capture);
    m_carried_ids.clear();
    // The success-boundary intent closure (R1-3): the intent pass above
    // adopted an id that reappeared only in the final snapshot; one whose
    // id is absent is cleared so it cannot reselect in a later refresh.
    if (!m_selection_intent_id.isEmpty()
        && !m_current_search->visibleItemById(m_selection_intent_id)) {
        m_selection_intent_id.clear();
    }
    m_refresh_active = false;
}

void MainWindow::OnSetShopThreads()
{
    bool ok;
    QString thread = QInputDialog::getText(
        this,
        "Shop thread",
        "Enter thread number. You can enter multiple shops by separating them with a comma. More "
        "than one shop may be needed if you have a lot of items.",
        QLineEdit::Normal,
        m_shop.threads().join(","),
        &ok);
    // A confirmed empty input clears the threads; SkipEmptyParts keeps
    // stray commas or a blank box from storing an empty thread id (F45).
    if (ok) {
        static const auto spaces = QRegularExpression("\\s+");
        m_shop.SetThread(thread.remove(spaces).split(',', Qt::SkipEmptyParts));
    }
    UpdateShopMenu();
}

void MainWindow::OnShowPOESESSID()
{
    static QInputDialog *dialog = nullptr;

    // Create and configure the input dialog.
    if (!dialog) {
        dialog = new QInputDialog(this);
        dialog->setWindowTitle("Path of Exile - Session ID");
        dialog->setLabelText("POESESSID:");
        dialog->setInputMode(QInputDialog::TextInput);
        auto lineEdit = dialog->findChild<QLineEdit *>();
        if (lineEdit) {
            // Use a fixed width font for the input, and set it to be exactly
            // as wide as a POESESSID cookie.
            const QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            const QFontMetrics metric(font);
            const int w = metric.horizontalAdvance("00000000000000000000000000000000");
            lineEdit->setFont(font);
            lineEdit->setMinimumWidth(w);
        }
    }

    // Load the session_id if it exists.
    dialog->setTextValue(m_settings.value("session_id").toString());

    // Get the user input and set the session cookie.
    int code = dialog->exec();
    if (code == QDialog::DialogCode::Accepted) {
        const QString poesessid = dialog->textValue();
        emit SetSessionId(poesessid);
    }
}

void MainWindow::UpdateShopMenu()
{
    QString title = "Forum shop thread...";
    if (!m_shop.threads().empty()) {
        title += " [" + m_shop.threads().join(",") + "]";
    }
    ui->actionSetShopThreads->setText(title);
    ui->actionSetAutomaticallyShopUpdate->setChecked(m_shop.auto_update());
}

void MainWindow::OnUpdateAvailable(bool available)
{
    m_update_button.setVisible(available);
}

void MainWindow::OnCopyShopToClipboard()
{
    m_shop.CopyToClipboard();
}

void MainWindow::OnSetTabRefreshInterval()
{
    // 30 is ItemsManager's default when the setting has never been written.
    // The maximum is a day, raised to the current value if that is larger so
    // that accepting the dialog unchanged never lowers it.
    constexpr int kMaximumMinutes = 24 * 60;
    const int current = std::max(1, m_settings.value("autoupdate_interval", 30).toInt());
    bool ok = false;
    const int interval = QInputDialog::getInt(this,
                                              "Auto refresh items",
                                              "Refresh items every X minutes",
                                              current,
                                              1,
                                              std::max(kMaximumMinutes, current),
                                              1,
                                              &ok);
    if (ok) {
        m_items_manager.SetAutoUpdateInterval(interval);
    }
}

void MainWindow::OnFetchTabsList()
{
    m_items_manager.Update(TabSelection::TabsOnly);
}

void MainWindow::OnRefreshAllTabs()
{
    m_items_manager.Update(TabSelection::All);
}

void MainWindow::OnRefreshCheckedTabs()
{
    m_items_manager.Update(TabSelection::Checked);
}

void MainWindow::OnSetAutomaticTabRefresh()
{
    m_items_manager.SetAutoUpdate(ui->actionSetAutomaticTabRefresh->isChecked());
}

void MainWindow::OnUpdateShops()
{
    m_shop.SubmitShopToForum(true);
}

void MainWindow::OnEditShopTemplate()
{
    bool ok;
    QString text = QInputDialog::getMultiLineText(
        this,
        "Shop template",
        "Enter shop template. [items] will be replaced with the list of items you marked for sale.",
        m_shop.shop_template(),
        &ok);
    if (ok && !text.isEmpty()) {
        m_shop.SetShopTemplate(text);
    }
}

void MainWindow::OnSetAutomaticShopUpdate()
{
    m_shop.SetAutoUpdate(ui->actionSetAutomaticallyShopUpdate->isChecked());
}

void MainWindow::OnSetDarkTheme(bool toggle)
{
    if (toggle) {
        emit SetTheme("dark");
        m_settings.setValue("theme", "dark");
    }
}

void MainWindow::OnSetLightTheme(bool toggle)
{
    if (toggle) {
        emit SetTheme("light");
        m_settings.setValue("theme", "light");
    }
}

void MainWindow::OnSetDefaultTheme(bool toggle)
{
    if (toggle) {
        emit SetTheme("default");
        m_settings.setValue("theme", "default");
    }
}

void MainWindow::OnSetLogging(spdlog::level::level_enum level)
{
    spdlog::set_level(level);
    ui->actionLoggingOFF->setChecked(level == spdlog::level::off);
    ui->actionLoggingFATAL->setChecked(level == spdlog::level::critical);
    ui->actionLoggingERROR->setChecked(level == spdlog::level::err);
    ui->actionLoggingWARN->setChecked(level == spdlog::level::warn);
    ui->actionLoggingINFO->setChecked(level == spdlog::level::info);
    ui->actionLoggingDEBUG->setChecked(level == spdlog::level::debug);
    ui->actionLoggingTRACE->setChecked(level == spdlog::level::trace);
    const QString level_name = to_qstring(level);
    spdlog::info("Logging level set to {}", level_name);
    m_settings.setValue("log_level", level_name);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_quitting) {
        event->accept();
        return;
    }

    QMessageBox msgbox(this);
    msgbox.setWindowTitle("Acquisition");
    msgbox.setText(tr("Are you sure you want to quit?"));
    msgbox.setStandardButtons(QMessageBox::No | QMessageBox::Yes);
    msgbox.setDefaultButton(QMessageBox::Yes);

    const auto button = msgbox.exec();
    if (button == QMessageBox::Yes) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::OnUploadToImgur()
{
    ui->uploadTooltipButton->setDisabled(true);
    ui->uploadTooltipButton->setText("Uploading...");

    QPixmap pixmap(ui->itemTooltipWidget->size());
    ui->itemTooltipWidget->render(&pixmap);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG"); // writes pixmap into bytes in PNG format

    QNetworkRequest request(QUrl("https://api.imgur.com/3/upload/"));
    request.setRawHeader("Authorization", "Client-ID d6d2d8a0437a90f");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(kImgurUploadTimeout);
    QByteArray image_data = "image=" + QUrl::toPercentEncoding(bytes.toBase64());
    QNetworkReply *reply = m_network_manager.post(request, image_data);
    connect(reply, &QNetworkReply::finished, this, &MainWindow::OnUploadFinished);
}

void MainWindow::OnCopyForPOB()
{
    if (m_current_item == nullptr) {
        return;
    }
    // if category isn't wearable, including flasks, don't do anything
    if (!m_current_item->Wearable()) {
        spdlog::warn("{}, category: {}, should not have been exportable.",
                     m_current_item->PrettyName(),
                     m_current_item->category());
        return;
    }

    QApplication::clipboard()->setText(m_current_item->POBformat());
    spdlog::info("{} was copied to your clipboard in Path of Building's \"Create custom\" format.",
                 m_current_item->PrettyName());
}

void MainWindow::OnUploadFinished()
{
    ui->uploadTooltipButton->setDisabled(false);
    ui->uploadTooltipButton->setText("Upload to imgur");

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("Imgur upload failed: {}", reply->errorString());
        return;
    }

    const auto result = ParseImgurUploadReply(bytes);
    if (!result) {
        spdlog::error("Imgur upload failed: {}", result.error());
        return;
    }

    const QString url = *result;
    QApplication::clipboard()->setText(url);
    spdlog::info("Image uploaded to '{}' and the URL has been copied to your clipboard.", url);
}
