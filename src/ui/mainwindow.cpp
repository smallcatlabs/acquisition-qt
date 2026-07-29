// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/mainwindow.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QImageReader>
#include <QInputDialog>
#include <QLayout>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer> // SPIKE(S1-M2)
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar> // SPIKE(S1-M2)
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTabBar>
#include <QVersionNumber>

#include <set>
#include <utility>
#include <vector>

#include "buyoutmanager.h"
#include "currencymanager.h"
#include "datastore/datastore.h"
#include "imagecache.h"
#include "item.h"
#include "itemconstants.h"
#include "itemlocation.h"
#include "items_model.h"
#include "itemsmanager.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitdialog.h"
#include "ratelimit/ratelimiter.h"
#include "replytimeout.h"
#include "search.h"
#include "shop.h"
#include "ui/currencydialog.h"
#include "ui/itemtooltip.h"
#include "ui/logpanel.h"
#include "ui/searchform.h"
#include "ui/verticalscrollarea.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/networkmanager.h"
#include "util/spdlog_qt.h"
#include "util/updatechecker.h"
#include "util/util.h"
#include "version_defines.h"

constexpr const char *POE_WEBCDN
    = "http://webcdn.pathofexile.com"; // Should be updated to https://web.poecdn.com ?

constexpr int CURRENT_ITEM_UPDATE_DELAY_MS = 100;
constexpr int SEARCH_UPDATE_DELAY_MS = 350;

// SPIKE(S1-M2): provisional S = 60 s (D9 rule 2), injectable via
// SpikeSetThrottleInterval and the settings key below.
constexpr int SPIKE_THROTTLE_DEFAULT_MS = 60'000;
constexpr const char *SPIKE_THROTTLE_SETTINGS_KEY = "spike_s1m2_throttle_ms";

struct ImgurStatus
{
    struct Link
    {
        QString link;
    };

    int status;
    ImgurStatus::Link data;
};

