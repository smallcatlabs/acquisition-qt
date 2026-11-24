// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "application.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QNetworkCookieJar>
#include <QSettings>

#include "buyoutmanager.h"
#include "cacheload.h"
#include "currencymanager.h"
#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/sqlitedatastore.h"
#include "datastore/stashrepo.h"
#include "datastore/userstore.h"
#include "imagecache.h"
#include "itemsmanager.h"
#include "itemsmanagerworker.h"
#include "poe/poeapiclient.h"
#include "ratelimit/ratelimiter.h"
#include "repoe/repoe.h"
#include "shop.h"
#include "ui/logindialog.h"
#include "ui/mainwindow_bridge.h"
#include "util/databackup.h"
#include "util/fatalerror.h"
#include "util/networkmanager.h"
#include "util/oauthmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/updatechecker.h"
#include "version_defines.h"

Application::Application(const QDir &appDataDir, std::unique_ptr<NetworkManager> network_manager)
{
    spdlog::debug("Application: created");

    spdlog::debug("Application: data directory is {}", appDataDir.absolutePath());
    m_data_dir = appDataDir;

    InitCoreServices(std::move(network_manager));
}

Application::~Application() = default;

void Application::InitCoreServices(std::unique_ptr<NetworkManager> network_manager)
{
    m_core = std::make_unique<Application::CoreServices>(m_data_dir, std::move(network_manager));

    // Connect to the update signal in case an update is detected before the main window is open.
    connect(&update_checker(),
            &UpdateChecker::UpdateAvailable,
            &update_checker(),
            &UpdateChecker::AskUserToUpdate);

    // Connect signals from the login dialog.
    connect(&login(), &LoginDialog::ChangeTheme, this, &Application::SetTheme);
    connect(&login(), &LoginDialog::LoginComplete, this, &Application::OnLogin);

    if (settings().value("realm").toString().isEmpty()) {
        settings().setValue("realm", "pc");
    }
    spdlog::debug("Application: realm is {}", settings().value("realm"));

    spdlog::debug("Application: loading theme");
    SetTheme(settings().value("theme", "default").toString());

    // Start the process of fetching RePoE data.
    spdlog::debug("Application: initializing RePoE");
    repoe().Init(m_data_dir.absolutePath());

    // Start the initial check for updates.
    spdlog::debug("Application: checking for application updates");
    update_checker().CheckForUpdates();

    // Show the login dialog now.
    spdlog::debug("Application: showing the login dialog");
    login().show();
}

