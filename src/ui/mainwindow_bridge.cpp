// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/mainwindow_bridge.h"

#include "application.h"
#include "imagecache.h"
#include "itemsmanager.h"
#include "itemsmanagerworker.h"
#include "shop.h"
#include "ui/mainwindow.h"
#include "util/updatechecker.h"

void MainWindowDeleter::operator()(MainWindow *window) const
{
    delete window;
}

MainWindow *CreateMainWindow(QSettings &settings,
                             NetworkManager &network_manager,
                             RateLimiter &rate_limiter,
                             DataStore &datastore,
                             ItemsManager &items_manager,
                             BuyoutManager &buyout_manager,
                             BuyoutRepo &buyout_repo,
                             StashRepo &stash_repo,
                             CharacterRepo &character_repo,
                             CurrencyManager &currency_manager,
                             Shop &shop,
                             ImageCache &image_cache,
                             const QDir &app_data_dir)
{
    return new MainWindow(settings,
                          network_manager,
                          rate_limiter,
                          datastore,
                          items_manager,
                          buyout_manager,
                          buyout_repo,
                          stash_repo,
                          character_repo,
                          currency_manager,
                          shop,
                          image_cache,
                          app_data_dir);
}

void ConnectMainWindow(Application &application,
                       MainWindow &main_window,
                       ItemsManager &items_manager,
                       ItemsManagerWorker &items_worker,
                       Shop &shop,
                       UpdateChecker &update_checker,
                       ImageCache &image_cache)
{
    QObject::connect(&main_window,
                     &MainWindow::SetSessionId,
                     &application,
                     &Application::SetSessionId);
    QObject::connect(&main_window, &MainWindow::SetTheme, &application, &Application::SetTheme);
    ConnectUpdateChecker(main_window, update_checker);

    QObject::connect(&items_manager,
                     &ItemsManager::ItemsRefreshed,
                     &main_window,
                     &MainWindow::OnItemsRefreshed);
    // Streamed deltas (items-pipeline M2 D3, M3 D3/D4): the manager's light
    // re-emits apply synchronously as bucket-scoped model operations on the
    // current search; background searches are marked items-dirty.
    QObject::connect(&items_manager,
                     &ItemsManager::TabRefreshed,
                     &main_window,
                     &MainWindow::OnTabRefreshed);
    QObject::connect(&items_manager,
                     &ItemsManager::ChildrenReconciled,
                     &main_window,
                     &MainWindow::OnChildrenReconciled);
    // The typed terminal event (M3 S4): closes the selection-intent
    // window on every outcome, terminal failure included (R1-3/R2-1).
    QObject::connect(&items_manager,
                     &ItemsManager::RefreshFinished,
                     &main_window,
                     &MainWindow::OnRefreshFinished);
    QObject::connect(&items_manager,
                     &ItemsManager::StatusUpdate,
                     &main_window,
                     &MainWindow::OnStatusUpdate);
    QObject::connect(&items_worker,
                     &ItemsManagerWorker::NotifyUser,
                     &main_window,
                     &MainWindow::OnNotifyUser);

    QObject::connect(&main_window, &MainWindow::GetImage, &image_cache, &ImageCache::fetch);
    QObject::connect(&image_cache,
                     &ImageCache::imageReady,
                     &main_window,
                     &MainWindow::OnImageFetched);

    QObject::connect(&shop, &Shop::StatusUpdate, &main_window, &MainWindow::OnStatusUpdate);
    QObject::connect(&shop,
                     &Shop::UserWarning,
                     &main_window,
                     &MainWindow::OnShopWarning,
                     Qt::QueuedConnection);
}

void ConnectUpdateChecker(MainWindow &main_window, UpdateChecker &update_checker)
{
    // The "Update available" button is shown only once a check has found a
    // newer version, so a click asks the user what to do about it. The
    // periodic check runs on the UpdateChecker's own timer.
    QObject::connect(&main_window,
                     &MainWindow::UpdateCheckRequested,
                     &update_checker,
                     &UpdateChecker::AskUserToUpdate);
    QObject::connect(&update_checker,
                     &UpdateChecker::UpdateAvailable,
                     &main_window,
                     &MainWindow::OnUpdateAvailable);
}

void ShowMainWindow(MainWindow &window)
{
    window.show();
}
