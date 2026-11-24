// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Logging in, step 1 of the release smoke list (#380), on the real
// Application (appfixture.h): the login dialog, the OAuth manager and the
// token store are the application's own. One scenario per process.
//
// Nothing here reaches a real server or a real browser. The token endpoint
// and the league list are answered by the scripted network, and a request it
// does not answer fails the scenario. The browser is replaced, for the
// process, by a URL handler that records the address it was asked to open;
// the redirect the browser would make afterwards is sent by the test to the
// application's own loopback listener.
//
// No captured reply of the token endpoint exists in the repository. Its
// shape here is the fields OAuthToken::fromTokens reads, with the values the
// ground truth records: expires_in 36000, the scope reordered, the rate-limit
// headers of `token-request-limit` (N33), and a rejected refresh as HTTP 400
// `invalid_grant` (N46) with the error body of N34. The Content-Type is
// assumed to be application/json. A refresh that fails any other way was
// never captured: a 503 with a page for a body, a 400 with another error
// code, and a failure with no answer at all are invented.
//
// Nor was a login that fails in the browser ever captured. A denied
// authorization is the redirect RFC 6749 describes (4.1.2.1: `error`,
// `error_description`, `state`), and a refused code exchange is a 400
// `invalid_grant` in the same error body; both descriptions are invented,
// and so is the 503 with a page for a body.
//
// What Qt makes of an error reply from the token endpoint depends on its
// release (6.11.1 against 6.11.3 and 6.12.0; oauthmanager.cpp says how). The
// scenarios hold with each.

#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHostAddress>
#include <QPushButton>
#include <QTcpSocket>
#include <QUrlQuery>

#include <mutex>
#include <string>
#include <thread>

#include <glaze/glaze.hpp>
#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "appfixture.h"
#include "datastore/sqlitedatastore.h"
#include "testsuite.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/oauthtoken.h"

// Stands in for the user's browser: QDesktopServices hands it every http and
// https address the application asks to open, and it opens nothing.
class LoginTestBrowser : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void open(const QUrl &url) { opened << url; }
};

class AppLoginTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void storedTokenIsRefreshedAndLogsIn();
    void browserLoginStoresTheTokenAndLogsIn();
    void rejectedStoredTokenIsDroppedAndSaid();
    void unreachableServerKeepsTheStoredToken();
    void serverErrorKeepsTheStoredToken();
    void otherRefusalKeepsTheStoredToken();
    void storedLoginRefusedWhileTheBrowserIsOut();
    void refreshRejectedDuringARunIsLeftAlone();
    void failedReauthenticationKeepsTheLogin();
    void redirectWithWrongStateEndsTheAttempt();
    void deniedInTheBrowserIsShown();
    void refusedCodeExchangeIsShown();
    void failedCodeExchangeIsShown();
    void refusedReauthenticationKeepsTheLogin();
    void secondAuthenticateReplacesTheFirst();
    void redirectForAnAbandonedAttemptIsRefused();
    void loginEndsAWaitingBrowserLogin();
    void refusedLoginLeavesTheBrowserLoginWaiting();

private:
    std::shared_ptr<spdlog::logger> m_previous_logger;
    LoginTestBrowser m_browser;
};

namespace {

    // Invented, and recognisable in a log.
    const QString kStoredAccess = "fake-stored-access-token-1111";
    const QString kStoredRefresh = "fake-stored-refresh-token-2222";
    const QString kFreshAccess = "fake-fresh-access-token-3333";
    const QString kFreshRefresh = "fake-fresh-refresh-token-4444";
    const QString kCode = "fake-authorization-code-5555";
    const QString kForgedCode = "fake-forged-authorization-code-6666";
    const QString kSecondCode = "fake-second-authorization-code-7777";
    const QString kAbandonedCode = "fake-abandoned-authorization-code-8888";
    // Another account's login, as the browser sign-in of someone else gives.
    const QString kOtherAccount = "OtherAccount#1111";
    const QString kOtherAccess = "fake-other-access-token-9999";
    const QString kOtherRefresh = "fake-other-refresh-token-0000";

    constexpr const char *kTokenUrl = "https://www.pathofexile.com/oauth/token";
    constexpr const char *kAuthorizeUrl = "https://www.pathofexile.com/oauth/authorize";
    // What the browser shows once the listener has taken a redirect: the same
    // page whether the login then succeeds or not (#409).
    constexpr const char *kCallbackText = "You can close this page and return to Acquisition.";

    // What the Authenticate button reads: before a login, and while the
    // browser is out.
    constexpr const char *kAuthenticate = "Authenticate";
    constexpr const char *kAuthenticating = "Authenticating...";

    // What the dialog says when the browser's reply carries a state that is
    // not this attempt's.
    constexpr const char *kWrongStateError
        = "Authentication failed: the reply from the browser did not match this login attempt.";

    // When the token endpoint refuses the code, in the scenarios' words.
    constexpr const char *kRefusedCode
        = R"({"error":"invalid_grant","error_description":"Authorization code is invalid"})";
    constexpr const char *kRefusedCodeError
        = "Authentication failed: invalid_grant (Authorization code is invalid)";

    // And when the redirect was this attempt's but no token came of it, nor
    // an error the server named.
    constexpr const char *kNoTokenError
        = "Authentication failed: no token was received from Path of Exile.";

