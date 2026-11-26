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

#include <QTest>

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHostAddress>
#include <QPushButton>
#include <QTcpSocket>
#include <QUrlQuery>

#include <mutex>
#include <string>
#include <thread>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/spdlog.h>

#include "appfixture.h"
#include "datastore/sqlitedatastore.h"
#include "oauthtestaccess.h"
#include "replytimeout.h"
#include "testsuite.h"
#include "util/oauthtoken.h"
#include "util/spdlog_qt.h"

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
    void refreshWhileTheBrowserIsOutLeavesItWaiting();
    void failedRefreshWhileTheBrowserIsOutLeavesItWaiting();
    void loginDuringTheCodeExchangeKeepsTheSession();
    void secondAuthenticateDuringTheCodeExchange();
    void staleRefreshLeavesANewerLoginAlone();
    void redirectDuringARefreshIsTaken();
    void replayedRedirectIsNotExchangedAgain();
    void tokensInARedirectAreNotTaken();
    void heldLoginIsRefreshedOnTimeAfterAnAbandonedExchange();
    void exchangeWithNoAnswerEndsTheAttempt();
    void refreshWithNoAnswerAtStartIsSaid();
    void codeWithEscapesIsExchangedAsSent();
    void refreshWithoutARefreshTokenKeepsTheOneHeld();
    void refreshThatFailsIsTriedAgain();
    void leagueListFollowsTheRealm();

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
    // The held login's token once refreshed again, later in the run.
    const QString kRefreshedAccess = "fake-refreshed-access-token-aaaa";
    const QString kRefreshedRefresh = "fake-refreshed-refresh-token-bbbb";
    // The other account's token once refreshed.
    const QString kOtherRefreshedAccess = "fake-other-refreshed-access-token-cccc";
    const QString kOtherRefreshedRefresh = "fake-other-refreshed-refresh-token-dddd";

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

    // What the log says when an answer of the token endpoint arrives that
    // nothing waits for any more, and when a refresh of the login held fails.
    constexpr const char *kAnswerIgnoredLog
        = "OAuth: ignoring an answer of the token endpoint nothing waits for";
    constexpr const char *kRefreshFailedLog = "OAuth: the refresh failed";

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
        SqliteDataStore(globalStorePath(fixture))
            .Set("oauth_token", serializeTestToken(OAuthToken::fromTokens(fields)));
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
    // `query` is sent as written, already percent-encoded.
    QByteArray deliverRedirect(const QUrl &redirect, const QByteArray &query)
    {
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
        socket.write("GET " + redirect.path(QUrl::FullyEncoded).toLatin1() + '?' + query
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

    QByteArray deliverRedirect(const QUrl &redirect, const QUrlQuery &query)
    {
        return deliverRedirect(redirect, query.toString(QUrl::FullyEncoded).toLatin1());
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
        // first). The token it grants expires in two seconds, shorter than
        // the lead time, so the refresh comes at half its life: a second
        // after the grant.
        f.network->answer(
            [granted = false](QNetworkAccessManager::Operation,
                              const QUrl &url) mutable -> std::optional<ScriptedNetwork::Answer> {
                if (granted || (url.toString() != kTokenUrl)) {
                    return std::nullopt;
                }
                granted = true;
                return tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh, 2));
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
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains(kRefreshFailedLog); }, 10000));
    QVERIFY(error.isHidden());
    // A refusal is final (N46): the refresh is not tried again.
    QCOMPARE(OAuthTestAccess(fixture.access().oauth()).refreshDue().has_value(), false);
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

namespace {

    // Answers the token endpoint's requests in turn, one answer each. A
    // request past the last answer is left to the other rules.
    void answerTokenRequestsInTurn(AppFixture &f, const QList<ScriptedNetwork::Answer> &answers)
    {
        f.network->answer([answers, next = qsizetype(0)](QNetworkAccessManager::Operation,
                                                         const QUrl &url) mutable
                              -> std::optional<ScriptedNetwork::Answer> {
            if ((url.toString() != kTokenUrl) || (next >= answers.size())) {
                return std::nullopt;
            }
            return answers[next++];
        });
    }

    // The same answer, kept back until the scenario releases it.
    ScriptedNetwork::Answer heldBack(ScriptedNetwork::Answer answer)
    {
        answer.hold = true;
        return answer;
    }