void Application::InitUserSession(const SessionIdentity &identity)
{
    // Owners (worker, hub) may only be destroyed after the event loop has
    // stopped: the coroutine-shutdown analysis in network-redesign.md
    // ("Shutdown and task ownership", ~lines 999-1007) is valid ONLY then,
    // because a suspended frame detaches rather than dies and would resume
    // into freed memory if any loop iteration ran after teardown. Replacing a
    // live m_session here would destroy those owners mid-loop and violate that
    // invariant. The current UI reaches this exactly once, before any session
    // exists; this guard encodes the precondition the spec only stated in prose
    // so a future re-login path cannot silently reintroduce the use-after-free.
    if (m_session) {
        FatalError("Application: refusing to replace a live user session while the "
                   "event loop is running (network-redesign.md shutdown invariant)");
    }

    m_session = std::make_unique<Application::UserSession>(core(), identity);

    auto item_mgr = &items_manager();
    auto worker = &items_worker();

    ConnectRefreshRequests(*item_mgr, *worker);
    connect(worker, &ItemsManagerWorker::StatusUpdate, item_mgr, &ItemsManager::OnStatusUpdate);
    connect(worker, &ItemsManagerWorker::ItemsRefreshed, item_mgr, &ItemsManager::OnItemsRefreshed);
    // Presentation-lane deltas (items-pipeline M2, D3): the manager streams
    // each one into its published copy and re-emits for the UI.
    connect(worker, &ItemsManagerWorker::TabRefreshed, item_mgr, &ItemsManager::OnTabRefreshed);
    connect(worker,
            &ItemsManagerWorker::ChildrenReconciled,
            item_mgr,
            &ItemsManager::OnChildrenReconciled);
    // The typed terminal event (M2 D4), forwarded signal-to-signal: the
    // manager adds nothing to it.
    connect(worker, &ItemsManagerWorker::RefreshFinished, item_mgr, &ItemsManager::RefreshFinished);
    connect(item_mgr, &ItemsManager::ItemsRefreshed, this, &Application::OnItemsRefreshed);
    // Automatic forum submission rides the terminal event (M2 D8/R1-1):
    // the shop's slot gates on a clean CompletedRefresh.
    connect(item_mgr, &ItemsManager::RefreshFinished, &shop(), &Shop::OnRefreshFinished);
    connect(&shop(), &Shop::PoeSessionRejected, this, &Application::ClearSessionId);

    auto characters = &userstore().characters();
    auto stashes = &userstore().stashes();

    connect(worker,
            &ItemsManagerWorker::characterListReceived,
            characters,
            &CharacterRepo::saveCharacterList);
    connect(worker,
            &ItemsManagerWorker::characterReceived,
            characters,
            &CharacterRepo::saveCharacter);
    connect(worker, &ItemsManagerWorker::stashListReceived, stashes, &StashRepo::saveStashList);
    connect(worker, &ItemsManagerWorker::stashReceived, stashes, &StashRepo::saveStash);

    // Authoritative-list reconciliation: rows the server no longer lists
    // are deleted so they cannot resurrect from the cache (F53).
    connect(worker,
            &ItemsManagerWorker::characterListReplaced,
            characters,
            &CharacterRepo::reconcileCharacterList);
    connect(worker, &ItemsManagerWorker::stashListReplaced, stashes, &StashRepo::reconcileStashList);
    connect(worker,
            &ItemsManagerWorker::stashChildrenReplaced,
            stashes,
            &StashRepo::reconcileStashChildren);

    auto updater = &update_checker();
    auto cache = &image_cache();

    // Disconnect from the update signal so that only the main window gets it from now on.
    disconnect(updater, &UpdateChecker::UpdateAvailable, nullptr, nullptr);

    // Connect UI signals.
    ConnectMainWindow(*this, main_window(), *item_mgr, *worker, shop(), *updater, *cache);
    // A login can come before the game data: the window is built without
    // it, and takes what it needs from it when it arrives (#386).
    ConnectGameData(main_window(), repoe());
}

Application::CoreServices &Application::core() const
{
    if (m_core) {
        return *m_core;
    }
    FatalError("Application: core services not initialized");
}

Application::UserSession &Application::session() const
{
    if (m_session) {
        return *m_session;
    }
    FatalError("Application: user session not initialized");
}

std::unique_ptr<DataStore> OpenGlobalDataStore(const QDir &dataDir, QSettings &settings)
{
    const QDir user_dir(dataDir.filePath("data"));
    const QString global_data_file = user_dir.filePath(SqliteDataStore::MakeFilename("", ""));
    // The version that last used the data is kept in the global store, with
    // the data it describes, so that losing settings.ini cannot make the data
    // look like it came from an unknown version (#446). v0.16 to v0.18 kept
    // it in settings.ini instead: where that setting exists it is the later
    // word (a store row beside it dates from before v0.16), and it is removed
    // once read so the store is the only record from here on.
    //
    // Back up before the store is opened for use: opening creates the
    // database, and a first run would otherwise back up its own new file.
    QString previous_version = settings.value("version").toString();
    if (previous_version.isEmpty() && QFile::exists(global_data_file)) {
        previous_version = SqliteDataStore(global_data_file).Get("version", "");
    }
    if (previous_version.isEmpty()) {
        previous_version = "UNKNOWN-VERSION";
    }
    BackupDataOnNewVersion(dataDir, previous_version, APP_VERSION_STRING);

    spdlog::trace("Application: opening global data file: {}", global_data_file);
    auto store = std::make_unique<SqliteDataStore>(global_data_file);
    // Recorded whether or not a backup was made; otherwise every later start
    // would try again.
    if (store->Get("version", "") != APP_VERSION_STRING) {
        spdlog::debug("Application: recording version {} in the global store", APP_VERSION_STRING);
        store->Set("version", APP_VERSION_STRING);
    }
    settings.remove("version");
    return store;
}