    // What the dialog says at start when the server refuses the stored
    // login, and when the refresh fails any other way.
    constexpr const char *kRefusedLoginError
        = "The stored login was refused by Path of Exile. Please authenticate again.";
    constexpr const char *kUnreachableError
        = "Could not refresh the stored login: the Path of Exile server could not be reached.";

    // The token endpoint's refusal of a refresh token (N46).
    constexpr const char *kInvalidGrant
        = R"({"error":"invalid_grant",)"
          R"("error_description":"Refresh token doesn't exist or has expired"})";

    // Everything logged while the scenario runs: the application's log and
    // Qt's own messages, in one text.
    std::mutex g_log_mutex;
    QString g_log;
    QtMessageHandler g_previous_handler = nullptr;

    void appendLog(const QString &line)
    {
        const std::lock_guard lock(g_log_mutex);
        g_log += line;
        g_log += '\n';
    }

    QString loggedText()
    {
        const std::lock_guard lock(g_log_mutex);
        return g_log;
    }

    void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        appendLog("qt: " + message);
        if (g_previous_handler) {
            g_previous_handler(type, context, message);
        }
    }

    // The secrets among `secrets` that the log holds.
    QStringList leaked(const QStringList &secrets)
    {
        const QString log = loggedText();
        QStringList found;
        for (const QString &secret : secrets) {
            if (!secret.isEmpty() && log.contains(secret)) {
                found << secret;
            }
        }
        return found;
    }

    // A token endpoint reply: the body, with the rate-limit headers the
    // endpoint sends with every answer, errors included (N33).
    ScriptedNetwork::Answer tokenAnswer(int status, const QByteArray &body)
    {
        return {.body = body,
                .status = status,
                .headers = {{"Content-Type", "application/json"},
                            {"X-Rate-Limit-Policy", "token-request-limit"},
                            {"X-Rate-Limit-Rules", "Ip"},
                            {"X-Rate-Limit-Ip", "60:30:30"},
                            {"X-Rate-Limit-Ip-State", "1:30:0"}},
                .error = (status == 200) ? QNetworkReply::NoError
                                         : QNetworkReply::ProtocolInvalidOperationError};
    }

    // A granted token, as the token endpoint sends it for either grant.
    QByteArray grantedToken(const QString &access,
                            const QString &refresh,
                            int expires_in = 36000,
                            const QString &username = AppFixture::kAccount)
    {
        return QJsonDocument(
                   QJsonObject{{"access_token", access},
                               {"expires_in", expires_in},
                               {"token_type", "bearer"},
                               {"scope", "account:leagues account:characters account:stashes"},
                               {"username", username},
                               {"sub", "00000000-0000-4000-8000-000000000000"},
                               {"refresh_token", refresh}})
            .toJson(QJsonDocument::Compact);
    }

    QString globalStorePath(const AppFixture &fixture)
    {
        return fixture.dir.filePath("data/" + SqliteDataStore::MakeFilename("", ""));
    }

    // Stores a token the way OAuthManager stores the one it receives, as
    // after an earlier run. Only before the application is built.
    void storeToken(AppFixture &fixture, const QString &access, const QString &refresh)
    {
        const QVariantMap fields
            = QJsonDocument::fromJson(grantedToken(access, refresh)).object().toVariantMap();
        std::string serialized;
        if (glz::write_json(OAuthToken::fromTokens(fields), serialized)) {
            qFatal("tst_applogin: cannot serialize a token");
        }
        SqliteDataStore(globalStorePath(fixture))
            .Set("oauth_token", QString::fromStdString(serialized));
    }

    // The token in the global store on disk, if there is one. Read from a
    // thread of its own: SqliteDataStore names its connection by file and
    // thread, and the application's store holds that name on this thread.
    std::optional<OAuthToken> storedToken(const AppFixture &fixture)
    {
        QString stored;
        std::thread([&] {
            stored = SqliteDataStore(globalStorePath(fixture)).Get("oauth_token", "");
        }).join();
        if (stored.isEmpty()) {
            return std::nullopt;
        }
        return json::readOAuthToken(stored.toUtf8());
    }

    // The forms posted to the token endpoint so far, as sent.
    QList<QByteArray> tokenPosts(const AppFixture &fixture)
    {
        QList<QByteArray> bodies;
        for (const ScriptedNetwork::Request &request : fixture.network->requests()) {
            if ((request.op == QNetworkAccessManager::PostOperation)
                && (request.url.toString() == kTokenUrl)) {
                bodies << request.body;
            }
        }
        return bodies;
    }

    // The names of a form's fields; a field without a value ("a&b=1") is
    // named all the same.
    QStringList fieldNames(const QByteArray &form)
    {
        QStringList names;
        for (const QByteArray &pair : form.split('&')) {
            const qsizetype equals = pair.indexOf('=');
            names << QString::fromUtf8((equals < 0) ? pair : pair.first(equals));
        }
        return names;
    }

    template<class Widget>
    Widget &child(const QWidget &parent, const char *name)
    {
        auto *widget = parent.findChild<Widget *>(name);
        if (!widget) {
            qFatal("tst_applogin: no widget named %s", name);
        }
        return *widget;
    }

    // What the browser does once the user has decided: it follows the
    // redirect to the application's loopback listener, with what the server
    // put in the query. Returns what the listener answered, empty if nothing
    // listened.
    QByteArray deliverRedirect(const QUrl &redirect, const QUrlQuery &query)
    {
        QUrl target = redirect;
        target.setQuery(query);

        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, quint16(redirect.port()));
        // A refused connection ends the wait too: then nothing listens.
        (void) QTest::qWaitFor(
            [&] {
                return (socket.state() == QAbstractSocket::ConnectedState)
                       || (socket.error() != QAbstractSocket::UnknownSocketError);
            },
            5000);
        if (socket.state() != QAbstractSocket::ConnectedState) {
            return {};
        }
        socket.write("GET " + target.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority)
                     + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(redirect.port())
                     + "\r\nConnection: close\r\n\r\n");
        QByteArray answer;
        // What was answered is the result, whether or not the listener closed.
        (void) QTest::qWaitFor(
            [&] {
                answer += socket.readAll();
                return socket.state() == QAbstractSocket::UnconnectedState;
            },
            5000);
        return answer + socket.readAll();
    }

    // The redirect after the user has approved: the code and the state.
    QByteArray deliverRedirect(const QUrl &redirect, const QString &code, const QString &state)
    {
        QUrlQuery query;
        query.addQueryItem("code", code);
        query.addQueryItem("state", state);
        return deliverRedirect(redirect, query);
    }

    // The S256 challenge of a PKCE verifier, as the authorize address carries it.
    QString challengeOf(const QString &verifier)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    // What a scenario must end without: a request the script did not
    // answer, or a dialog box.
    QString surprises(const AppFixture &fixture)
    {
        QStringList found;
        for (const QString &url : fixture.network->unanswered()) {
            found << "unanswered request: " + url;
        }
        for (const QString &dialog : fixture.dialogs) {
            found << "dialog: " + dialog;
        }
        return found.join('\n');
    }

    // The scenario's end: none of `secrets` was logged, and the check would
    // have seen one. The control logs the first secret through the
    // application's logger at its lowest level and the last through Qt.
    void checkNoSecretLogged(const QStringList &secrets)
    {
        QCOMPARE(leaked(secrets), QStringList());
        spdlog::trace("leak check control: {}", secrets.first());
        QCOMPARE(leaked(secrets), QStringList{secrets.first()});
        qDebug("leak check control: %s", qPrintable(secrets.last()));
        QCOMPARE(leaked(secrets), (QStringList{secrets.first(), secrets.last()}));
    }

    // The authorize address the browser was asked to open, checked; its
    // state, code challenge and redirect, for what follows.
    struct Authorize
    {
        QString state;
        QString challenge;
        QUrl redirect;
    };

    std::optional<Authorize> checkAuthorizeUrl(const QUrl &url)
    {
        bool ok = false;
        [&] {
            QCOMPARE(url.toString(QUrl::RemoveQuery), QString(kAuthorizeUrl));
            const QUrlQuery asked(url);
            QCOMPARE(asked.queryItemValue("response_type"), QString("code"));
            QCOMPARE(asked.queryItemValue("client_id"), QString("acquisition"));
            QStringList scopes = asked.queryItemValue("scope", QUrl::FullyDecoded).split(' ');
            scopes.sort();
            QCOMPARE(scopes,
                     (QStringList{"account:characters", "account:leagues", "account:stashes"}));
            QCOMPARE(asked.queryItemValue("code_challenge_method"), QString("S256"));
            QVERIFY(!asked.queryItemValue("code_challenge").isEmpty());
            QVERIFY(!asked.queryItemValue("state").isEmpty());
            const QUrl redirect(asked.queryItemValue("redirect_uri", QUrl::FullyDecoded));
            QCOMPARE(redirect.scheme(), QString("http"));
            QCOMPARE(redirect.host(), QString("127.0.0.1"));
            QCOMPARE(redirect.path(), QString("/auth/path-of-exile"));
            QVERIFY(redirect.port() > 0);
            ok = true;
        }();
        if (!ok) {
            return std::nullopt;
        }
        const QUrlQuery asked(url);
        return Authorize{.state = asked.queryItemValue("state", QUrl::FullyDecoded),
                         .challenge = asked.queryItemValue("code_challenge", QUrl::FullyDecoded),
                         .redirect = QUrl(asked.queryItemValue("redirect_uri", QUrl::FullyDecoded))};
    }

} // namespace

