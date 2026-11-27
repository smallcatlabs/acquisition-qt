// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/logindialog.h"
#include "ui_logindialog.h"

#include <QDesktopServices>
#include <QFile>
#include <QMessageBox>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>

#include "datastore/datastore.h"
#include "poe/types/league.h"
#include "replytimeout.h"
#include "util/json_readers.h"
#include "util/networkmanager.h"
#include "util/oauthmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/updatechecker.h"
#include "util/util.h"
#include "version_defines.h"

#include "mainwindow.h"

constexpr const char *POE_LEAGUE_LIST_URL
    = "https://api.pathofexile.com/leagues?type=main&compact=1";

LoginDialog::LoginDialog(QSettings &settings,
                         NetworkManager &network_manager,
                         OAuthManager &oauth_manager,
                         DataStore &datastore)
    : QDialog(nullptr)
    , m_settings(settings)
    , m_network_manager(network_manager)
    , m_oauth_manager(oauth_manager)
    , m_datastore(datastore)
    , ui(new Ui::LoginDialog)
{
    // Setup the dialog box.
    spdlog::trace("LoginDialog::LoginDialog() calling UI setup");
    ui->setupUi(this);

    // Set window properties.
    spdlog::trace("LoginDialog::LoginDialog() setting window properties");
    setWindowTitle(QString("Acquisition Login [") + APP_VERSION_STRING + "]");
    setWindowIcon(QIcon(":/icons/icon.svg"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Setup the realm options.
    ui->realmComboBox->addItems({"pc", "sony", "xbox"});

    // Setup theme.
    ui->themeComboBox->addItems({"default", "light", "dark"});

    // Setup logging levels.
    for (int level = spdlog::level::trace; level < spdlog::level::n_levels; ++level) {
        ui->loggingLevelComboBox->addItem(to_qstring(static_cast<spdlog::level::level_enum>(level)));
    }

    // No error stands yet, so the label is hidden.
    ShowErrors();
    ui->errorLabel->setStyleSheet("QLabel {color: red;}");
    // An error can quote the server (a refused login's description), so it
    // is shown as written, never rendered as rich text.
    ui->errorLabel->setTextFormat(Qt::PlainText);

    // Load saved settings.
    LoadSettings();

    // Set the proxy.
    QNetworkProxyFactory::setUseSystemConfiguration(ui->proxyCheckBox->isChecked());

    // Connect main UI buttons.
    connect(ui->loginTabs, &QTabWidget::currentChanged, this, &LoginDialog::OnLoginTabChanged);
    connect(ui->authenticateButton,
            &QPushButton::clicked,
            this,
            &LoginDialog::OnAuthenticateButtonClicked);
    connect(ui->loginButton, &QPushButton::clicked, this, &LoginDialog::OnLoginButtonClicked);

    // Connects options UI elements.
    connect(ui->advancedCheckBox,
            &QCheckBox::checkStateChanged,
            this,
            &LoginDialog::OnAdvancedCheckBoxChanged);
    connect(ui->rememberMeCheckBox,
            &QCheckBox::checkStateChanged,
            this,
            &LoginDialog::OnRememberMeCheckBoxChanged);
    connect(ui->reportCrashesCheckBox,
            &QCheckBox::checkStateChanged,
            this,
            &LoginDialog::OnReportCrashesCheckBoxChanged);
    connect(ui->proxyCheckBox,
            &QCheckBox::checkStateChanged,
            this,
            &LoginDialog::OnProxyCheckBoxChanged);
    connect(ui->loggingLevelComboBox,
            &QComboBox::currentTextChanged,
            this,
            &LoginDialog::OnLoggingLevelChanged);
    connect(ui->themeComboBox, &QComboBox::currentTextChanged, this, &LoginDialog::OnThemeChanged);
    connect(ui->realmComboBox, &QComboBox::currentTextChanged, this, &LoginDialog::OnRealmChanged);
    connect(ui->realmComboBox, &QComboBox::activated, this, &LoginDialog::OnRealmActivated);

    // Listen for access from the OAuth manager, and for a login that fails.
    connect(&m_oauth_manager, &OAuthManager::grantAccess, this, &LoginDialog::OnOAuthAccessGranted);
    connect(&m_oauth_manager, &OAuthManager::loginFailed, this, &LoginDialog::OnOAuthLoginFailed);
    connect(&m_oauth_manager,
            &OAuthManager::storedLoginFailed,
            this,
            &LoginDialog::OnOAuthStoredLoginFailed);

    // Use OnLoginTabChanged to do things like enable the login button.
    OnLoginTabChanged(ui->loginTabs->currentIndex());

    // Request the list of leagues.
    spdlog::trace("Login: requesting leagues");
    RequestLeagues();
}

LoginDialog::~LoginDialog()
{
    if (!ui->rememberMeCheckBox->isChecked()) {
        if (auto lg = spdlog::default_logger(); lg) {
            lg->trace("Login: forgetting the user's credentials");
        }
        // "Remember me" governs credentials only. Every other setting (theme,
        // proxy, log level, crash reporting, the recorded version, ...) stays.
        m_settings.remove("session_id");
        m_settings.remove("account");
        m_datastore.Set("oauth_token", "");
    }
    delete ui;
}

void LoginDialog::LoadSettings()
{
    const QString realm = m_settings.value("realm").toString();
    ui->realmComboBox->setCurrentText(realm);

    const QString league = m_settings.value("league").toString();
    ui->leagueComboBox->setCurrentText(league);

    const spdlog::string_view_t sv = spdlog::level::to_string_view(spdlog::get_level());
    const QString logging_level = QString::fromUtf8(sv.data(), sv.size());
    ui->loggingLevelComboBox->setCurrentText(logging_level);

    const QString theme = m_settings.value("theme").toString();
    ui->themeComboBox->setCurrentText(theme);

    const bool show_advanced = m_settings.value("show_advanced_login_options").toBool();
    ui->advancedCheckBox->setChecked(show_advanced);

    const bool remember_user = m_settings.value("remember_user", true).toBool();
    ui->rememberMeCheckBox->setChecked(remember_user);

    const bool use_proxy = m_settings.value("use_system_proxy").toBool();
    ui->proxyCheckBox->setChecked(use_proxy);

    const bool report_crashes = m_settings.value("report_crashes", true).toBool();
    ui->reportCrashesCheckBox->setChecked(report_crashes);

    const QString login_tab = m_settings.value("login_tab").toString();
    spdlog::trace("LoginDialog::LoadSettings() login_tab = {}", login_tab);
    for (auto i = 0; i < ui->loginTabs->count(); ++i) {
        const QString tab_name = ui->loginTabs->widget(i)->objectName();
        if (0 == login_tab.compare(tab_name, Qt::CaseInsensitive)) {
            ui->loginTabs->setCurrentIndex(i);
            break;
        }
    }
}

void LoginDialog::RequestLeagues()
{
    // A request still on its way is for a realm no longer chosen (#287): it
    // is dropped, and says nothing.
    if (m_leagues_reply) {
        QNetworkReply *previous = m_leagues_reply;
        m_leagues_reply = nullptr;
        disconnect(previous, nullptr, this, nullptr);
        previous->abort();
        previous->deleteLater();
    }

    // The league box waits for the realm's list.
    ui->leagueComboBox->setEnabled(false);

    // Make a non-API request to get the list of leagues. This currently uses
    // a legacy endpoint that is not rate limited and does not require
    // authentication. It lists the leagues of one realm: pc, unless asked.
    QUrl url{QString(POE_LEAGUE_LIST_URL)};
    QUrlQuery query(url);
    query.addQueryItem("realm", ui->realmComboBox->currentText());
    url.setQuery(query);
    QNetworkRequest request = QNetworkRequest(url);
    request.setTransferTimeout(kPoeApiTimeout);

    // Send the request and handle errors.
    spdlog::trace("LoginDialog::RequestLeagues() sending request: {}", request.url().toString());
    QNetworkReply *reply = m_network_manager.get(request);
    m_leagues_reply = reply;
    // A failed request is said when it finishes (OnLeaguesReceived).
    connect(reply, &QNetworkReply::finished, this, &LoginDialog::OnLeaguesReceived);
    connect(reply, &QNetworkReply::sslErrors, this, [](const QList<QSslError> &errors) {
        for (const auto &error : errors) {
            spdlog::error("SSL Error requesting leagues: {}", error.errorString());
        }
    });
    UpdateLoginButton();
}

void LoginDialog::OnLeaguesReceived()
{
    spdlog::trace("LoginDialog::OnLeaguesReceived() reply recieved");
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();
    // Only the request of the realm now chosen is connected: RequestLeagues
    // disconnects and aborts any earlier one.
    m_leagues_reply = nullptr;

    // Check for network errors.
    if (reply->error()) {
        spdlog::trace("LoginDialog::OnLeaguesReceived() reply error {}", reply->error());
        LeaguesRequestError(reply->errorString(), bytes);
        return;
    }

    // Parse the leagues.
    const auto leagues = json::readLeagueList(bytes);
    if (!leagues) {
        LeaguesRequestError("the reply could not be read", bytes);
        return;
    }
    if (leagues->empty()) {
        LeaguesRequestError("the list held no leagues", bytes);
        return;
    }

    // Get the league from settings.ini
    const QString saved_league = m_settings.value("league").toString();
    spdlog::trace("LoginDialog::OnLeaguesReceived() loaded leage from settings: {}", saved_league);

    bool use_saved_league = false;

    // Refilling the box for another realm must not reach OnLeagueChanged:
    // clearing it would save an empty league.
    const QSignalBlocker blocker(ui->leagueComboBox);
    ui->leagueComboBox->clear();
    for (auto &league : *leagues) {
        // Add the league to the combo box.
        ui->leagueComboBox->addItem(league.id);

        // Set the current league if it matches the saved league.
        if (0 == saved_league.compare(league.id, Qt::CaseInsensitive)) {
            use_saved_league = true;
        }
    }
    ui->leagueComboBox->setEnabled(true);
    // editable so private-league ids (absent from the public list) can be typed
    ui->leagueComboBox->setEditable(true);

    // If we found a match for the save league use it. If we didn't, then
    // we need to clear the setting, since the list of leagues may have
    // changed since the last time acquisition was run.
    if (use_saved_league || !saved_league.isEmpty()) {
        spdlog::trace("LoginDialog::OnLeaguesReceived() setting league to saved value: {}",
                      saved_league);
        ui->leagueComboBox->setCurrentText(saved_league);
    } else {
        const auto league = leagues->front().name.value_or("Standard");
        spdlog::trace("LoginDialog::OnLeaguesReceived() setting league to default value: {}",
                      league);
        m_settings.setValue("league", league);
    }

    // Now that leagues have been received, start listening for changes. A
    // later list, for another realm, finds the connection made.
    connect(ui->leagueComboBox,
            &QComboBox::currentTextChanged,
            this,
            &LoginDialog::OnLeagueChanged,
            Qt::UniqueConnection);

    // The list is in: an error left by an earlier request is over.
    m_leagues_error.clear();
    ShowErrors();
    UpdateLoginButton();
}

void LoginDialog::LeaguesRequestError(const QString &error, const QByteArray &reply)
{
    spdlog::error("League reply was: {}", reply);
    m_leagues_error = "Error requesting leagues: " + error;
    spdlog::error("LoginDialog: {}", m_leagues_error);
    ShowErrors();
    UpdateLoginButton();
}

void LoginDialog::OnAuthenticateButtonClicked()
{
    spdlog::trace("LoginDialog::OnAuthenticateButtonClicked() entered");
    ClearError();
    // The button stays usable while the browser is out (#401): the browser
    // may never come back, and pressing the button again drops this attempt
    // and starts another.
    ui->authenticateButton->setText("Authenticating...");
    if (!m_oauth_manager.initLogin()) {
        DisplayAuthenticationError("Cannot start authentication: Acquisition could not listen "
                                   "for the browser's reply on 127.0.0.1.");
    }
}

void LoginDialog::OnLoginButtonClicked()
{
    spdlog::trace("LoginDialog::OnLoginButtonClicked() entered");
    ClearError();
    // Until the login is decided; DisplayError gives the button back.
    ui->loginButton->setEnabled(false);
    ui->loginButton->setText("Logging in...");

    // Refuse the login here: a session only starts from a complete identity
    // (LoginWithOAuth builds it, and writes it to the settings only once it
    // is valid), and once a session exists it cannot be replaced (the
    // shutdown invariant in network-redesign.md), so a failure after that
    // point would leave no way back to this dialog.
    if (ui->leagueComboBox->currentText().isEmpty()) {
        DisplayError("Please select a league before logging in.");
        return;
    }

    LoginWithOAuth();
}

void LoginDialog::LoginWithOAuth()
{
    spdlog::info("Starting OAuth authentication");
    const QDateTime now = QDateTime::currentDateTime();
    if (m_current_token.has_value()) {
        const OAuthToken &token = m_current_token.value();
        if (token.access_expiration && (now < *token.access_expiration)) {
            const auto identity = SessionIdentity::Make(token.username,
                                                        ui->realmComboBox->currentText(),
                                                        ui->leagueComboBox->currentText());
            if (!identity) {
                // A session cannot run without an account, realm and
                // league; the token's user name is the account.
                DisplayError(token.username.isEmpty()
                                 ? "The OAuth token does not name an account; "
                                   "please authenticate again."
                                 : "Please select a realm and a league before logging in.");
                return;
            }
            m_settings.setValue("account", identity->account());
            m_settings.setValue("realm", identity->realm());
            m_settings.setValue("league", identity->league());
            // The session is this token's account's from here on. A browser
            // login still waiting, started to authenticate as someone else,
            // must not finish under it (#408).
            m_oauth_manager.endLogin();
            emit LoginComplete(*identity);
        } else if (token.refresh_expiration && (now < *token.refresh_expiration)) {
            DisplayError("The OAuth token needs to be refreshed");
        } else {
            DisplayError("The OAuth token is not valid.");
        }
    } else {
        DisplayError("You are not authenticated.");
    }
}

void LoginDialog::OnOAuthAccessGranted(const OAuthToken &token)
{
    spdlog::trace("LoginDialog::OnOAuthAccessGranted() entered");
    ui->authenticateLabel->setText("You are authenticated as \"" + token.username + "\"");
    ui->authenticateButton->setText("Re-authenticate (as someone else).");
    ui->authenticateButton->setEnabled(true);
    m_current_token = token;
    UpdateLoginButton();
}

void LoginDialog::OnOAuthLoginFailed(const QString &reason)
{
    spdlog::trace("LoginDialog::OnOAuthLoginFailed() entered");
    DisplayAuthenticationError(reason);
}

void LoginDialog::OnOAuthStoredLoginFailed(const QString &reason)
{
    spdlog::trace("LoginDialog::OnOAuthStoredLoginFailed() entered");
    // The Authenticate button is left alone: a browser login the user has
    // started meanwhile is still waiting, and the button says so.
    DisplayError(reason);
}

void LoginDialog::OnLoginTabChanged(int index)
{
    spdlog::trace("LoginDialog::OnLoginTabChanged() entered");
    const QWidget *tab = ui->loginTabs->widget(index);
    if (tab == nullptr) {
        spdlog::error("LoginDialog: current tab widget is null");
        return;
    }
    const bool hide_options = false;
    const bool hide_advanced = !ui->advancedCheckBox->isChecked();
    const bool hide_error = ui->errorLabel->text().isEmpty();
    ui->optionsWidget->setHidden(hide_options);
    ui->advancedOptionsFrame->setHidden(hide_options || hide_advanced);
    ui->errorLabel->setHidden(hide_options || hide_error);
    ui->loginButton->setHidden(hide_options);
    UpdateLoginButton();
    m_settings.setValue("login_tab", tab->objectName());
};

// Another realm has its own leagues (#287): its list is asked for.
void LoginDialog::OnRealmChanged(const QString &realm)
{
    spdlog::trace("LoginDialog::OnRealmChanged() requesting leagues for realm {}", realm);
    RequestLeagues();
}

// The realm chosen again, after its list could not be had, asks again.
void LoginDialog::OnRealmActivated(int index)
{
    Q_UNUSED(index);
    if (!m_leagues_error.isEmpty() && !m_leagues_reply) {
        spdlog::trace("LoginDialog::OnRealmActivated() requesting leagues again");
        RequestLeagues();
    }
}

void LoginDialog::OnLeagueChanged(const QString &league)
{
    spdlog::trace("LoginDialog::OnLeagueChanged() entered");
    m_settings.setValue("league", league);
}

void LoginDialog::OnAdvancedCheckBoxChanged(Qt::CheckState state)
{
    spdlog::trace("LoginDialog: advanced options checkbox changed to {}", state);
    const bool checked = (state == Qt::Checked);
    const bool hide_options = false;
    ui->advancedOptionsFrame->setHidden(!checked || hide_options);
    m_settings.setValue("show_advanced_login_options", checked);
}

void LoginDialog::OnProxyCheckBoxChanged(Qt::CheckState state)
{
    spdlog::trace("LoginDialog: proxy checkbox changed to {}", state);
    const bool checked = (state == Qt::Checked);
    QNetworkProxyFactory::setUseSystemConfiguration(checked);
    m_settings.setValue("use_system_proxy", checked);
}

void LoginDialog::OnRememberMeCheckBoxChanged(Qt::CheckState state)
{
    spdlog::trace("LoginDialog: remember me checkbox changed to {}", state);
    const bool checked = (state == Qt::Checked);
    m_settings.setValue("remember_user", checked);
}

void LoginDialog::OnReportCrashesCheckBoxChanged(Qt::CheckState state)
{
    spdlog::trace("LoginDialog: crash reporting checkbox changed to {}", state);
    const bool checked = (state == Qt::Checked);
    QMessageBox msgbox(this);
    msgbox.setWindowTitle("Acquisition Crash Reporting");

    // Crash reporting is decided once, at startup (main.cpp), so a change here
    // takes effect the next time Acquisition runs. Correcting the box after a
    // refusal must not re-enter this slot.
    const auto restore_checkbox = [this](bool value) {
        const QSignalBlocker blocker(ui->reportCrashesCheckBox);
        ui->reportCrashesCheckBox->setChecked(value);
    };

    if (checked) {
        msgbox.setText("Crash reporting will start the next time Acquisition "
                       "runs.\n\nDo you want to enable crash reporting?");
        auto yes = msgbox.addButton("  Enable crash reports  ", msgbox.YesRole);
        msgbox.addButton("  Cancel  ", msgbox.NoRole);
        msgbox.exec();
        const bool enable_reporting = (msgbox.clickedButton() == yes);
        m_settings.setValue("report_crashes", enable_reporting);
        if (enable_reporting) {
            spdlog::info("Crash reporting will be enabled the next time Acquisition runs");
        } else {
            restore_checkbox(false);
        }

    } else {
        // Since crashpad cannot be stopped once it is started, acquisition
        // will have to be exited and restarted to disable crash reporting,
        // so make sure the user accepts and agrees to this.
        msgbox.setText("Acquisition will have to restart to disable crash "
                       "reporting.\n\nDo you want Acquisition to exit now and disable "
                       "crash reporting the next time it runs?");
        auto yes = msgbox.addButton("  Yes, exit now  ", msgbox.YesRole);
        msgbox.addButton("  No, keep crash reporting on  ", msgbox.NoRole);
        msgbox.exec();
        const bool disable_reporting = (msgbox.clickedButton() == yes);
        m_settings.setValue("report_crashes", !disable_reporting);
        if (disable_reporting) {
            spdlog::info("Crash reporting will be disabled the next time Acquisition runs");
            close();
        } else {
            restore_checkbox(true);
        }
    }
}

void LoginDialog::OnLoggingLevelChanged(const QString &level_name)
{
    const spdlog::level::level_enum level = spdlog::level::from_str(level_name.toStdString());
    spdlog::set_level(level);
    m_settings.setValue("log_level", level_name);
}

void LoginDialog::OnThemeChanged(const QString &theme)
{
    emit ChangeTheme(theme);
}

void LoginDialog::DisplayError(const QString &error)
{
    spdlog::error("LoginDialog: {}", error);
    m_login_error = error;
    ShowErrors();

    // OnLoginButtonClicked() disables the button while a login is in flight;
    // an error ends the attempt.
    ui->loginButton->setText("Login");
    UpdateLoginButton();
}

// The user tries again: the last attempt's error is over. The league list's
// error is not: Login stays unavailable for that reason, and the label still
// says so.
void LoginDialog::ClearError()
{
    m_login_error.clear();
    ShowErrors();
}

// The error label's text is set only here, from the errors that stand
// (#480): the last attempt's, then the league list's, so neither hides the
// other.
void LoginDialog::ShowErrors()
{
    QStringList errors;
    for (const QString &error : {m_login_error, m_leagues_error}) {
        if (!error.isEmpty()) {
            errors << error;
        }
    }
    ui->errorLabel->setText(errors.join('\n'));
    ui->errorLabel->setHidden(errors.isEmpty());
}

// Login is offered exactly when there is a token and the realm's league list
// is in: not while it is on its way, not after it could not be had.
void LoginDialog::UpdateLoginButton()
{
    ui->loginButton->setEnabled(m_current_token.has_value() && m_leagues_error.isEmpty()
                                && !m_leagues_reply);
}

// An authentication attempt is over without access: says why, and offers the
// button again as it was before the attempt.
void LoginDialog::DisplayAuthenticationError(const QString &error)
{
    DisplayError(error);
    ui->authenticateButton->setText(m_current_token ? "Re-authenticate (as someone else)."
                                                    : "Authenticate");
}

bool LoginDialog::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest) {
        setFixedSize(sizeHint());
    }
    return QDialog::event(e);
}