Application::CoreServices::CoreServices(const QDir &appDataDir,
                                        std::unique_ptr<NetworkManager> network)
    : dir(appDataDir)
    , network_manager(std::move(network))
{
    if (!network_manager) {
        FatalError("Application: no network manager was supplied");
    }
    // The HTTP disk cache is gone; clear what earlier versions left in this
    // data directory. Never touches any other directory.
    NetworkManager::removeLegacyDiskCache(dir);

    spdlog::trace("Application: creating RePoE");
    repoe = std::make_unique<RePoE>(*network_manager);

    const QString settings_path = dir.filePath("settings.ini");
    spdlog::trace("Application: creating the settings object: {}", settings_path);
    settings = std::make_unique<QSettings>(settings_path, QSettings::IniFormat);

    global_data = OpenGlobalDataStore(dir, *settings);

    const QString image_cache_dir = dir.absoluteFilePath("cache");
    image_cache = std::make_unique<ImageCache>(*network_manager, image_cache_dir);

    spdlog::trace("Application: creating update checker");
    update_checker = std::make_unique<UpdateChecker>(*settings, *network_manager);

    spdlog::trace("Application: creating OAuth manager");
    oauth_manager = std::make_unique<OAuthManager>(*network_manager, *global_data);

    spdlog::trace("Application: creating login dialog");
    login = std::make_unique<LoginDialog>(*settings, *network_manager, *oauth_manager, *global_data);
}

Application::UserSession::UserSession(const Application::CoreServices &core,
                                      const SessionIdentity &login_identity)
    : identity(login_identity)
    , data_dir(core.dir.filePath("data"))
{
    spdlog::trace("Application::UserSession::UserSession() entered");

    const auto &dir = core.dir;
    auto &settings = *core.settings;
    auto &network_manager = *core.network_manager;
    auto &image_cache = *core.image_cache;

    // The identity was decided and validated at login (#299); nothing here
    // reads it back from the settings.
    const QString &account = identity.account();
    spdlog::trace("Application::UserSession::UserSession() league = {}", identity.league());
    spdlog::trace("Application::UserSession::UserSession() account = {}", account);

    const QString data_file = SqliteDataStore::MakeFilename(account, identity.league());
    const QString data_path = data_dir.absoluteFilePath(data_file);
    spdlog::trace("Application::UserSession::UserSession() data_path = {}", data_path);
    data = std::make_unique<SqliteDataStore>(data_path);

    spdlog::trace("Application::UserSession::UserSession() creating user datastore");
    userstore = std::make_unique<UserStore>(data_dir, account);

    spdlog::trace("Application::UserSession::UserSession() creating rate limiter");
    rate_limiter = std::make_unique<RateLimiter>(network_manager);
    if (settings.value("network_capture_enabled", false).toBool()) {
        // One file per session, stamped at enable time: a fixed name would
        // either pile sessions into one file (append) or destroy retained
        // capture evidence on the next launch (truncate).
        const QString capture_name = QString("network-capture-%1.jsonl")
                                         .arg(QDateTime::currentDateTimeUtc().toString(
                                             "yyyyMMdd'T'hhmmss'Z'"));
        rate_limiter->EnableCapture(dir.filePath(capture_name));
    }

    spdlog::trace("Application::UserSession::UserSession() creating the api client");
    api = std::make_unique<PoeApiClient>(*rate_limiter);

    spdlog::trace("Application::UserSession::UserSession() creating buyout manager");
    buyout_manager = std::make_unique<BuyoutManager>(*data, userstore->buyouts());

    spdlog::trace("Application::UserSession::UserSession() creating items manager");
    items_manager = std::make_unique<ItemsManager>(settings, *buyout_manager);

    spdlog::trace("Application::UserSession::UserSession() creating items worker");
    items_worker = std::make_unique<ItemsManagerWorker>(settings, identity, *buyout_manager, *api);

    spdlog::trace("Application::UserSession::UserSession() creating shop");
    shop = std::make_unique<Shop>(settings,
                                  identity,
                                  network_manager,
                                  *api,
                                  *data,
                                  *items_manager,
                                  *buyout_manager);

    spdlog::trace("Application::UserSession::UserSession() creating currency manager");
    currency_manager = std::make_unique<CurrencyManager>(settings, *data, *items_manager);

    // Prepare to show the main window now that everything is initialized.
    spdlog::trace("Application:creating main window");
    main_window.reset(CreateMainWindow(settings,
                                       identity,
                                       network_manager,
                                       *rate_limiter,
                                       *items_manager,
                                       *buyout_manager,
                                       userstore->buyouts(),
                                       userstore->stashes(),
                                       userstore->characters(),
                                       *currency_manager,
                                       *shop,
                                       image_cache,
                                       dir));
}