    // Refreshes the login held now, as the refresh timer does when the
    // access token is about to expire. A scenario that calls it decides the
    // moment, instead of a token's expiry against the wall clock.
    void refreshNow(AppFixture &f)
    {
        OAuthTestAccess(f.access().oauth()).refreshNow();
    }

} // namespace

// The stored login's refresh is answered while the user is in the browser
// (#410). The refresh is taken, and the browser login goes on waiting: its
// redirect is answered, and the sign-in finished there, as another account,
// is the login held afterwards.
void AppLoginTest::refreshWhileTheBrowserIsOutLeavesItWaiting()
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

    // No event has been processed since the application was built: the
    // refresh is on its way and its answer has not come.
    QCOMPARE(tokenPosts(fixture).size(), 1);
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    // The refresh is granted. The dialog takes it as it takes any grant: the
    // held login is shown, and the button offers to re-authenticate.
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    QCOMPARE(authenticate.text(), QString("Re-authenticate (as someone else)."));
    QVERIFY(error.isHidden());

    // The browser login is still waiting: its redirect is taken.
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 2);
    QCOMPARE(AppFixture::formField(posts[1], "code"), kCode);
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QCOMPARE(stored->refresh_token, kOtherRefresh);
    QVERIFY(error.isHidden());

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
                         AppFixture::formField(posts[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh});
}

// A refresh later in the run fails while the user is in the browser (#410).
// That is no failure of the browser login, which goes on waiting: nothing
// is said, the button still reads "Authenticating...", and the redirect logs
// in.
void AppLoginTest::failedRefreshWhileTheBrowserIsOutLeavesItWaiting()
{
    AppFixture fixture([](AppFixture &f) {
        answerTokenRequestsInTurn(
            f,
            {// The first browser login.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             // Its refresh, refused once the user is back in the browser.
             tokenAnswer(400, kInvalidGrant),
             // The second browser login, as someone else.
             tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))});
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
    QVERIFY(deliverRedirect(first->redirect, kCode, first->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));

    // The user authenticates again, and the token is refreshed meanwhile.
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[1], "grant_type"), QString("refresh_token"));
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains(kRefreshFailedLog); }, 10000));
    QVERIFY(error.isHidden());
    QCOMPARE(authenticate.text(), QString(kAuthenticating));

    QVERIFY(deliverRedirect(second->redirect, kSecondCode, second->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 3);
    QCOMPARE(AppFixture::formField(posts[2], "code"), kSecondCode);
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QVERIFY(error.isHidden());

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), kOtherAccount);

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kCode,
                         kSecondCode,
                         AppFixture::formField(posts[0], "code_verifier"),
                         AppFixture::formField(posts[2], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh,
                         kOtherAccess,
                         kOtherRefresh});
}

// With a login held, the user authenticates as someone else, the redirect
// arrives, and Login is pressed before the code exchange is answered (#414).
// Starting the session abandons the exchange, and its answer, when it comes,
// is taken by nobody: the stored login and the session's account are
// unchanged, nothing is said, and the next refresh sends the held login's
// refresh token.
void AppLoginTest::loginDuringTheCodeExchangeKeepsTheSession()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {// The refresh at start.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             // The code exchange, as the other account, kept back until the
             // session has started.
             heldBack(
                 tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))),
             // The next refresh.
             tokenAnswer(200, grantedToken(kRefreshedAccess, kRefreshedRefresh))});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[1], "code"), kCode);

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    // The exchange is answered now, for the other account.
    QCOMPARE(fixture.network->release(), 1);
    // Ignored, or (before #414) taken: either is logged.
    QVERIFY(QTest::qWaitFor(
        [] {
            return loggedText().contains(kAnswerIgnoredLog) || loggedText().contains(kOtherAccount);
        },
        10000));
    auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, QString(AppFixture::kAccount));
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    // The next refresh is the held login's, and is taken.
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(AppFixture::formField(posts[2], "grant_type"), QString("refresh_token"));
    QCOMPARE(AppFixture::formField(posts[2], "refresh_token"), kFreshRefresh);
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));
    QCOMPARE(stored->username, QString(AppFixture::kAccount));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));
    // Nothing failed, so nothing was reported.
    QVERIFY(!loggedText().contains(kRefreshFailedLog));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh,
                         kRefreshedAccess,
                         kRefreshedRefresh});
}

