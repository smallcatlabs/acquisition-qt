// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "application.h"

#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QNetworkCookieJar>
#include <QSettings>

#include "buyoutmanager.h"
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

Application::Application(const QDir &appDataDir)
{
    spdlog::debug("Application: created");

    spdlog::debug("Application: data directory is {}", appDataDir.absolutePath());
    m_data_dir = appDataDir;

    InitCoreServices();
}

Application::~Application() = default;

void Application::InitCoreServices()
{
    m_core = std::make_unique<Application::CoreServices>(m_data_dir);

    // Connect to the update signal in case an update is detected before the main window is open.
    connect(&update_checker(),
            &UpdateChecker::UpdateAvailable,
            &update_checker(),
            &UpdateChecker::AskUserToUpdate);

    // Connect signals from the login dialog.
    connect(&login(), &LoginDialog::ChangeTheme, this, &Application::SetTheme);
    // Queued: SetUserDir destroys the login dialog, whose button is still
    // emitting when this signal fires.
    connect(&login(),
            &LoginDialog::ChangeUserDir,
            this,
            &Application::SetUserDir,
            Qt::QueuedConnection);
    connect(&login(), &LoginDialog::LoginComplete, this, &Application::OnLogin);

    if (settings().value("realm").toString().isEmpty()) {
        settings().setValue("realm", "pc");
    }
    spdlog::debug("Application: realm is {}", settings().value("realm"));

    SaveDataOnNewVersion();

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

void Application::InitUserSession()
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

    m_session = std::make_unique<Application::UserSession>(core());

    auto item_mgr = &items_manager();
    auto worker = &items_worker();

    connect(item_mgr, &ItemsManager::UpdateSignal, worker, &ItemsManagerWorker::Update);
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
    // the shop's slot gates on a clean CompletedRefresh. Connected after
    // the ItemsRefreshed chain above so ExpireShopData has already run when
    // the gate evaluates (the worker emits ItemsRefreshed before
    // RefreshFinished, so this holds by emission order regardless).
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

Application::CoreServices::CoreServices(const QDir &appDataDir)
    : dir(appDataDir)
{
    spdlog::trace("Application: creating NetworkManager");
    network_manager = std::make_unique<NetworkManager>();

    spdlog::trace("Application: creating RePoE");
    repoe = std::make_unique<RePoE>(*network_manager);

    const QString settings_path = dir.filePath("settings.ini");
    spdlog::trace("Application: creating the settings object: {}", settings_path);
    settings = std::make_unique<QSettings>(settings_path, QSettings::IniFormat);

    const QDir user_dir(dir.filePath("data"));
    const QString global_data_file = user_dir.filePath(SqliteDataStore::MakeFilename("", ""));
    spdlog::trace("Application: opening global data file: {}", global_data_file);
    global_data = std::make_unique<SqliteDataStore>(global_data_file);

    const QString image_cache_dir = dir.absoluteFilePath("cache");
    image_cache = std::make_unique<ImageCache>(*network_manager, image_cache_dir);

    spdlog::trace("Application: creating update checker");
    update_checker = std::make_unique<UpdateChecker>(*settings, *network_manager);

    spdlog::trace("Application: creating OAuth manager");
    oauth_manager = std::make_unique<OAuthManager>(*network_manager, *global_data);

    spdlog::trace("Application: creating login dialog");
    login = std::make_unique<LoginDialog>(dir.absolutePath(),
                                          *settings,
                                          *network_manager,
                                          *oauth_manager,
                                          *global_data);
}

Application::UserSession::UserSession(const Application::CoreServices &core)
{
    spdlog::trace("Application::UserSession::UserSession() entered");

    const auto &dir = core.dir;
    auto &settings = *core.settings;
    auto &network_manager = *core.network_manager;
    auto &image_cache = *core.image_cache;

    const QString league = settings.value("league").toString();
    const QString account = settings.value("account").toString();
    if (league.isEmpty()) {
        FatalError("Login failure: the league has not been set.");
    }
    if (account.isEmpty()) {
        FatalError("Login failure: the account has not been set.");
    }
    spdlog::trace("Application::UserSession::UserSession() league = {}", league);
    spdlog::trace("Application::UserSession::UserSession() account = {}", account);

    QDir data_dir = dir.filePath("data");
    const QString data_file = SqliteDataStore::MakeFilename(account, league);
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
    items_worker = std::make_unique<ItemsManagerWorker>(settings, *buyout_manager, *api);

    spdlog::trace("Application::UserSession::UserSession() creating shop");
    shop = std::make_unique<Shop>(settings,
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

void Application::OnLogin()
{
    spdlog::debug("Application: login initiated");

    InitUserSession();

    // Look for an existing POESESSID. For some reason doing this in InitCoreServices
    // doesn't seem to work, even though that's where it logically belongs. Could possibly
    // be due to the fact that happens from main() before the Qt app is started?
    if (settings().contains("session_id")) {
        spdlog::debug("Application: loading an existing POESESSID cookie");
        const auto session_id = settings().value("session_id").toString();
        network_manager().setPoeSessionId(session_id);
    }

    if (repoe().IsInitialized()) {
        spdlog::debug("Application: RePoE data is available.");
        items_worker().OnRePoEReady();
    } else {
        spdlog::debug("Application: Waiting for RePoE data.");
        connect(&repoe(), &RePoE::finished, &items_worker(), &ItemsManagerWorker::OnRePoEReady);
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

void Application::SetUserDir(const QString &dir)
{
    // Same shutdown invariant as InitUserSession(): resetting m_session (and
    // m_core beneath it) tears down the worker and hub, which is only safe once
    // the event loop has stopped (network-redesign.md, "Shutdown and task
    // ownership"). The data directory is chosen from the login dialog before any
    // session exists; a change once a session is live would destroy owners
    // mid-loop, so refuse it loudly rather than corrupt suspended frames.
    if (m_session) {
        FatalError("Application: refusing to change the data directory while a user "
                   "session is live (network-redesign.md shutdown invariant)");
    }

    m_core.reset();
    m_data_dir = QDir(dir);

    InitCoreServices();
}

void Application::OnItemsRefreshed(bool initial_refresh)
{
    spdlog::trace("Application::OnItemsRefreshed() entered");
    spdlog::trace("Application::OnItemsRefreshed() initial_refresh = {}", initial_refresh);
    Q_UNUSED(initial_refresh);
    currency_manager().Update();
    // Snapshot-boundary expiry (M2 D8/R5-6): dirties the preview cache but
    // keeps any waiting clean automatic capture — only local edits drop it.
    shop().OnPublishedSnapshot();
    // Automatic forum submission moved to the typed terminal event (M2
    // D8/R1-1): Shop::OnRefreshFinished gates it on a CLEAN CompletedRefresh,
    // which the initial cached load never emits (it is not an update).
}

void Application::SaveDataOnNewVersion()
{
    spdlog::trace("Application::SaveDataOnNewVersion() entered");
    BackupDataOnNewVersion(m_data_dir,
                           settings(),
                           global_data().Get("version", "UNKNOWN-VERSION"),
                           APP_VERSION_STRING);
}
