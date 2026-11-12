// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <memory>

#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <spdlog/spdlog.h>

#include "currencymanager.h"
#include "imagecache.h"
#include "itemcategories.h"
#include "itemsmanager.h"
#include "poe/poeapiclient.h"
#include "ratelimit/ratelimiter.h"
#include "shop.h"
#include "testfixtures.h"
#include "ui/mainwindow.h"
#include "util/networkmanager.h"

inline void InitializeMainWindowTestCategories()
{
    // SearchForm materializes the Category choices during MainWindow
    // construction. The category tables are process-global, so initialize
    // them exactly once for this test binary.
    static const bool loaded = [] {
        InitItemClasses(R"json({"TestClass":{"name":"Weapons"}})json");
        InitItemBaseTypes(
            R"json({"Metadata/Items/TestSword":{"item_class":"TestClass","name":"Test Sword","release_state":"released"}})json");
        return true;
    }();
    Q_UNUSED(loaded);
}

class MainWindowFixture
{
public:
    MainWindowFixture()
    {
        // NetworkManager creates a disk cache in AppLocalDataLocation.
        QStandardPaths::setTestModeEnabled(true);
        InitializeMainWindowTestCategories();
        if (!spdlog::get("main")) {
            qFatal("MainWindowFixture: the 'main' logger was not registered");
        }

        settings = std::make_unique<QSettings>(tempDir.filePath("settings.ini"),
                                               QSettings::IniFormat);
        stashRepo = std::make_unique<StashRepo>(*buyoutFixture.db);
        characterRepo = std::make_unique<CharacterRepo>(*buyoutFixture.db);
        if (!stashRepo->ensureSchema() || !characterRepo->ensureSchema()) {
            qFatal("MainWindowFixture: failed to create current-store schema");
        }
        networkManager = std::make_unique<NetworkManager>();
        rateLimiter = std::make_unique<RateLimiter>(*networkManager);
        api = std::make_unique<PoeApiClient>(*rateLimiter);
        itemsManager = std::make_unique<ItemsManager>(*settings, *buyoutFixture.manager);
        currencyManager = std::make_unique<CurrencyManager>(*settings,
                                                            *buyoutFixture.data,
                                                            *itemsManager);
        shop = std::make_unique<Shop>(*settings,
                                      *networkManager,
                                      *api,
                                      *buyoutFixture.data,
                                      *itemsManager,
                                      *buyoutFixture.manager);
        imageCache = std::make_unique<ImageCache>(*networkManager, tempDir.filePath("cache"));
        window = std::make_unique<MainWindow>(*settings,
                                              *networkManager,
                                              *rateLimiter,
                                              *itemsManager,
                                              *buyoutFixture.manager,
                                              *buyoutFixture.repo,
                                              *stashRepo,
                                              *characterRepo,
                                              *currencyManager,
                                              *shop,
                                              *imageCache,
                                              QDir(tempDir.path()));

        QObject::connect(itemsManager.get(),
                         &ItemsManager::ItemsRefreshed,
                         window.get(),
                         &MainWindow::OnItemsRefreshed);
        // Streamed deltas (M2 D9), wired the way ConnectMainWindow does.
        QObject::connect(itemsManager.get(),
                         &ItemsManager::TabRefreshed,
                         window.get(),
                         &MainWindow::OnTabRefreshed);
        QObject::connect(itemsManager.get(),
                         &ItemsManager::ChildrenReconciled,
                         window.get(),
                         &MainWindow::OnChildrenReconciled);
        QObject::connect(itemsManager.get(),
                         &ItemsManager::RefreshFinished,
                         window.get(),
                         &MainWindow::OnRefreshFinished);
    }

    QTemporaryDir tempDir;
    BuyoutManagerFixture buyoutFixture;
    std::unique_ptr<StashRepo> stashRepo;
    std::unique_ptr<CharacterRepo> characterRepo;
    std::unique_ptr<QSettings> settings;
    std::unique_ptr<NetworkManager> networkManager;
    std::unique_ptr<RateLimiter> rateLimiter;
    std::unique_ptr<PoeApiClient> api;
    std::unique_ptr<ItemsManager> itemsManager;
    std::unique_ptr<CurrencyManager> currencyManager;
    std::unique_ptr<Shop> shop;
    std::unique_ptr<ImageCache> imageCache;
    std::unique_ptr<MainWindow> window;
};