void AppLoginTest::initTestCase()
{
    // The application's logger, built as logging::init builds it (F42): the
    // log panel attaches through the hub, and the capture sees every level.
    auto capture = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [](const spdlog::details::log_msg &message) {
            appendLog(QString::fromUtf8(message.payload.data(), qsizetype(message.payload.size())));
        });
    auto hub = std::make_shared<spdlog::sinks::dist_sink_mt>();
    const std::vector<spdlog::sink_ptr> sinks{capture, hub};
    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    spdlog::register_logger(logger);
    m_previous_logger = spdlog::default_logger();
    spdlog::set_default_logger(logger);
    g_previous_handler = qInstallMessageHandler(captureQtMessage);
}

void AppLoginTest::cleanupTestCase()
{
    qInstallMessageHandler(g_previous_handler);
    spdlog::set_default_logger(m_previous_logger);
    spdlog::drop("main");
}

void AppLoginTest::init()
{
    // Before the application exists, so nothing it does can reach a browser.
    QDesktopServices::setUrlHandler("https", &m_browser, "open");
    QDesktopServices::setUrlHandler("http", &m_browser, "open");
}

void AppLoginTest::cleanup()
{
    QDesktopServices::unsetUrlHandler("https");
    QDesktopServices::unsetUrlHandler("http");
    if (QTest::currentTestFailed()) {
        fprintf(stderr, "--- the scenario's log ---\n%s\n", qPrintable(loggedText()));
    }
}