// The redirect arrives, and the user presses Authenticate again before the
// code exchange is answered. The answer is the abandoned attempt's, and is
// refused: nothing is stored, nothing is said, and the new attempt goes on
// waiting. Its own redirect then logs in.
void AppLoginTest::secondAuthenticateDuringTheCodeExchange()
{
    AppFixture fixture([](AppFixture &f) {
        answerTokenRequestsInTurn(
            f,
            {// The first attempt's exchange, as the other account, kept back
             // until the second attempt has started.
             heldBack(
                 tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))),
             // The second attempt's.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh))});
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
    QVERIFY(deliverRedirect(first->redirect, kAbandonedCode, first->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 1; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "code"), kAbandonedCode);

    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 2);
    const auto second = checkAuthorizeUrl(m_browser.opened.last());
    QVERIFY(second);

    // The first attempt's exchange is answered now.
    QCOMPARE(fixture.network->release(), 1);
    QVERIFY(QTest::qWaitFor(
        [] {
            return loggedText().contains(kAnswerIgnoredLog) || loggedText().contains(kOtherAccount);
        },
        10000));
    QCOMPARE(storedToken(fixture).has_value(), false);
    QCOMPARE(authenticated.text(), QString("You must be authenticated to login."));
    QCOMPARE(authenticate.text(), QString(kAuthenticating));
    QVERIFY(error.isHidden());
    QVERIFY(!login.isEnabled());

    QVERIFY(deliverRedirect(second->redirect, kCode, second->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 2);
    QCOMPARE(AppFixture::formField(posts[1], "code"), kCode);
    QCOMPARE(challengeOf(AppFixture::formField(posts[1], "code_verifier")), second->challenge);
    QCOMPARE(authenticated.text(),
             QString("You are authenticated as \"%1\"").arg(AppFixture::kAccount));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kFreshRefresh);
    QVERIFY(error.isHidden());
    QVERIFY(!loggedText().contains(kRefreshFailedLog));

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));

    QCOMPARE(m_browser.opened.size(), 2);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kAbandonedCode,
                         kCode,
                         AppFixture::formField(posts[0], "code_verifier"),
                         AppFixture::formField(posts[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh,
                         kFreshAccess,
                         kFreshRefresh});
}

// The stored login's refresh at start is slow. Meanwhile the user signs in
// in the browser as someone else, that exchange is answered first, and Login
// starts the session for the new account. The refresh's answer, arriving
// last, renews a login no longer held: it is refused, and the stored login,
// the session's account and the next refresh stay the new account's.
void AppLoginTest::staleRefreshLeavesANewerLoginAlone()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {// The refresh at start, kept back until the session has started.
             heldBack(tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh))),
             // The browser sign-in, as the other account.
             tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount)),
             // The other account's next refresh.
             tokenAnswer(200,
                         grantedToken(kRefreshedAccess, kRefreshedRefresh, 36000, kOtherAccount))});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QCOMPARE(tokenPosts(fixture).size(), 1);
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));
    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), kOtherAccount);

    // The refresh at start is answered now, for the account held before.
    QCOMPARE(fixture.network->release(), 1);
    QVERIFY(QTest::qWaitFor(
        [] {
            return loggedText().contains(kAnswerIgnoredLog)
                   || loggedText().contains("OAuth: tokens received for "
                                            + QString(AppFixture::kAccount));
        },
        10000));
    auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QCOMPARE(stored->refresh_token, kOtherRefresh);
    QCOMPARE(fixture.access().identity().account(), kOtherAccount);

    // The next refresh is the other account's.
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(AppFixture::formField(posts[2], "refresh_token"), kOtherRefresh);
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));
    QCOMPARE(stored->username, kOtherAccount);
    QVERIFY(!loggedText().contains(kRefreshFailedLog));

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh,
                         kRefreshedAccess,
                         kRefreshedRefresh});
}

