// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/logindialog.h"
#include "ui_logindialog.h"

#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
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

constexpr int CLOUDFLARE_RATE_LIMITED = 1015;

LoginDialog::LoginDialog(const QDir &app_data_dir,
                         QSettings &settings,
                         NetworkManager &network_manager,
                         OAuthManager &oauth_manager,
                         DataStore &datastore)
    : QDialog(nullptr)
    , m_app_data_dir(app_data_dir)
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
    for (const auto &level_name : spdlog::level::level_string_views) {
        ui->loggingLevelComboBox->addItem(QString::fromUtf8(level_name));
    }

    // Display the data directory
    ui->userDirButton->setText(m_app_data_dir.absolutePath());

    // Hide the error message by default
    ui->errorLabel->setText("");
    ui->errorLabel->hide();
    ui->errorLabel->setStyleSheet("QLabel {color: red;}");

    // Disable the login button until we are ready to login.
    ui->loginButton->setEnabled(false);

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
    connect(ui->userDirButton, &QPushButton::pressed, this, &LoginDialog::OnUserDirButtonPushed);
    connect(ui->themeComboBox, &QComboBox::currentTextChanged, this, &LoginDialog::OnThemeChanged);

    // Listen for access from the OAuth manager.
    connect(&m_oauth_manager,
            &OAuthManager::grantAccess,
            this,
            &LoginDialog::OnOAuthAccessGranted);

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
            lg->trace("Login: clearing settings");
        }
        // "Remember me" governs credentials only: preserve the league and
        // realm selections, which are not credentials, across the clear.
        const QVariant league = m_settings.value("league");
        const QVariant realm = m_settings.value("realm");
        m_settings.clear();
        if (league.isValid()) {
            m_settings.setValue("league", league);
        }
        if (realm.isValid()) {
            m_settings.setValue("realm", realm);
        }
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
    // Make a non-API request to get the list of leagues. This currently uses
    // a legacy endpoint that is not rate limited and does not require authentication.
    QNetworkRequest request = QNetworkRequest(QUrl(QString(POE_LEAGUE_LIST_URL)));
    request.setTransferTimeout(kPoeApiTimeout);

    // Send the request and handle errors.
    spdlog::trace("LoginDialog::RequestLeagues() sending request: {}", request.url().toString());
    QNetworkReply *reply = m_network_manager.get(request);
    connect(reply, &QNetworkReply::finished, this, &LoginDialog::OnLeaguesReceived);
    connect(reply, &QNetworkReply::errorOccurred, this, [=, this](QNetworkReply::NetworkError code) {
        Q_UNUSED(code);
        DisplayError("Error requesting leagues: " + reply->errorString());
        ui->loginButton->setEnabled(false);
    });
    connect(reply, &QNetworkReply::sslErrors, this, [=, this](const QList<QSslError> &errors) {
        for (const auto &error : errors) {
            spdlog::error("SSL Error requesting leagues: {}", error.errorString());
        }
        DisplayError("SSL error fetching leagues");
        ui->loginButton->setEnabled(false);
    });
}

void LoginDialog::OnLeaguesReceived()
{
    spdlog::trace("LoginDialog::OnLeaguesReceived() reply recieved");
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();

    // Check for network errors.
    if (reply->error()) {
        spdlog::trace("LoginDialog::OnLeaguesReceived() reply error {}", reply->error());
        LeaguesRequestError(reply->errorString(), bytes);
        return;
    }

    // Parse the leagues.
    const auto leagues = json::readLeagueList(bytes);
    if (!leagues) {
        return;
    }

    // Get the league from settings.ini
    const QString saved_league = m_settings.value("league").toString();
    spdlog::trace("LoginDialog::OnLeaguesReceived() loaded leage from settings: {}", saved_league);

    bool use_saved_league = false;

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

    // Now that leagues have been received, start listening for changes.
    connect(ui->leagueComboBox, &QComboBox::currentTextChanged, this, &LoginDialog::OnLeagueChanged);

    // Now we can let the user login.
    //ui->loginButton->setEnabled(true);
}

void LoginDialog::LeaguesRequestError(const QString &error, const QByteArray &reply)
{
    spdlog::error("League reply was: {}", reply);
    DisplayError("Error requesting leagues: " + error);
    ui->loginButton->setEnabled(false);
}

void LoginDialog::OnAuthenticateButtonClicked()
{
    spdlog::trace("LoginDialog::OnAuthenticateButtonClicked() entered");
    ui->errorLabel->hide();
    ui->errorLabel->setText("");
    ui->authenticateButton->setEnabled(false);
    ui->authenticateButton->setText("Authenticating...");
    m_oauth_manager.initLogin();
}