// A token stored by an earlier run is refreshed as the application starts;
// once the leagues are in, Login gives a session for the account the token
// names and the league the user picked.
void AppLoginTest::storedTokenIsRefreshedAndLogsIn()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        // The last run was another account's, in another league: the
        // session's account comes from the token, its league from the user.
        QSettings settings(f.dir.filePath("settings.ini"), QSettings::IniFormat);
        settings.setValue("account", "PreviousAccount#1111");
        settings.setValue("league", "Standard");
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    // One refresh grant, without the user, from the stored refresh token.
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "grant_type"), QString("refresh_token"));
    QCOMPARE(AppFixture::formField(posts[0], "refresh_token"), kStoredRefresh);
    QCOMPARE(AppFixture::formField(posts[0], "client_id"), QString("acquisition"));
    // The server refuses a refresh that carries an empty client_secret.
    QVERIFY2(!fieldNames(posts[0]).contains("client_secret"), posts[0].constData());
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    // The refresh token rotates on every refresh (N33): the new one is kept.
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QCOMPARE(stored->access_token, kFreshAccess);

    // The leagues arrive, and the user picks one.
    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QCOMPARE(league.currentText(), QString("Standard"));
    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();

    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));
    QCOMPARE(fixture.access().identity().realm(), QString(AppFixture::kRealm));
    QCOMPARE(fixture.access().identity().league(), QString(AppFixture::kLeague));
    QVERIFY(!dialog.isVisible());
    QVERIFY(fixture.window().isVisible());
    // The session is that account's in that league: its cached tabs load.
    QVERIFY(QTest::qWaitFor([&] { return fixture.tree().model()->rowCount() == 3; }, 10000));

    QCOMPARE(m_browser.opened, QList<QUrl>());
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess, kStoredRefresh, kFreshAccess, kFreshRefresh});
}

// No token: the user authenticates in the browser. The application opens the
// authorize page with PKCE and a loopback redirect, exchanges the code the
// redirect brings, stores the token, and Login gives a session.
void AppLoginTest::browserLoginStoresTheTokenAndLogsIn()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    // Nothing stored, so nothing is refreshed and Login waits.
    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(!login.isEnabled());
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QCOMPARE(storedToken(fixture).has_value(), false);

    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    // Nothing is exchanged before the browser comes back.
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());

    const QByteArray answered = deliverRedirect(authorize->redirect, kCode, authorize->state);
    QVERIFY2(answered.contains(kCallbackText), answered.constData());
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));

    // The code is exchanged once, with the verifier the challenge was made from.
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "grant_type"), QString("authorization_code"));
    QCOMPARE(AppFixture::formField(posts[0], "code"), kCode);
    QCOMPARE(AppFixture::formField(posts[0], "client_id"), QString("acquisition"));
    QCOMPARE(AppFixture::formField(posts[0], "redirect_uri"), authorize->redirect.toString());
    const QString verifier = AppFixture::formField(posts[0], "code_verifier");
    QVERIFY(!verifier.startsWith("(no field"));
    QCOMPARE(challengeOf(verifier), authorize->challenge);

    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->access_token, kFreshAccess);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QCOMPARE(stored->username, QString(AppFixture::kAccount));
    // The listener is closed once access is granted: the same redirect again
    // is not answered, and nothing more is exchanged.
    QCOMPARE(deliverRedirect(authorize->redirect, kCode, authorize->state), QByteArray());
    QCOMPARE(tokenPosts(fixture).size(), 1);

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));
    QCOMPARE(fixture.access().identity().league(), QString(AppFixture::kLeague));
    QVERIFY(QTest::qWaitFor([&] { return fixture.tree().model()->rowCount() == 3; }, 10000));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kCode, verifier, kFreshAccess, kFreshRefresh});
}

