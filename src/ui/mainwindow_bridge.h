// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

class QObject;
class QDir;
class QSettings;

class Application;
class BuyoutManager;
class BuyoutRepo;
class CharacterRepo;
class CurrencyManager;
class DataStore;
class ImageCache;
class ItemsManager;
class ItemsManagerWorker;
class MainWindow;
class NetworkManager;
class RateLimiter;
class Shop;
class StashRepo;
class UpdateChecker;

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
                             const QDir &app_data_dir);

void ConnectMainWindow(Application &application,
                       MainWindow &main_window,
                       ItemsManager &items_manager,
                       ItemsManagerWorker &items_worker,
                       Shop &shop,
                       UpdateChecker &update_checker,
                       ImageCache &image_cache);
// The update checker's half of ConnectMainWindow.
void ConnectUpdateChecker(MainWindow &main_window, UpdateChecker &update_checker);
void ShowMainWindow(MainWindow &window);