// With a login held, the user is in the browser when the token is
// refreshed, and the redirect arrives before the refresh is answered (#410).
// The redirect is taken and exchanged. Its answer, as another account, is
// the login held from then on; the refresh, answered last, renews the login
// held before and is refused.
void AppLoginTest::redirectDuringARefreshIsTaken()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {// The refresh at start.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             // The next refresh, kept back until the browser has come back.
             heldBack(tokenAnswer(200, grantedToken(kRefreshedAccess, kRefreshedRefresh))),
             // The browser sign-in, as the other account.
             tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))});
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
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));

    // The redirect, while the refresh is on its way: it is exchanged.
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(AppFixture::formField(posts[1], "grant_type"), QString("refresh_token"));
    QCOMPARE(AppFixture::formField(posts[2], "code"), kCode);
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));

    // The refresh is answered last, for the login held before: refused.
    QCOMPARE(fixture.network->release(), 1);
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains(kAnswerIgnoredLog); }, 10000));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QCOMPARE(stored->refresh_token, kOtherRefresh);
    QCOMPARE(authenticated.text(), QString("You are authenticated as \"%1\"").arg(kOtherAccount));
    QVERIFY(error.isHidden());

    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.access().identity().account(), kOtherAccount);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    posts = tokenPosts(fixture);
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(posts[2], "code_verifier"),
                         kRefreshedAccess,
                         kRefreshedRefresh,
                         kOtherAccess,
                         kOtherRefresh});
}

// The browser sends the redirect again (a reload of the page) while its code
// is being exchanged, once before a refresh is answered and once after. The
// first redirect was taken, and the code is not exchanged a second time.
void AppLoginTest::replayedRedirectIsNotExchangedAgain()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {// The refresh at start.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             // The browser sign-in, as the other account, kept back.
             heldBack(
                 tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))),
             // A refresh while the code is exchanged.
             tokenAnswer(200, grantedToken(kRefreshedAccess, kRefreshedRefresh)),
             // The next refresh, the other account's.
             tokenAnswer(200,
                         grantedToken(kOtherRefreshedAccess,
                                      kOtherRefreshedRefresh,
                                      36000,
                                      kOtherAccount))});
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
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));

    // The same redirect again: answered by the page, not exchanged.
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QTest::qWait(200);
    QCOMPARE(tokenPosts(fixture).size(), 2);

    // A refresh is answered while the code is exchanged; the same redirect
    // once more is still not exchanged.
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[2], "grant_type"), QString("refresh_token"));
    QVERIFY(QTest::qWaitFor(
        [&] {
            const auto stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QTest::qWait(200);
    QCOMPARE(tokenPosts(fixture).size(), 3);

    // The exchange is answered: the login is the other account's.
    QCOMPARE(fixture.network->release(), 1);
    QVERIFY(QTest::qWaitFor(
        [&] {
            return authenticated.text()
                   == QString("You are authenticated as \"%1\"").arg(kOtherAccount);
        },
        10000));
    auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->username, kOtherAccount);
    QVERIFY(error.isHidden());

    // The exchange's answer is the login held now, although a refresh was
    // answered meanwhile: the next refresh sends the other account's
    // refresh token.
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 4; }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(AppFixture::formField(posts[3], "refresh_token"), kOtherRefresh);
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kOtherRefreshedRefresh);
        },
        10000));
    QCOMPARE(stored->username, kOtherAccount);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kRefreshedAccess,
                         kRefreshedRefresh,
                         kOtherAccess,
                         kOtherRefresh,
                         kOtherRefreshedAccess,
                         kOtherRefreshedRefresh});
}

// A redirect that carries tokens instead of a code, with the attempt's state,
// as anything able to reach the loopback listener could send: no token is
// taken from it. Tokens are taken only from the token endpoint's answer.
void AppLoginTest::tokensInARedirectAreNotTaken()
{
    // No answer for the token endpoint: an exchange would be a surprise.
    AppFixture fixture;
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);

    QUrlQuery forged;
    forged.addQueryItem("access_token", kOtherAccess);
    forged.addQueryItem("refresh_token", kOtherRefresh);
    forged.addQueryItem("token_type", "bearer");
    forged.addQueryItem("expires_in", "36000");
    forged.addQueryItem("scope", "account:leagues account:characters account:stashes");
    forged.addQueryItem("username", kOtherAccount);
    forged.addQueryItem("state", authorize->state);
    QVERIFY(deliverRedirect(authorize->redirect, forged).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kNoTokenError));
    QCOMPARE(storedToken(fixture).has_value(), false);
    QCOMPARE(authenticated.text(), QString("You must be authenticated to login."));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QVERIFY(!login.isEnabled());
    QVERIFY(!loggedText().contains("OAuth: tokens received"));
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());

    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kOtherAccess, kOtherRefresh, authorize->state});
}