// The server rejects the stored token's refresh (N46, #402): the dialog says
// the stored login was refused, the token is removed from the store, and
// authenticating in the browser is offered. The next start, in the same data
// directory, has no token to refresh and asks the token endpoint for nothing.
void AppLoginTest::rejectedStoredTokenIsDroppedAndSaid()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        f.network->answer(kTokenUrl, tokenAnswer(400, kInvalidGrant));
    });
    {
        LoginDialog &dialog = fixture.access().login();
        auto &league = child<QComboBox>(dialog, "leagueComboBox");
        auto &login = child<QPushButton>(dialog, "loginButton");
        auto &error = child<QLabel>(dialog, "errorLabel");
        auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
        auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

        QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
        QCOMPARE(error.text(), QString(kRefusedLoginError));
        QCOMPARE(tokenPosts(fixture).size(), 1);
        QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "refresh_token"), kStoredRefresh);
        QCOMPARE(storedToken(fixture).has_value(), false);

        QVERIFY(!login.isEnabled());
        login.click(); // a disabled button takes no click
        QVERIFY(!fixture.access().hasSession());
        QVERIFY(authenticate.isEnabled());
        QCOMPARE(authenticate.text(), QString(kAuthenticate));
        QCOMPARE(authenticated.text(), QString("You must be authenticated to login."));
        QCOMPARE(surprises(fixture), QString());
    }

    // The next start. Its script has no answer for the token endpoint: a
    // refresh would be a surprise.
    fixture.restart();
    {
        LoginDialog &dialog = fixture.access().login();
        auto &league = child<QComboBox>(dialog, "leagueComboBox");
        auto &login = child<QPushButton>(dialog, "loginButton");
        auto &error = child<QLabel>(dialog, "errorLabel");

        QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
        QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
        QVERIFY(error.isHidden());
        QVERIFY(!login.isEnabled());
        QCOMPARE(surprises(fixture), QString());
    }

    QCOMPARE(m_browser.opened, QList<QUrl>());
    checkNoSecretLogged({kStoredAccess, kStoredRefresh});
}

namespace {

    // A refresh of the stored token that fails without the server refusing
    // the token (#402): the dialog says the server could not be reached, and
    // the token is kept. The next start refreshes it again, and with the
    // endpoint answering it logs in.
    void checkFailedRefreshKeepsTheToken(const ScriptedNetwork::Answer &failure,
                                         const LoginTestBrowser &browser)
    {
        AppFixture fixture([&](AppFixture &f) {
            storeToken(f, kStoredAccess, kStoredRefresh);
            f.network->answer(kTokenUrl, failure);
        });
        {
            LoginDialog &dialog = fixture.access().login();
            auto &league = child<QComboBox>(dialog, "leagueComboBox");
            auto &login = child<QPushButton>(dialog, "loginButton");
            auto &error = child<QLabel>(dialog, "errorLabel");
            auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

            QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
            QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
            QCOMPARE(error.text(), QString(kUnreachableError));
            QCOMPARE(tokenPosts(fixture).size(), 1);
            const auto stored = storedToken(fixture);
            QVERIFY(stored);
            QCOMPARE(stored->refresh_token, kStoredRefresh);
            QCOMPARE(stored->access_token, kStoredAccess);

            QVERIFY(!login.isEnabled());
            QVERIFY(!fixture.access().hasSession());
            QVERIFY(authenticate.isEnabled());
            QCOMPARE(authenticate.text(), QString(kAuthenticate));
            QCOMPARE(surprises(fixture), QString());
        }

        fixture.restart([](AppFixture &f) {
            f.network->answer(kTokenUrl,
                              tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
        });
        {
            LoginDialog &dialog = fixture.access().login();
            auto &login = child<QPushButton>(dialog, "loginButton");
            auto &error = child<QLabel>(dialog, "errorLabel");

            QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
            QCOMPARE(tokenPosts(fixture).size(), 1);
            QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "refresh_token"), kStoredRefresh);
            QVERIFY(error.isHidden());
            QCOMPARE(surprises(fixture), QString());
        }

        QCOMPARE(browser.opened, QList<QUrl>());
        checkNoSecretLogged({kStoredAccess, kStoredRefresh, kFreshAccess, kFreshRefresh});
    }

} // namespace

// The token endpoint cannot be reached: no status, no headers, no body.
void AppLoginTest::unreachableServerKeepsTheStoredToken()
{
    checkFailedRefreshKeepsTheToken({.status = 0, .error = QNetworkReply::ConnectionRefusedError},
                                    m_browser);
}

// The token endpoint answers with a server error, and not in JSON.
void AppLoginTest::serverErrorKeepsTheStoredToken()
{
    checkFailedRefreshKeepsTheToken({.body = "<html><body>Service Unavailable</body></html>",
                                     .status = 503,
                                     .headers = {{"Content-Type", "text/html"}},
                                     .error = QNetworkReply::ServiceUnavailableError},
                                    m_browser);
}

// The token endpoint refuses the request, but not the token: only
// `invalid_grant` says the stored login is no longer good.
void AppLoginTest::otherRefusalKeepsTheStoredToken()
{
    checkFailedRefreshKeepsTheToken(
        tokenAnswer(400,
                    R"({"error":"invalid_request",)"
                    R"("error_description":"The grant type was not specified in the request"})"),
        m_browser);
}

// The user presses Authenticate before the refresh of the stored login has
// been answered, and the answer is a refusal: the dialog says so and the
// token is dropped, but the browser login is not what failed. Its button
// still reads "Authenticating...", and its redirect logs in.
void AppLoginTest::storedLoginRefusedWhileTheBrowserIsOut()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        // Every token request but the first is the code exchange.
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
        // The first is the refresh (the rule added last is asked first).
        f.network->answer(
            [refused = false](QNetworkAccessManager::Operation,
                              const QUrl &url) mutable -> std::optional<ScriptedNetwork::Answer> {
                if (refused || (url.toString() != kTokenUrl)) {
                    return std::nullopt;
                }
                refused = true;
                return tokenAnswer(400, kInvalidGrant);
            });
    });
    LoginDialog &dialog = fixture.access().login();
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    // No event has been processed since the application was built: the
    // refresh is on its way and its answer has not come.
    QCOMPARE(tokenPosts(fixture).size(), 1);
    QVERIFY(error.isHidden());
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kRefusedLoginError));
    QCOMPARE(storedToken(fixture).has_value(), false);
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QVERIFY(!login.isEnabled());

    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 2);
    QCOMPARE(AppFixture::formField(posts[0], "grant_type"), QString("refresh_token"));
    QCOMPARE(AppFixture::formField(posts[1], "code"), kCode);
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh});
}