void Application::OnLogin(const SessionIdentity &identity)
{
    spdlog::debug("Application: login initiated");

    InitUserSession(identity);

    // Look for an existing POESESSID. For some reason doing this in InitCoreServices
    // doesn't seem to work, even though that's where it logically belongs. Could possibly
    // be due to the fact that happens from main() before the Qt app is started?
    if (settings().contains("session_id")) {
        spdlog::debug("Application: loading an existing POESESSID cookie");
        const auto session_id = settings().value("session_id").toString();
        network_manager().setPoeSessionId(session_id);
    }

    // The cached items are built into Items, which needs the game data, so
    // the cache load starts once RePoE is ready.
    const auto start_cache_load = [this] {
        auto &s = session();
        s.cache_load = StartCacheLoad(*s.items_worker,
                                      s.data_dir,
                                      s.identity,
                                      s.userstore->isUsable());
    };
    if (repoe().IsInitialized()) {
        spdlog::debug("Application: RePoE data is available.");
        start_cache_load();
    } else {
        spdlog::debug("Application: Waiting for RePoE data.");
        connect(&repoe(), &RePoE::finished, this, start_cache_load, Qt::SingleShotConnection);
    }

    spdlog::trace("Application::OnLogin() closing the login dialog");
    login().close();

    spdlog::trace("Application::OnLogin() showing the main window");
    ShowMainWindow(main_window());
}

void Application::ClearSessionId()
{
    network_manager().clearPoeSessionId();
    settings().remove("session_id");
}

void Application::SetSessionId(const QString &poesessid)
{
    if (poesessid.isEmpty()) {
        ClearSessionId();
    } else {
        network_manager().setPoeSessionId(poesessid);
        settings().setValue("session_id", poesessid);
    }
}

void Application::SetTheme(const QString &theme)
{
    spdlog::trace("Application::SetTheme() entered");

    QString stylesheet;
    QColor text_color;

    if (0 == theme.compare("default", Qt::CaseInsensitive)) {
        stylesheet.clear();
        text_color = Qt::black;
    } else if (0 == theme.compare("light", Qt::CaseInsensitive)) {
        stylesheet = ":qdarkstyle/light/lightstyle.qss";
        text_color = Qt::black;
    } else if (0 == theme.compare("dark", Qt::CaseInsensitive)) {
        stylesheet = ":qdarkstyle/dark/darkstyle.qss";
        text_color = Qt::white;
    } else {
        spdlog::error("Invalid theme: {}", theme);
        return;
    }

    spdlog::trace("Application::SetTheme() setting theme: {}", theme);
    settings().setValue("theme", theme);

    QString style_data;
    if (!stylesheet.isEmpty()) {
        spdlog::trace("Application::SetTheme() loading stylesheet: {}", stylesheet);
        QFile f(stylesheet);
        if (f.exists()) {
            if (!f.open(QFile::ReadOnly | QFile::Text)) {
                spdlog::error("Error loading stylesheet ({}): {}", stylesheet, f.errorString());
            } else {
                style_data = f.readAll();
                f.close();
            }
        } else {
            spdlog::error("Style sheet not found: {}", stylesheet);
        }
    }

    spdlog::trace("Application::SetTheme() setting stylesheet");
    qApp->setStyleSheet(style_data);
}

void Application::OnItemsRefreshed(bool initial_refresh)
{
    spdlog::trace("Application::OnItemsRefreshed() entered");
    spdlog::trace("Application::OnItemsRefreshed() initial_refresh = {}", initial_refresh);
    Q_UNUSED(initial_refresh);
    currency_manager().Update();
    // Automatic forum submission moved to the typed terminal event (M2
    // D8/R1-1): Shop::OnRefreshFinished gates it on a CLEAN CompletedRefresh,
    // which the initial cached load never emits (it is not an update).
}
