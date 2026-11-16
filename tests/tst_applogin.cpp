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
// assumed to be application/json.

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
    void rejectedStoredTokenGivesNoSession();
    void redirectWithWrongStateIsIgnored();

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

    constexpr const char *kTokenUrl = "https://www.pathofexile.com/oauth/token";
    constexpr const char *kAuthorizeUrl = "https://www.pathofexile.com/oauth/authorize";
    constexpr const char *kAuthorizedText = "Acquisition has been authorized.";

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
    QByteArray grantedToken(const QString &access, const QString &refresh)
    {
        return QJsonDocument(
                   QJsonObject{{"access_token", access},
                               {"expires_in", 36000},
                               {"token_type", "bearer"},
                               {"scope", "account:leagues account:characters account:stashes"},
                               {"username", AppFixture::kAccount},
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

    // What the browser does once the user has approved: it follows the
    // redirect to the application's loopback listener, with the code and the
    // state. Returns what the listener answered, empty if nothing listened.
    QByteArray deliverRedirect(const QUrl &redirect, const QString &code, const QString &state)
    {
        QUrl target = redirect;
        QUrlQuery query;
        query.addQueryItem("code", code);
        query.addQueryItem("state", state);
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
    QVERIFY2(answered.contains(kAuthorizedText), answered.constData());
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
    QCOMPARE(QString::fromLatin1(
                 QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)),
             authorize->challenge);

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

// The server rejects the stored token's refresh (N46): Login stays disabled,
// no session is built, and authenticating in the browser is still offered.
void AppLoginTest::rejectedStoredTokenGivesNoSession()
{
    AppFixture fixture([](AppFixture &f) {
        storeToken(f, kStoredAccess, kStoredRefresh);
        f.network->answer(
            kTokenUrl,
            tokenAnswer(400,
                        R"({"error":"invalid_grant",)"
                        R"("error_description":"Refresh token doesn't exist or has expired"})"));
    });
    LoginDialog &dialog = fixture.access().login();
    auto &league = child<QComboBox>(dialog, "leagueComboBox");
    auto &login = child<QPushButton>(dialog, "loginButton");
    auto &authenticate = child<QPushButton>(dialog, "authenticateButton");
    auto &authenticated = child<QLabel>(dialog, "authenticateLabel");

    QVERIFY(QTest::qWaitFor([&] { return league.count() == 2; }, 10000));
    QCOMPARE(tokenPosts(fixture).size(), 1);
    QCOMPARE(AppFixture::formField(tokenPosts(fixture)[0], "refresh_token"), kStoredRefresh);
    // The refusal has been taken in: Qt reports a failed request and does
    // not read the error body.
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains("OAuth: request failed"); }, 10000));

    QVERIFY(!login.isEnabled());
    login.click(); // a disabled button takes no click
    QVERIFY(!fixture.access().hasSession());
    QVERIFY(authenticate.isEnabled());
    QCOMPARE(authenticated.text(), QString("You must be authenticated to login."));

    QCOMPARE(m_browser.opened, QList<QUrl>());
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kStoredAccess, kStoredRefresh});
}

// A redirect whose state is not the one the application sent is not
// exchanged, and no token or session comes of it. The genuine redirect that
// follows is exchanged: the listener was there and the check tells them apart.
void AppLoginTest::redirectWithWrongStateIsIgnored()
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

    const QString forged_state = authorize->state + "-forged";
    QVERIFY(!deliverRedirect(authorize->redirect, kForgedCode, forged_state).isEmpty());
    // The application has seen the redirect and refused it.
    QVERIFY(QTest::qWaitFor([] { return loggedText().contains("OAuth: request failed"); }, 10000));
    QCOMPARE(tokenPosts(fixture), QList<QByteArray>());
    QCOMPARE(storedToken(fixture).has_value(), false);
    QVERIFY(!login.isEnabled());
    QVERIFY(!fixture.access().hasSession());

    // The genuine redirect is still taken, and only its code is exchanged.
    const QByteArray answered = deliverRedirect(authorize->redirect, kCode, authorize->state);
    QVERIFY2(answered.contains(kAuthorizedText), answered.constData());
    QVERIFY(QTest::qWaitFor([&] { return login.isEnabled(); }, 10000));
    const QList<QByteArray> posts = tokenPosts(fixture);
    QCOMPARE(posts.size(), 1);
    QCOMPARE(AppFixture::formField(posts[0], "code"), kCode);
    QVERIFY(!fixture.access().hasSession());

    QCOMPARE(m_browser.opened.size(), 1);
    QCOMPARE(surprises(fixture), QString());
    checkNoSecretLogged({kForgedCode,
                         kCode,
                         AppFixture::formField(posts[0], "code_verifier"),
                         kFreshAccess,
                         kFreshRefresh});
}

ACQ_TEST_GUI_SCENARIO_MAIN(AppLoginTest)
#include "tst_applogin.moc"