MainWindow::MainWindow(QSettings &settings,
                       NetworkManager &network_manager,
                       RateLimiter &rate_limiter,
                       DataStore &datastore,
                       ItemsManager &items_manager,
                       BuyoutManager &buyout_manager,
                       CurrencyManager &currency_manager,
                       Shop &shop,
                       ImageCache &image_cache)
    : m_settings(settings)
    , m_network_manager(network_manager)
    , m_rate_limiter(rate_limiter)
    , m_datastore(datastore)
    , m_items_manager(items_manager)
    , m_buyout_manager(buyout_manager)
    , m_currency_manager(currency_manager)
    , m_shop(shop)
    , m_image_cache(image_cache)
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

    // SPIKE(S1-M2): single-shot, started by the first intersecting delta and
    // never re-armed by later arrivals (the non-resetting half of D9 rule 2).
    m_spike_throttle.setInterval(
        m_settings.value(SPIKE_THROTTLE_SETTINGS_KEY, SPIKE_THROTTLE_DEFAULT_MS).toInt());
    m_spike_throttle.setSingleShot(true);
    connect(&m_spike_throttle, &QTimer::timeout, this, &MainWindow::SpikeThrottleFired);

    LoadSettings();
    NewSearch();
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
    connect(&m_rate_limiter, &RateLimiter::Paused, this, [=](int pause) {
        if (pause > 0) {
            button->setText("Rate limited for " + QString::number(pause) + " seconds");
            button->setStyleSheet("font-weight: bold; color: red");
        } else if (pause == 0) {
            button->setText("Rate limiting is OFF");
            button->setStyleSheet("");
        } else {
            button->setText("ERROR: pause is " + QString::number(pause));
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
            RestoreViewExpansion(*m_current_search);
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

    m_refresh_button.setStyleSheet("color: blue; font-weight: bold;");
    m_refresh_button.setFlat(true);
    m_refresh_button.hide();
    statusBar()->addPermanentWidget(&m_refresh_button);
    connect(&m_refresh_button, &QPushButton::clicked, this, &MainWindow::OnRefreshAllTabs);

    m_update_button.setText("Update available");
    m_update_button.setStyleSheet("color: blue; font-weight: bold;");
    m_update_button.setFlat(true);
    m_update_button.hide();
    statusBar()->addPermanentWidget(&m_update_button);
    connect(&m_update_button, &QPushButton::clicked, this, [=, this]() {
        emit UpdateCheckRequested();
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

    // Connect the Theme submenu.
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

    // Connect the Tooltip tab buttons
    connect(ui->uploadTooltipButton, &QPushButton::clicked, this, &MainWindow::OnUploadToImgur);
    connect(ui->pobTooltipButton, &QPushButton::clicked, this, &MainWindow::OnCopyForPOB);

    // Connect the currency actions.
    connect(ui->actionListCurrency, &QAction::triggered, this, &MainWindow::OnListCurrency);
    connect(ui->actionExportCurrency, &QAction::triggered, this, &MainWindow::OnExportCurrency);
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

void MainWindow::OnExpandAll()
{
    spdlog::trace("MainWindow::OnExpandAll() entered");
    // Only need to expand the top level, which corresponds to buckets,
    // aka stash tabs and characters. The expanded() signals emitted during
    // this operation coalesce into a single deferred column resize.
    setCursor(Qt::WaitCursor);
    ui->treeView->expandToDepth(0);
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
    setCursor(Qt::WaitCursor);
    const auto &model = *ui->treeView->model();
    const int rowCount = model.rowCount();
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex idx = model.index(row, 0, QModelIndex());
        ui->treeView->collapse(idx);
    }
    ScheduleResizeTreeColumns();
    unsetCursor();
}

void MainWindow::OnCheckAll()
{
    spdlog::trace("MainWindow::OnCheckAll() entered");
    for (auto const &bucket : m_current_search->buckets()) {
        m_buyout_manager.SetRefreshChecked(bucket.location(), true);
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::OnUncheckAll()
{
    spdlog::trace("MainWindow::OnUncheckAll() entered");
    for (auto const &bucket : m_current_search->buckets()) {
        m_buyout_manager.SetRefreshChecked(bucket.location(), false);
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::OnRefreshSelected()
{
    spdlog::trace("MainWindow::OnRefreshSelected()");
    // Get names of tabs to refresh
    std::vector<ItemLocation> locations;
    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (auto const &index : selected_rows) {
        // Fetch tab names per index
        locations.emplace_back(m_current_search->GetTabLocation(index));
    }
    m_items_manager.Update(TabSelection::Selected, locations);
}

void MainWindow::CheckSelected(bool value)
{
    spdlog::trace("MainWindow::CheckSelected() entered");
    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (auto const &index : selected_rows) {
        m_buyout_manager.SetRefreshChecked(m_current_search->GetTabLocation(index), value);
    }
    static_cast<ItemsModel *>(ui->treeView->model())->refreshCheckStates();
}

void MainWindow::ResizeTreeColumns()
{
    spdlog::trace("MainWindow::ResizeTreeColumns() entered");
    for (int i = 0; i < ui->treeView->header()->count(); ++i) {
        ui->treeView->resizeColumnToContents(i);
    }
}

void MainWindow::ScheduleResizeTreeColumns()
{
    m_delayed_resize_columns.start();
}

void MainWindow::OnBuyoutChange()
{
    spdlog::trace("MainWindow::OnBuyoutChange() entered");
    m_shop.ExpireShopData();

    Buyout bo;
    bo.type = Buyout::IndexAsBuyoutType(ui->buyoutTypeComboBox->currentIndex());
    bo.currency = Currency::FromIndex(ui->buyoutCurrencyComboBox->currentIndex());
    bo.value = ui->buyoutValueLineEdit->text().replace(',', ".").toDouble();
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

    // Don't assign a zero buyout if nothing is entered in the value textbox
    if (ui->buyoutValueLineEdit->text().isEmpty() && bo.IsPriced()) {
        spdlog::trace("MainWindow::OnBuyoutChange() buyout iempty");
        return;
    }

    const auto &selected_rows = ui->treeView->selectionModel()->selectedRows();
    for (const auto &index : selected_rows) {
        const auto location = m_current_search->GetTabLocation(index);
        const auto tab = location.id();

        // Don't allow users to manually update locked tabs (game priced)
        if (m_buyout_manager.GetTab(location).IsGameSet()) {
            spdlog::trace("MainWindow::OnBuyoutChange() refusing to update locked tab: {}", tab);
            continue;
        }
        if (!index.parent().isValid()) {
            m_buyout_manager.SetTab(location, bo);
        } else {
            const int bucket_row = index.parent().row();
            if (m_current_search->has_bucket(bucket_row)) {
                const Bucket &bucket = m_current_search->bucket(bucket_row);
                const int item_row = index.row();
                if (bucket.has_item(item_row)) {
                    const Item &item = *bucket.item(item_row);
                    // Don't allow users to manually update locked items (game priced per item in note section)
                    if (m_buyout_manager.Get(item).IsGameSet()) {
                        spdlog::trace(
                            "MainWindow::OnBuyoutChange() refusing to update locked item: {}",
                            item.name());
                        continue;
                    }
                    m_buyout_manager.Set(item, bo);
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

void MainWindow::OnSearchFormChange()
{
    spdlog::trace("MainWindow::OnSearchFormChange() entered");
    SaveViewExpansion(*m_current_search);
    m_current_search->SetRefreshReason(RefreshReason::SearchFormChanged);
    ModelViewRefresh();
}

// SPIKE(S1-M2): expansion is keyed by the stable (type, id) key instead of
// the mutable header text, so a renamed tab keeps its expansion (R6-3 (1)).
void MainWindow::SaveViewExpansion(Search &search)
{
    std::set<QString> expanded;
    if (!search.defaultExpanded()) {
        const int rows = search.model().rowCount();
        for (int row = 0; row < rows; ++row) {
            const QModelIndex index = search.model().index(row, 0);
            if (index.isValid() && ui->treeView->isExpanded(index) && search.has_bucket(row)) {
                expanded.emplace(search.bucket(row).location().stableKey());
            }
        }
    }
    search.setExpandedHeaders(std::move(expanded));
}

void MainWindow::RestoreViewExpansion(Search &search)
{
    if (search.defaultExpanded()) {
        ui->treeView->expandToDepth(0);
        return;
    }
    const auto &headers = search.expandedHeaders();
    const int rows = search.model().rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = search.model().index(row, 0);
        if (headers.empty()) {
            ui->treeView->collapse(index);
        } else if (headers.count(search.bucket(row).location().stableKey()) > 0) {
            ui->treeView->expand(index);
        } else {
            ui->treeView->collapse(index);
        }
    }
}

void MainWindow::ModelViewRefresh()
{
    spdlog::trace("MainWindow::ModelViewRefresh() entered");

    // SPIKE(S1-M2): any refresh of the shown view retires the current
    // search's pending tick — a successful refilter pays for the work
    // (R5-5), and a tab switch hands the update to the dirty flag (rule 4).
    m_spike_throttle.stop();

    disconnect(m_current_item_conn);

    m_buyout_manager.Save();

    spdlog::trace("MainWindow::ModelViewRefresh() activating current search");
    m_search_form->saveTo(*m_current_search);
    m_current_search->FilterItems(m_items_manager.items());
    ItemsModel &model = m_current_search->model();
    ui->treeView->setSortingEnabled(false);
    if (ui->treeView->model() != &model) {
        ui->treeView->setModel(&model);
    }
    ui->treeView->header()->setSortIndicator(model.GetSortColumn(), model.GetSortOrder());
    ui->treeView->setSortingEnabled(true);
    RestoreViewExpansion(*m_current_search);
    ScheduleResizeTreeColumns();

    // This updates the item information when current item changes.
    m_current_item_conn = connect(ui->treeView->selectionModel(),
                                  &QItemSelectionModel::currentChanged,
                                  this,
                                  &MainWindow::OnCurrentItemChanged);

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
}

void MainWindow::OnCurrentItemChanged(const QModelIndex &current, const QModelIndex &previous)
{
    Q_UNUSED(previous);
    spdlog::trace("MainWindow::OnCurrentItemChange() entered");
    m_buyout_manager.Save();

    if (!current.isValid()) {
        return;
    }

    if (current.parent().isValid()) {
        // Clicked on an item. The warning branches reset the stored selection
        // instead of keeping the previous item/bucket pair, so the panel
        // clears rather than rendering stale state (F44).
        const int bucket_row = current.parent().row();
        if (m_current_search->has_bucket(bucket_row)) {
            const Bucket &bucket = m_current_search->bucket(bucket_row);
            const int item_row = current.row();
            if (bucket.has_item(item_row)) {
                m_current_item = bucket.item(item_row);
                m_current_bucket_location.reset();
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
        const int bucket_row = current.row();
        if (m_current_search->has_bucket(bucket_row)) {
            m_current_bucket_location = m_current_search->bucket(bucket_row).location();
            UpdateCurrentBucket();
        } else {
            spdlog::warn("OnCurrentItemChanged(): bucket {} does not exist", bucket_row);
            m_current_bucket_location.reset();
            ClearCurrentItem();
        }
    }
    if (m_current_item || m_current_bucket_location) {
        UpdateCurrentBuyout();
    } else {
        ResetBuyoutWidgets();
    }
}

void MainWindow::ReselectCurrentItem()
{
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

        // SPIKE(S1-M2): the stable-identity match may have landed on a
        // replacement object for the same item; adopt it so the details
        // panel doesn't keep the superseded copy alive (R6-3 (2)).
        const int bucket_row = index.parent().isValid() ? index.parent().row() : -1;
        if ((bucket_row >= 0) && m_current_search->has_bucket(bucket_row)) {
            const Bucket &bucket = m_current_search->bucket(bucket_row);
            if (bucket.has_item(index.row())) {
                m_current_item = bucket.item(index.row());
            }
        }
    }
}

void MainWindow::ReselectCurrentBucket()
{
    const auto &buckets = m_current_search->buckets();
    for (size_t row = 0; row < buckets.size(); ++row) {
        if (buckets[row].location() == *m_current_bucket_location) {
            spdlog::trace("MainWindow::ReselectCurrentBucket() reselecting the previous bucket");
            const QModelIndex index = m_current_search->model().index(static_cast<int>(row), 0);
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

// SPIKE(S1-M2): D9's five-rule state machine, prototyped for the S1-M2
// cadence spike. Rules 3-5's cancellations live in ModelViewRefresh; the
// items-dirty flags are cleared by each search's own FilterItems.
void MainWindow::OnTabRefreshed(const ItemLocation &location, const Items &delta_items)
{
    // Rule 1: every delta marks every search items-dirty, the current search
    // and non-intersecting deltas included.
    for (auto &search : m_searches) {
        search->setItemsDirty(true);
    }

    // Rule 2: an intersecting delta schedules the current search's throttled
    // refilter; the timer is not re-armed by later arrivals.
    if (!m_current_search || m_spike_throttle.isActive()) {
        return;
    }
    if (SpikeDeltaIntersectsCurrent(location, delta_items)) {
        m_spike_throttle.start();
        spdlog::debug("SPIKE(S1-M2): throttle armed by tab '{}' ({} items), S = {} ms",
                      location.GetHeader(),
                      delta_items.size(),
                      m_spike_throttle.interval());
    }
}

bool MainWindow::SpikeDeltaIntersectsCurrent(const ItemLocation &location,
                                             const Items &delta_items) const
{
    // Half A: any delta item matches the current filter set.
    for (const auto &item : delta_items) {
        if (m_current_search->MatchesActiveFilters(*item)) {
            return true;
        }
    }
    // Half B (removal): any item in the visible filtered result was fetched
    // from this delta's fetch source — an empty or shrunken replacement must
    // count as a visible change.
    return m_current_search->visibleSources().count(location.fetchSourceKey()) > 0;
}

// Rule 3: the tick fires, the current search refilters once, and the R6-3
// fidelity contract runs — expansion, selection, and scroll are captured
// immediately before the reset and restored by stable identity after it.
void MainWindow::SpikeThrottleFired()
{
    if (!m_current_search) {
        return;
    }
    spdlog::debug("SPIKE(S1-M2): throttle fired, refiltering the current search");

    QElapsedTimer timer;
    timer.start();

    // R6-3 (4): capture-before-reset. Selection is already tracked in
    // m_current_item / m_current_bucket_location.
    SaveViewExpansion(*m_current_search);
    const SpikeScrollAnchor anchor = SpikeCaptureScroll();

    m_current_search->SetRefreshReason(RefreshReason::ItemsChanged);
    ModelViewRefresh();

    SpikeRestoreScroll(anchor);

    SpikeResetRecord record;
    record.total_ms = timer.elapsed();
    record.filter_ms = m_current_search->lastFilterMs();
    // Everything that isn't the filter loop + model reset: form save,
    // expansion restore, reselection, and the scroll restore.
    record.restore_ms = record.total_ms - record.filter_ms;
    record.visible_items = static_cast<int>(m_current_search->items().size());
    record.buckets = static_cast<int>(m_current_search->buckets().size());
    spdlog::info("SPIKE(S1-M2): reset applied in {} ms (filter+model {} ms, "
                 "restore {} ms) over {} visible items in {} buckets",
                 record.total_ms,
                 record.filter_ms,
                 record.restore_ms,
                 record.visible_items,
                 record.buckets);
    emit SpikeResetApplied(record);
}

MainWindow::SpikeScrollAnchor MainWindow::SpikeCaptureScroll() const
{
    // R6-3 (3): anchor the top visible row by stable identity, with the raw
    // scrollbar value as the fallback.
    SpikeScrollAnchor anchor;
    anchor.scrollbar_value = ui->treeView->verticalScrollBar()->value();
    const QModelIndex top = ui->treeView->indexAt(QPoint(4, 4));
    if (!top.isValid()) {
        return anchor;
    }
    if (top.parent().isValid()) {
        const int bucket_row = top.parent().row();
        if (m_current_search->has_bucket(bucket_row)) {
            const Bucket &bucket = m_current_search->bucket(bucket_row);
            anchor.bucket_key = bucket.location().stableKey();
            if (bucket.has_item(top.row())) {
                anchor.item_id = bucket.item(top.row())->id();
            }
        }
    } else if (m_current_search->has_bucket(top.row())) {
        anchor.bucket_key = m_current_search->bucket(top.row()).location().stableKey();
    }
    return anchor;
}

void MainWindow::SpikeRestoreScroll(const SpikeScrollAnchor &anchor)
{
    if (!anchor.bucket_key.isEmpty()) {
        const auto &buckets = m_current_search->buckets();
        for (size_t row = 0; row < buckets.size(); ++row) {
            if (buckets[row].location().stableKey() != anchor.bucket_key) {
                continue;
            }
            QModelIndex index = m_current_search->model().index(static_cast<int>(row), 0);
            if (!anchor.item_id.isEmpty()) {
                const auto &items = buckets[row].items();
                for (size_t n = 0; n < items.size(); ++n) {
                    if (items[n]->id() == anchor.item_id) {
                        index = m_current_search->model().index(static_cast<int>(n), 0, index);
                        break;
                    }
                }
            }
            ui->treeView->scrollTo(index, QAbstractItemView::PositionAtTop);
            return;
        }
    }
    ui->treeView->verticalScrollBar()->setValue(anchor.scrollbar_value);
}

void MainWindow::SpikeSetThrottleInterval(int ms)
{
    m_spike_throttle.setInterval(ms);
    m_settings.setValue(SPIKE_THROTTLE_SETTINGS_KEY, ms);
}

int MainWindow::SpikeThrottleInterval() const
{
    return m_spike_throttle.interval();
}

bool MainWindow::SpikeThrottlePending() const
{
    return m_spike_throttle.isActive();
}

int MainWindow::SpikeThrottleRemainingMs() const
{
    return m_spike_throttle.isActive() ? m_spike_throttle.remainingTime() : 0;
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
        m_current_search->setCurrentItem(m_current_item);
        m_current_search->setCurrentBucket(m_current_bucket_location);
    }
    if (static_cast<size_t>(index) == m_searches.size()) {
        // "+" clicked
        NewSearch();
    } else {
        m_current_search = m_searches[index].get();
        m_current_item = m_current_search->currentItem();
        m_current_bucket_location = m_current_search->currentBucket();
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
    auto search = std::make_unique<Search>(m_buyout_manager, caption, m_filter_catalog);
    m_current_search = search.get();
    m_current_item = m_current_search->currentItem();
    m_current_bucket_location = m_current_search->currentBucket();
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
    ui->imageLabel->hide();
    ui->minimapLabel->hide();
    ui->locationLabel->hide();
    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    ui->nameLabel->setText(m_current_bucket_location->GetHeader());
    ui->nameLabel->show();

    ui->pobTooltipButton->setEnabled(false);
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

    ui->locationLabel->setText(m_current_item->location().GetHeader());
    ui->pobTooltipButton->setEnabled(m_current_item->Wearable());

    QString icon = m_current_item->icon();
    if ((icon.size() >= 1) && (icon[0] == '/')) {
        icon = POE_WEBCDN + icon;
    }
    emit GetImage(icon);
}

void MainWindow::OnImageFetched(const QString &url)
{
    if (m_current_item) {
        const QString icon = m_current_item->icon();
        if (url == icon) {
            const QImage image = m_image_cache.load(url);
            if (!image.isNull()) {
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
        ui->buyoutValueLineEdit->setText(QString::number(bo.value));
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

void MainWindow::OnItemsRefreshed()
{
    spdlog::trace("MainWindow::OnItemsRefreshed() entered");
    int tab = 0;
    for (const auto &search : m_searches) {
        search->SetRefreshReason(RefreshReason::ItemsChanged);
        // Don't update current search - it will be updated in OnSearchFormChange
        if (search.get() != m_current_search) {
            search->FilterItems(m_items_manager.items());
            m_tab_bar->setTabText(tab, search->GetCaption());
        }
        tab++;
    }
    ModelViewRefresh();
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

void MainWindow::OnUpdateAvailable()
{
    m_update_button.show();
}

void MainWindow::OnCopyShopToClipboard()
{
    m_shop.CopyToClipboard();
}

void MainWindow::OnSetTabRefreshInterval()
{
    int interval = QInputDialog::getInt(this,
                                        "Auto refresh items",
                                        "Refresh items every X minutes",
                                        QLineEdit::Normal,
                                        m_settings.value("autoupdate_interval").toInt());
    if (interval > 0) {
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
        ui->actionSetLightTheme->setChecked(false);
        ui->actionSetDefaultTheme->setChecked(false);
        m_settings.setValue("theme", "dark");
    }
    ui->actionSetDarkTheme->setChecked(toggle);
}

void MainWindow::OnSetLightTheme(bool toggle)
{
    if (toggle) {
        emit SetTheme("light");
        ui->actionSetDarkTheme->setChecked(false);
        ui->actionSetDefaultTheme->setChecked(false);
        m_settings.setValue("theme", "light");
    }
    ui->actionSetLightTheme->setChecked(toggle);
}

void MainWindow::OnSetDefaultTheme(bool toggle)
{
    if (toggle) {
        emit SetTheme("default");
        ui->actionSetDarkTheme->setChecked(false);
        ui->actionSetLightTheme->setChecked(false);
        m_settings.setValue("theme", "default");
    }
    ui->actionSetDefaultTheme->setChecked(toggle);
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
    QByteArray bytes = reply->readAll();
    reply->deleteLater();

    ImgurStatus result;

    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const std::string_view sv{bytes, size_t(bytes.size())};
    const auto ec = glz::read<permissive>(result, sv);
    if (!ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Error parsing Imgur result: {}", msg);
        return;
    }

    if (result.status != 200) {
        spdlog::error("Imgur API returned status != 200: {}", bytes);
        return;
    }

    const QString url = result.data.link;
    QApplication::clipboard()->setText(url);
    spdlog::info("Image uploaded to '{}' and the URL has been copied to your clipboard.", url);
}