// After a browser login started and its exchange was abandoned (Login pressed
// before the answer), the login held is still refreshed on time: the refresh
// timer stays armed for the held token's expiry, and the refresh it sends is
// the held login's, with its refresh token.
void AppLoginTest::heldLoginIsRefreshedOnTimeAfterAnAbandonedExchange()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {// The refresh at start.
             tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             // The code exchange, kept back until the session has started.
             heldBack(
                 tokenAnswer(200, grantedToken(kOtherAccess, kOtherRefresh, 36000, kOtherAccount))),
             // The refresh of the login held.
             tokenAnswer(200, grantedToken(kRefreshedAccess, kRefreshedRefresh))});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    const OAuthTestAccess oauth(fixture.access().oauth());

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    QVERIFY(deliverRedirect(authorize->redirect, kCode, authorize->state).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    league.setCurrentIndex(league.findText(AppFixture::kLeague));
    login.click();
    QVERIFY2(fixture.access().hasSession(), qPrintable(error.text()));
    QCOMPARE(fixture.network->release(), 1);
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains(kAnswerIgnoredLog); }, 10000));
    QCOMPARE(tokenPosts(fixture).size(), 2);

    // The timer is armed for the held token: 36000 seconds less the lead
    // time of 300, counted from the refresh at start a moment ago (a coarse
    // timer may report a little more).
    const auto due = oauth.refreshDue();
    QVERIFY(due);
    QVERIFY2((*due > std::chrono::seconds(35700 - 60)) && (*due < std::chrono::seconds(35700 + 60)),
             qPrintable(QString::number(due->count())));

    // When it fires, the login held is refreshed.
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(AppFixture::formField(posts[2], "grant_type"), QString("refresh_token"));
    QCOMPARE(AppFixture::formField(posts[2], "refresh_token"), kFreshRefresh);
    std::optional<OAuthToken> stored;
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));
    QCOMPARE(stored->username, QString(AppFixture::kAccount));
    QCOMPARE(fixture.access().identity().account(), QString(AppFixture::kAccount));
    QVERIFY(!loggedText().contains(kRefreshFailedLog));
    // And the timer is armed again, for the new token.
    QVERIFY(oauth.refreshDue());

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kCode,
                         AppFixture::formField(posts[1], "code_verifier"),
                         kOtherAccess,
                         kOtherRefresh,
                         kRefreshedAccess,
                         kRefreshedRefresh});
}

namespace {

    // How long the scenarios below wait for a token request nobody answers,
    // in place of the request's own transfer timeout (kOAuthTokenTimeout).
    constexpr std::chrono::milliseconds kSilentWait{500};

    // Every request to the token endpoint carried its transfer timeout.
    void checkTokenRequestsCarryTheTimeout(const AppFixture &fixture)
    {
        for (const ScriptedNetwork::Request &request : fixture.network->requests()) {
            if (request.url.toString() == kTokenUrl) {
                QCOMPARE(request.transfer_timeout, kOAuthTokenTimeout);
            }
        }
    }

} // namespace

// The token endpoint takes the code exchange and never answers. The request
// carries a transfer timeout, Qt abandons it when that runs out, and the
// browser login ends: the dialog says no token was received and offers
// Authenticate again.
void AppLoginTest::exchangeWithNoAnswerEndsTheAttempt()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl, ScriptedNetwork::Answer{.silent = true});
        f.network->silent_wait = kSilentWait;
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
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 1; }, 10000));

    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kNoTokenError));
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    QVERIFY(authenticate.isEnabled());
    QVERIFY(!login.isEnabled());
    QCOMPARE(storedToken(fixture).has_value(), false);
    checkTokenRequestsCarryTheTimeout(fixture);

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    // The abandoned reply is not read for a body: Qt has closed it.
    QVERIFY(!loggedText().contains("device not open"));
    checkNoSecretLogged({kCode, AppFixture::formField(tokenPosts(fixture)[0], "code_verifier")});
}

