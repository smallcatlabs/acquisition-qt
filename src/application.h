// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QMessageBox>
#include <QObject>
#include <QString>

#include <memory>

class QNetworkReply;
class QSettings;

class BuyoutManager;
class CurrencyManager;
class DataStore;
class ImageCache;
class ItemsManager;
class ItemsManagerWorker;
class LoginDialog;
class MainWindow;
class NetworkManager;
class OAuthManager;
struct OAuthToken;
class PoeApiClient;
class RateLimiter;
class RePoE;
class Shop;
class UserStore;
class UpdateChecker;

// Opens the global data store under <data dir>/data, first backing up that
// folder when the version recorded in the settings is not this version.
std::unique_ptr<DataStore> OpenGlobalDataStore(const QDir &dataDir, QSettings &settings);

struct MainWindowDeleter
{
    void operator()(MainWindow *window) const;
};

class Application : public QObject
{
    Q_OBJECT
public:
    explicit Application(const QDir &appDataDir);
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

public slots:
    void ClearSessionId();
    void SetSessionId(const QString &poesessid);
    void SetTheme(const QString &theme);
    void SetUserDir(const QString &dir);
    void OnLogin();
    void OnItemsRefreshed(bool initial_refresh);

private:
    struct CoreServices
    {
        CoreServices(const QDir &appDataDir);

        const QDir dir;
        std::unique_ptr<QSettings> settings;
        std::unique_ptr<NetworkManager> network_manager;
        std::unique_ptr<DataStore> global_data;
        std::unique_ptr<UpdateChecker> update_checker;
        std::unique_ptr<OAuthManager> oauth_manager;
        std::unique_ptr<RePoE> repoe;
        std::unique_ptr<ImageCache> image_cache;
        std::unique_ptr<LoginDialog> login;
    };

    struct UserSession
    {
        UserSession(const Application::CoreServices &core);

        std::unique_ptr<DataStore> data;
        std::unique_ptr<UserStore> userstore;
        std::unique_ptr<RateLimiter> rate_limiter;
        // Declared immediately after the limiter it references, so it dies
        // before the limiter and after every consumer that holds it (R6-2).
        std::unique_ptr<PoeApiClient> api;
        std::unique_ptr<BuyoutManager> buyout_manager;
        std::unique_ptr<ItemsManager> items_manager;
        std::unique_ptr<ItemsManagerWorker> items_worker;
        std::unique_ptr<Shop> shop;
        std::unique_ptr<CurrencyManager> currency_manager;
        std::unique_ptr<MainWindow, MainWindowDeleter> main_window;
    };

    CoreServices &core() const;

    inline QSettings &settings() const { return *core().settings; }
    inline NetworkManager &network_manager() const { return *core().network_manager; }
    inline UpdateChecker &update_checker() const { return *core().update_checker; }
    inline OAuthManager &oauth_manager() const { return *core().oauth_manager; }
    inline DataStore &global_data() const { return *core().global_data; }
    inline RePoE &repoe() const { return *core().repoe; }
    inline ImageCache &image_cache() const { return *core().image_cache; }
    inline LoginDialog &login() const { return *core().login; }

    UserSession &session() const;

    inline DataStore &data() const { return *session().data; }
    inline UserStore &userstore() const { return *session().userstore; }
    inline RateLimiter &rate_limiter() const { return *session().rate_limiter; }
    inline BuyoutManager &buyout_manager() const { return *session().buyout_manager; }
    inline ItemsManager &items_manager() const { return *session().items_manager; }
    inline ItemsManagerWorker &items_worker() const { return *session().items_worker; }
    inline Shop &shop() const { return *session().shop; }
    inline CurrencyManager &currency_manager() const { return *session().currency_manager; }
    inline MainWindow &main_window() const { return *session().main_window; }

    void InitCoreServices();
    void InitUserSession();

    std::unique_ptr<CoreServices> m_core;
    std::unique_ptr<UserSession> m_session;

    QDir m_data_dir;
};