void LoginDialog::OnLoginButtonClicked()
{
    spdlog::trace("LoginDialog::OnLoginButtonClicked() entered");
    ui->errorLabel->hide();
    ui->errorLabel->setText("");
    ui->loginButton->setEnabled(false);
    ui->loginButton->setText("Logging in...");

    const QString realm = ui->realmComboBox->currentText();
    const QString league = ui->leagueComboBox->currentText();

    // Refuse the login here rather than letting it reach UserSession, whose
    // constructor treats an unset league as fatal. That check has to stay
    // upstream of Application::InitUserSession(): once a session exists it
    // cannot be replaced (the shutdown invariant in network-redesign.md), so a
    // failure after construction leaves no way back to this dialog.
    if (league.isEmpty()) {
        DisplayError("Please select a league before logging in.");
        return;
    }

    m_settings.setValue("realm", realm);
    m_settings.setValue("league", league);

    LoginWithOAuth();
}

void LoginDialog::LoginWithOAuth()
{
    spdlog::info("Starting OAuth authentication");
    const QDateTime now = QDateTime::currentDateTime();
    if (m_current_token.has_value()) {
        const OAuthToken &token = m_current_token.value();
        if (token.access_expiration && (now < *token.access_expiration)) {
            m_settings.setValue("account", token.username);
            m_settings.setValue("realm", ui->realmComboBox->currentText());
            m_settings.setValue("league", ui->leagueComboBox->currentText());
            emit LoginComplete();
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
    if (ui->loginTabs->currentWidget() == ui->oauthTab) {
        ui->loginButton->setEnabled(true);
    }
    m_current_token = token;
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
    ui->loginButton->setEnabled(m_current_token.has_value());
    m_settings.setValue("login_tab", tab->objectName());
};

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

    if (checked) {
        // Before enabling crash reporting, make sure the user
        // understands and accepts that crash reporting cannot be
        // disabled without restarting acquisition.
        msgbox.setText("Once crash reporting is enabled, it cannot be "
                       "disabled without restarting Acquisition.\n\nDo you want to "
                       "enable crash reporting?");
        auto yes = msgbox.addButton("  Enable crash reports  ", msgbox.YesRole);
        msgbox.addButton("  Cancel  ", msgbox.NoRole);
        msgbox.exec();
        const bool enable_reporting = (msgbox.clickedButton() == yes);
        m_settings.setValue("report_crashes", enable_reporting);
        if (enable_reporting) {
            spdlog::info("TBD: crash reporting checkbox");
        } else {
            ui->reportCrashesCheckBox->setChecked(false);
        }

    } else {
        // Since crashpad cannot be stopped once it is started, acquisition
        // will have to be exited and restarted to disable crash reporting,
        // so make sure the user accepts and agrees to this.
        msgbox.setText("Acquisition will have to restart to disable crash "
                       "reporting.\n\nDo you want Acquisition to exit now and disable "
                       "crash reporting the next time it runs?");
        auto yes = msgbox.addButton("  Yes, exit now  ", msgbox.YesRole);
        msgbox.addButton("  No, continue without crash reporting  ", msgbox.NoRole);
        msgbox.exec();
        const bool disable_reporting = (msgbox.clickedButton() == yes);
        m_settings.setValue("report_crashes", !disable_reporting);
        if (disable_reporting) {
            close();
        } else {
            ui->reportCrashesCheckBox->setChecked(true);
        }
    }
}

void LoginDialog::OnUserDirButtonPushed()
{
    const QString current_dir = QFileInfo(m_settings.fileName()).absolutePath();
    const QString parent_dir = QFileInfo(current_dir).absolutePath();
    const QString new_dir = QFileDialog::getExistingDirectory(this,
                                                              "Select the user directory",
                                                              parent_dir);
    if (new_dir != current_dir) {
        ui->userDirButton->setText(new_dir);
        emit ChangeUserDir(new_dir);
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
    ui->errorLabel->setText(error);
    ui->errorLabel->show();

    // OnLoginButtonClicked() disables the button while a login is in flight.
    // Restoring the label without re-enabling it left the dialog looking ready
    // but inert after any failed attempt. Callers that want it to stay
    // disabled (the league request errors) disable it again after this call.
    ui->loginButton->setText("Login");
    ui->loginButton->setEnabled(true);
}

bool LoginDialog::event(QEvent *e)
{
    if (e->type() == QEvent::LayoutRequest) {
        setFixedSize(sizeHint());
    }
    return QDialog::event(e);
}