// The refresh of the stored login at start is taken and never answered: it
// ends at its transfer timeout, and the dialog says the server could not be
// reached. The token is kept.
void AppLoginTest::refreshWithNoAnswerAtStartIsSaid()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        f.network->answer(kTokenUrl, ScriptedNetwork::Answer{.silent = true});
        f.network->silent_wait = kSilentWait;
    });
    LoginDialog &dialog = fixture.access().login();
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QCOMPARE(tokenPosts(fixture).size(), 1);
    QVERIFY(QTest::qWaitFor([&] { return !error.isHidden(); }, 10000));
    QCOMPARE(error.text(), QString(kUnreachableError));
    const auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->refresh_token, kStoredRefresh);
    QVERIFY(!login.isEnabled());
    QCOMPARE(authenticate.text(), QString(kAuthenticate));
    checkTokenRequestsCarryTheTimeout(fixture);

    QCOMPARE(m_browser.opened, QList<QUrl>());
    QCOMPARE(surprises(fixture), QString());
    // The abandoned reply is not read for a body: Qt has closed it.
    QVERIFY(!loggedText().contains("device not open"));
    checkNoSecretLogged({kStoredAccess, kStoredRefresh});
}

// The code the server puts in the redirect may hold characters that the
// query escapes ('+', '/', '%', '=', '&'; a '+' itself stands for a space).
// The token endpoint receives the code itself, decoded once and encoded once.
void AppLoginTest::codeWithEscapesIsExchangedAsSent()
{
    AppFixture fixture([](AppFixture &f) {
        f.network->answer(kTokenUrl, tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    authenticate.click();
    QCOMPARE(m_browser.opened.size(), 1);
    const auto authorize = checkAuthorizeUrl(m_browser.opened.first());
    QVERIFY(authorize);
    const QByteArray query = "code=a%2Bb%2Fc%3Dd%26e%25f%7Eg+h&state="
                             + QUrl::toPercentEncoding(authorize->state);
    QVERIFY(deliverRedirect(authorize->redirect, query).contains(kCallbackText));
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "code"), QString("a+b/c=d&e%f~g h"));

    QCOMPARE(surprises(fixture), QString());
}

// A refresh answer may leave out the refresh token (RFC 6749, 6): the one
// held stays, in memory and in the store, and the next refresh sends it.
void AppLoginTest::refreshWithoutARefreshTokenKeepsTheOneHeld()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        QJsonObject without = QJsonDocument::fromJson(grantedToken(kFreshAccess, QString())).object();
        without.remove("refresh_token");
        answerTokenRequestsInTurn(f,
                                  {tokenAnswer(200,
                                               QJsonDocument(without).toJson(
                                                   QJsonDocument::Compact)),
                                   tokenAnswer(200,
                                               grantedToken(kRefreshedAccess, kRefreshedRefresh))});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &login = child<QPushButton>(dialog, "loginButton");

    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    auto stored = storedToken(fixture);
    QVERIFY(stored);
    QCOMPARE(stored->access_token, kFreshAccess);
    QCOMPARE(stored->refresh_token, kStoredRefresh);

    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 2; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[1], "refresh_token"), kStoredRefresh);
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));

    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged(
        {kStoredAccess, kStoredRefresh, kFreshAccess, kRefreshedAccess, kRefreshedRefresh});
}

// A refresh during the run that gets no answer (the network is down, or the
// computer has just woken) is tried again soon, and the next one succeeds.
void AppLoginTest::refreshThatFailsIsTriedAgain()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        answerTokenRequestsInTurn(
            f,
            {tokenAnswer(200, grantedToken(kFreshAccess, kFreshRefresh)),
             ScriptedNetwork::Answer{.status = 0, .error = QNetworkReply::ConnectionRefusedError},
             tokenAnswer(200, grantedToken(kRefreshedAccess, kRefreshedRefresh))});
    });
    LoginDialog &dialog = fixture.access().login();
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &error = child<QLabel>(dialog, "errorLabel");
    const OAuthTestAccess oauth(fixture.access().oauth());

    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains(kRefreshFailedLog); }, 10000));
    QVERIFY(error.isHidden());
    // Tried again within the minute, not at the token's expiry.
    const auto due = oauth.refreshDue();
    QVERIFY(due);
    QVERIFY2(*due <= std::chrono::seconds(60), qPrintable(QString::number(due->count())));

    refreshNow(fixture);
    QVERIFY(QTest::qWaitFor([&] { return tokenPosts(fixture).size() == 3; }, 10000));
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[2], "refresh_token"), kFreshRefresh);
    std::optional<OAuthToken> stored;
    QVERIFY(QTest::qWaitFor(
        [&] {
            stored = storedToken(fixture);
            return stored && (stored->refresh_token == kRefreshedRefresh);
        },
        10000));
    QVERIFY(oauth.refreshDue() > std::chrono::seconds(35000));

    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess,
                         kStoredRefresh,
                         kFreshAccess,
                         kFreshRefresh,
                         kRefreshedAccess,
                         kRefreshedRefresh});
}