// A refresh the server rejects later in the run, when the token it granted
// is about to expire, is not the stored login's refresh at start: #402 leaves
// it as it was. Nothing is said, and the token stays in the store.
void AppLoginTest::refreshRejectedDuringARunIsLeftAlone()
{
    AppFixture fixture([](AppFixture &f) {
        // Every token request but the first is the refresh, and is refused.
        f.network->answer(kTokenUrl, tokenAnswer(400, kInvalidGrant));
        // The first is the code exchange (the rule added last is asked
        // first). The token it grants expires one second past the lead time
        // at which the application refreshes, so the refresh follows the
        // grant by a second.
        f.network->answer(
            [granted = false](QNetworkAccessManager::Operation,
                              const QUrl &url) mutable -> std::optional<ScriptedNetwork::Answer> {
                if (granted || (url.toString() != kTokenUrl)) {
                    return std::nullopt;
                }
                granted = true;
                return tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh, 301));
            });
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));

    // The refresh, and its rejection.
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[1], "grant_type"), QString("refresh_token"));
    QVERIFY(QTest::qWaitFor(
        [] { return loggedText().contains("OAuth: request failed: error 2 (ServerError)"); },
        10000));
    QVERIFY(error.isHidden());
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kCode,
                         AppFixture::formField(tokenPosts(fixture)[0], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh});
}

// With a login held (the stored token, refreshed at start), authenticating
// again fails: the dialog says why, the button offers to re-authenticate as
// before, and the login that was held still logs in.
void AppLoginTest::failedReauthenticationKeepsTheLogin()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    const QString reauthenticate = "Re-authenticate (as someone else).";

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    QCOMPARE(authenticate.text(), reauthenticate);

    authenticate.click();
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kForgedCode, authorize->state + "-forged")
                .contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kWrongStateError));
    QCOMPARE(authenticate.text(), reauthenticate);
    QVERIFY(authenticate.isEnabled());
    // Only the refresh at start went to the token endpoint.
    QCOMPARE(tokenPosts(fixture).size(), 1);
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);

    QVERIFY(login.isEnabled());
    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kForgedCode, kStoredAccess, kStoredRefresh, kFreshAccess, kFreshRefresh});
}

// A redirect whose state is not the one the application sent is not
// exchanged, and the attempt ends there (#401): the dialog says why and
// offers Authenticate again, and the listener is closed, so the genuine
// redirect that follows is not answered. Authenticating again logs in: the
// listener was there, and the check told the two redirects apart.
void AppLoginTest::redirectWithWrongStateEndsTheAttempt()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto first = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(first);

    const QString forged_state = first->state + "-forged";
    QVERIFY(deliverRedirect(first->redirect, kForgedCode, forged_state).contains(kCallbackText));
    // The application has seen the redirect and refused it, and says so.
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kWrongStateError));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QVERIFY(authenticate.isEnabled());
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QCOMPARE(storedToken(fixture).has_value(), false);
    QVERIFY(!login.isEnabled());
    QVERIFY(!fixture.access().hasSession());

    // The attempt is over: the genuine redirect finds nobody listening.
    QCOMPARE(deliverRedirect(first->redirect, kCode, first->state), QByteArray());
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QVERIFY(!login.isEnabled());

    // Authenticating again starts afresh, and only its code is exchanged.
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    QVERIFY(error.isHidden());
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);
    const QByteArray answered = deliverRedirect(second->redirect, kSecondCode, second->state);
    QVERIFY2(answered.contains(kCallbackText), answered.constData());
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "code"), kSecondCode);
    QVERIFY(error.isHidden());
    QVERIFY(!fixture.access().hasSession());

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kForgedCode,
                         kCode,
                         kSecondCode,
                         AppFixture::formField(posts[0], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh});
}