// The league list is the selected realm's (#287): the first request carries
// the saved realm, and choosing another realm asks again and shows that
// realm's list. A reply for a realm no longer chosen does not replace it.
//
// No captured reply of the league list exists in the repository. These are
// in the shape json::readLeagueList reads (`compact=1`: an id and a realm per
// league); the console lists are invented, and differ only so that one can be
// told from the other.
void AppLoginTest::leagueListFollowsTheRealm()
{
    static const QByteArray pc = R"([{"id":"Standard","realm":"pc"},{"id":"Hardcore","realm":"pc"}])";
    static const QByteArray xbox
        = R"([{"id":"Standard","realm":"xbox"},{"id":"Xbox Stale","realm":"xbox"}])";
    static const QByteArray sony = R"([{"id":"Standard","realm":"sony"},)"
                                   R"({"id":"Hardcore","realm":"sony"},)"
                                   R"({"id":"Solo Self-Found","realm":"sony"}])";
    AppFixture fixture([](AppFixture &f) {
        f.network->answer([](QNetworkAccessManager::Operation,
                             const QUrl &url) -> std::optional<ScriptedNetwork::Answer> {
            if ((url.host() != "api.pathofexile.com") || (url.path() != "/leagues")) {
                return std::nullopt;
            }
            const QString realm = QUrlQuery(url).queryItemValue("realm");
            if (realm == "pc") {
                return ScriptedNetwork::Answer{.body = pc};
            }
            if (realm == "xbox") {
                // Kept back until the user has moved on to another realm.
                return ScriptedNetwork::Answer{.body = xbox, .hold = true};
            }
            if (realm == "sony") {
                return ScriptedNetwork::Answer{.body = sony};
            }
            return std::nullopt;
        });
    });
    LoginDialog &dialog = fixture.access().login();
    auto &realm = child<QComboBox>(dialog, "realmComboBox");
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &error = child<QLabel>(dialog, "errorLabel");
    const auto items = [&league] {
        QStringList found;
        for (int i = 0; i < league.count(); ++i) {
            found << league.itemText(i);
        }
        return found;
    };
    const auto leagueRequests = [&fixture] {
        return fixture.network->requested(QNetworkAccessManager::GetOperation,
                                          "https://api.pathofexile.com/leagues");
    };
    const QString base = "https://api.pathofexile.com/leagues?type=main&compact=1&realm=";

    // The saved realm's list, with the saved league chosen.
    QCOMPARE(realm.currentText(), QString(AppFixture::kRealm));
    QVERIFY(QTest::qWaitFor([&] { return league.isEnabled(); }, 10000));
    QCOMPARE(leagueRequests(), QStringList{base + "pc"});
    QCOMPARE(items(), (QStringList{"Standard", "Hardcore"}));
    QCOMPARE(league.currentText(), QString(AppFixture::kLeague));

    // Another realm: its list is asked for, and the league box waits for it.
    realm.setCurrentText("xbox");
    QCOMPARE(leagueRequests(), (QStringList{base + "pc", base + "xbox"}));
    QVERIFY(!league.isEnabled());

    // And another, before the first has answered: this one's list is shown,
    // with the saved league still chosen, since it is in this list too.
    realm.setCurrentText("sony");
    QCOMPARE(leagueRequests(), (QStringList{base + "pc", base + "xbox", base + "sony"}));
    QVERIFY(QTest::qWaitFor([&] { return league.isEnabled(); }, 10000));
    const QStringList sony_leagues{"Standard", "Hardcore", "Solo Self-Found"};
    QCOMPARE(items(), sony_leagues);
    QCOMPARE(league.currentText(), QString(AppFixture::kLeague));

    // The answer for the realm left behind arrives late, and changes nothing.
    QCOMPARE(fixture.network->release(), qsizetype(1));
    QTest::qWait(100);
    QCOMPARE(items(), sony_leagues);
    QCOMPARE(league.currentText(), QString(AppFixture::kLeague));
    QVERIFY(league.isEnabled());
    QCOMPARE(error.text(), QString());

    QCOMPARE(surprises(fixture), QString());
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppLoginTest)
#include "tst_applogin.moc"