// The user refuses the request in the browser: the redirect carries an error
// instead of a code (#401). The dialog shows the server's error and offers
// Authenticate again; nothing is exchanged, and the attempt is over.
void AppLoginTest::deniedInTheBrowserIsShown()
{
    // No answer for the token endpoint: an exchange would be a surprise.
    AppFixture fixture;
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    QUrlQuery denied;
    denied.addQueryItem("error", "access_denied");
    denied.addQueryItem("error_description", "The user denied the request");
    denied.addQueryItem("state", authorize->state);
    QVERIFY(deliverRedirect(authorize->redirect, denied).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(),
             QString("Authentication failed: access_denied (The user denied the request)"));
    // The server's words are shown as written, not rendered.
    QCOMPARE(error.textFormat(), Qt::PlainText);
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QVERIFY(authenticate.isEnabled());
    QVERIFY(!login.isEnabled());
    QCOMPARE(storedToken(fixture).has_value(), false);

    // The attempt is over: a redirect with a code finds nobody listening.
    QCOMPARE(deliverRedirect(authorize->redirect, kCode, authorize->state), QByteArray());
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QVERIFY(!login.isEnabled());

    // An error in a redirect that is not the attempt's own is nobody's word:
    // the dialog reports the mismatch, not the text the redirect brought.
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    QVERIFY(error.isHidden());
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);
    QUrlQuery forged;
    forged.addQueryItem("error", "access_denied");
    forged.addQueryItem("error_description", "Forged words");
    forged.addQueryItem("state", second->state + "-forged");
    QVERIFY(deliverRedirect(second->redirect, forged).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kWrongStateError));
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QVERIFY(!login.isEnabled());
    QVERIFY(!fixture.access().hasSession());

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kCode, authorize->state, second->state});
}

// The token endpoint refuses the code the redirect brought (#401): the dialog
// shows the server's error and offers Authenticate again, and no token is
// stored. A redirect without a code, next, is told apart from that. The
// third attempt, which the endpoint grants, logs in.
void AppLoginTest::refusedCodeExchangeIsShown()
{
    AppFixture fixture(
        [](AppFixture &f) { f.network->answer(kTokenUrl, tokenAnswer(400, kRefusedCode)); });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto first = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(first);

    // The listener takes the redirect: only the exchange can refuse the code.
    QVERIFY(deliverRedirect(first->redirect, kCode, first->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kRefusedCodeError));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QVERIFY(authenticate.isEnabled());
    QCOMPARE(tokenPosts(fixture).size(), 1);
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "code"), kCode);
    const QString first_verifier = AppFixture::formField(tokenPosts(fixture)[0], "code_verifier");
    QCOMPARE(storedToken(fixture).has_value(), false);
    QVERIFY(!login.isEnabled());
    QCOMPARE(authenticated.text(), QString("You must be authenticated to login."));

    // The next attempt's redirect brings the state and no code. Nothing
    // is exchanged, and the dialog does not repeat the server's words from
    // the attempt before.
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    QVERIFY(error.isHidden());
    const auto codeless = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(codeless);
    QUrlQuery no_code;
    no_code.addQueryItem("state", codeless->state);
    QVERIFY(deliverRedirect(codeless->redirect, no_code).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kNoTokenError));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QCOMPARE(tokenPosts(fixture).size(), 1);

    // The endpoint grants the next attempt's code.
    fixture.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 3);
    QVERIFY(error.isHidden());
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);
    QVERIFY(deliverRedirect(second->redirect, kSecondCode, second->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 2);
    QCOMPARE(AppFixture::formField(posts[1], "code"), kSecondCode);
    // Each attempt has its own verifier.
    const QString second_verifier = AppFixture::formField(posts[1], "code_verifier");
    QCOMPARE(challengeOf(second_verifier), second->challenge);
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QVERIFY(error.isHidden());

    QCOMPARE(m_browser.opened.size(), 3);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged(
        {kCode, kSecondCode, first_verifier, second_verifier, kFreshAccess, kFreshRefresh});
}

// The code exchange fails without the server naming an error (a 503 with a
// page for a body): the dialog says no token was received, nothing is stored,
// and Authenticate is offered again.
void AppLoginTest::failedCodeExchangeIsShown()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl,
                          {.body = "<html><body>Service Unavailable</body></html>",
                           .status = 503,
                           .headers = {{"Content-Type", "text/html"}},
                           .error = QNetworkReply::ServiceUnavailableError});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kNoTokenError));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QCOMPARE(tokenPosts(fixture).size(), 1);
    QCOMPARE(storedToken(fixture).has_value(), false);
    QVERIFY(!login.isEnabled());
    QVERIFY(!fixture.access().hasSession());

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kCode, AppFixture::formField(tokenPosts(fixture)[0], "code_verifier")});
}

// With a login held (the stored token, refreshed at start), authenticating
// again is refused at the code exchange: the dialog says why, the button
// offers to re-authenticate as before, and the login that was held is still
// the one in the store and still logs in.
void AppLoginTest::refusedReauthenticationKeepsTheLogin()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        // Every token request but the first is the code exchange.
        f.network->answer(kTokenUrl, tokenAnswer(400, kRefusedCode));
        // The first is the refresh at start (the rule added last is asked
        // first).
        f.network->answer(
            [granted = false](QNetworkAccessManager::Operation,
                              const QUrl &url) mutable -> std::optional<ScriptedNetwork::Answer> {
                if (granted || (url.toString() != kTokenUrl)) {
                    return std::nullopt;
                }
                granted = true;
                return tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh));
            });
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    const QString reauthenticate = "Re-authenticate (as someone else).";

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    QCOMPARE(authenticate.text(), reauthenticate);

    authenticate.click();
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kRefusedCodeError));
    QCOMPARE(authenticate.text(), reauthenticate);
    QVERIFY(authenticate.isEnabled());
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 2);
    QCOMPARE(AppFixture::formField(posts[1], "code"), kCode);
    // The refused exchange has put nothing in the place of the login held.
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QCOMPARE(stored->access_token, kFreshAccess);

    QVERIFY(login.isEnabled());
    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh});
}

// The browser never comes back (#401): the button stays usable while it reads
// "Authenticating...", and pressing it again drops the attempt and opens the
// browser afresh. The first attempt's redirect, arriving late, finds nobody
// listening; the second attempt's logs in.
void AppLoginTest::secondAuthenticateReplacesTheFirst()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto first = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(first);
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QVERIFY(authenticate.isEnabled());

    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QVERIFY(authenticate.isEnabled());
    // The second attempt is another login: its own state, its own verifier.
    QVERIFY(second->state != first->state);
    QVERIFY(second->challenge != first->challenge);
    if (second->redirect.port() == first->redirect.port()) {
        // Then the late redirect reaches the second listener, which is the
        // case redirectForAnAbandonedAttemptIsRefused covers.
        QSKIP("the system gave the second listener the first one's port");
    }

    // The first attempt's redirect arrives late: it is not answered, and the
    // second attempt goes on waiting.
    QCOMPARE(deliverRedirect(first->redirect, kAbandonedCode, first->state), QByteArray());
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QVERIFY(error.isHidden());
    QVERIFY(!login.isEnabled());

    const QByteArray answered = deliverRedirect(second->redirect, kCode, second->state);
    QVERIFY2(answered.contains(kCallbackText), answered.constData());
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "code"), kCode);
    QCOMPARE(AppFixture::formField(posts[0], "redirect_uri"), second->redirect.toString());
    const QString verifier = AppFixture::formField(posts[0], "code_verifier");
    QCOMPARE(challengeOf(verifier), second->challenge);
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    QVERIFY(error.isHidden());

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kAbandonedCode, kCode, verifier, kFreshAccess, kFreshRefresh});
}

// The redirect of an attempt the user abandoned by pressing Authenticate
// again, arriving where the new attempt listens (as when the system gives
// both listeners the same port): its state is no longer the application's, so
// it is refused like any other wrong state and logs nobody in.
void AppLoginTest::redirectForAnAbandonedAttemptIsRefused()
{
    // No answer for the token endpoint: an exchange would be a surprise.
    AppFixture fixture;
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto first = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(first);
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);

    QVERIFY(deliverRedirect(second->redirect, kAbandonedCode, first->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kWrongStateError));
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QCOMPARE(storedToken(fixture).has_value(), false);
    QVERIFY(!login.isEnabled());
    QVERIFY(!fixture.access().hasSession());

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kAbandonedCode, first->state, second->state});
}

namespace {

    // The token endpoint for a login held at start and a browser sign-in as
    // someone else afterwards: the first request is the refresh of the
    // stored token, and every later one is granted as the other account.
    void answerRefreshThenOtherAccount(AppFixture &f)
    {
        f.network
            ->answer(kTokenUrl,
                     tokenAnswer(200,
                                 grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount)));
        // The rule added last is asked first.
        f.network->answer(
            [refreshed = false](QNetworkAccessManager::Operation,
                                const QUrl &url) mutable -> std::optional<ScriptedNetwork::Answer> {
                if (refreshed || (url.toString() != kTokenUrl)) {
                    return std::nullopt;
                }
                refreshed = true;
                return tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh));
            });
    }

} // namespace

// With a login held, the user starts to authenticate as someone else and
// then presses Login without finishing in the browser (#408). The session is
// the held account's, and starting it ends the browser login: the redirect
// that comes afterwards, with a code for the other account, is not answered,
// nothing is exchanged, and the stored token stays the session's own.
void AppLoginTest::loginEndsAWaitingBrowserLogin()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerRefreshThenOtherAccount(f);
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    authenticate.click();
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    // The browser sign-in is finished now, as the other account.
    QCOMPARE(deliverRedirect(authorize->redirect, kCode, authorize->state), QByteArray());
    // The session goes on as the held account's: its cached tabs load, and
    // only the refresh at start has gone to the token endpoint.
    QVERIFY(QTest::qWaitFor([&] { return fixture.tree().model()->rowCount() == 3; }, 10000));
    QCOMPARE(tokenPosts(fixture).size(), 1);
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "grant_type"), QString("refresh_token"));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, QString(AppFixture::kAccount));
    QCOMPARE(stored->access_token, kFreshAccess);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         kOtherAccess,
                         kOtherRefresh});
}

// A Login that is refused starts no session, so it leaves a waiting browser
// login alone: the button still reads "Authenticating...", and the sign-in
// finished afterwards, as the other account, is the login the next press of
// Login uses.
void AppLoginTest::refusedLoginLeavesTheBrowserLoginWaiting()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerRefreshThenOtherAccount(f);
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    // No league: Login is refused, and no session starts.
    league.setCurrentIndex(-1);
    QCOMPARE(league.currentText(), QString());
    login.click();
    QCOMPARE(error.text(), QString("Please select a league before logging in."));
    QVERIFY(!fixture.access().hasSession());
    QCOMPARE(authenticate.text(), QString(kAuthenticating));

    // The browser login is still waiting: its redirect is taken.
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[1], "code"), kCode);
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QCOMPARE(stored->refresh_token, kOtherRefresh);

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), kOtherAccount);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(tokenPosts(fixture)[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh});
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppLoginTest)
#include "tst_applogin.moc"
