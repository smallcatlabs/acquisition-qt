// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "util/oauthmanager.h"

#include <QAbstractOAuth>
#include <QByteArray>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QOAuth2AuthorizationCodeFlow>
#include <QOAuthHttpServerReplyHandler>
#include <QRandomGenerator>
#include <QSet>

#include <array>
#include <functional>

#include "datastore/datastore.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    // Hard-coded settings for acquisition.
    constexpr const char *AUTHORIZATION_URL = "https://www.pathofexile.com/oauth/authorize";
    constexpr const char *TOKEN_URL = "https://www.pathofexile.com/oauth/token";
    constexpr const char *CLIENT_ID = "acquisition";
    constexpr const std::array SCOPE = {"account:leagues", "account:stashes", "account:characters"};

    // Acquisition as currently approved uses a plain HTTP callback.
    // This should be changed to HTTPS or a private URI scheme at some point.
    constexpr const char *CALLBACK_HOST = "127.0.0.1";
    constexpr const char *CALLBACK_PATH = "/auth/path-of-exile";

    constexpr const std::chrono::seconds REFRESH_LEAD_TIME{300};

    // The error code with which the token endpoint refuses a token it was
    // sent (RFC 6749, 5.2; N46), and how much of a reply is read for the
    // error it names.
    constexpr const char *TOKEN_REFUSED = "invalid_grant";
    constexpr qint64 ERROR_BODY_LIMIT = 4096;

    // The request attribute that says which stage of the flow
    // (QAbstractOAuth::Stage) a request to the token endpoint belongs to.
    constexpr QNetworkRequest::Attribute STAGE_ATTRIBUTE = QNetworkRequest::User;

    // This list is complete as of Qt 6.9.1:
    // See https://doc.qt.io/qt-6/qabstractoauth.html#Error-enum
    // clang-format off
    constexpr std::array<std::pair<QAbstractOAuth::Error, const char *>, 8> KNOWN_OAUTH_ERRORS{{
         {QAbstractOAuth::Error::NoError, "NoError"},                // 0	No error has ocurred.
         {QAbstractOAuth::Error::NetworkError, "NetworkError"},      // 1	Failed to connect to the server.
         {QAbstractOAuth::Error::ServerError, "ServerError"},        // 2	The server answered the request with an error, or its response was not successfully received (for example, due to a state mismatch).
         {QAbstractOAuth::Error::OAuthTokenNotFoundError, "OAuthTokenNotFoundError"},             // 3	The server's response to a token request provided no token identifier.
         {QAbstractOAuth::Error::OAuthTokenSecretNotFoundError, "OAuthTokenSecretNotFoundError"}, // 4	The server's response to a token request provided no token secret.
         {QAbstractOAuth::Error::OAuthCallbackNotVerified, "OAuthCallbackNotVerified"},           // 5	The authorization server has not verified the supplied callback URI in the request. This usually happens when the provided callback does not match with the callback supplied during client registration.
         {QAbstractOAuth::Error::ClientError, "ClientError"},        // (since Qt 6.9) 6	An error that is attributable to the client application (e.g. missing configuration or attempting a request in a state where it's not allowed). Currently used by QOAuth2DeviceAuthorizationFlow.
         {QAbstractOAuth::Error::ExpiredError, "ExpiredError"}}      // (since Qt 6.9) 7	A token has expired. Currently used by QOAuth2DeviceAuthorizationFlow.
    };
    // clang-format on

    // A state for one login attempt: 32 bytes from the system's generator,
    // in characters RFC 6749 allows in a state.
    QString makeState()
    {
        std::array<quint32, 8> words;
        QRandomGenerator::system()->fillRange(words.data(), qsizetype(words.size()));
        const QByteArray bytes(reinterpret_cast<const char *>(words.data()), sizeof(words));
        return QString::fromLatin1(
            bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    // The line for the user when the server answered a login with an error.
    QString serverErrorReason(const QString &error, const QString &description)
    {
        return description.isEmpty()
                   ? QString("Authentication failed: %1").arg(error)
                   : QString("Authentication failed: %1 (%2)").arg(error, description);
    }

    // Qt's loopback handler, which also shows every reply of the token
    // endpoint to `note` before Qt handles it, and calls `note` with nullptr
    // once Qt has. What Qt makes of an error reply depends on its release:
    // 6.11.1 reports a failed request and reads no body; 6.11.3 and 6.12.0
    // read the body and report the error it names. None says whether the
    // server answered at all, or with what status. Reading the reply here
    // gives the user the same line with each, and tells a refused token from
    // a server that could not be reached.
    class ReplyHandler : public QOAuthHttpServerReplyHandler
    {
    public:
        ReplyHandler(std::function<void(QNetworkReply *)> note, QObject *parent)
            : QOAuthHttpServerReplyHandler(parent)
            , m_note(std::move(note))
        {}

        void networkReplyFinished(QNetworkReply *reply) override
        {
            m_note(reply);
            QOAuthHttpServerReplyHandler::networkReplyFinished(reply);
            m_note(nullptr);
        }

    private:
        std::function<void(QNetworkReply *)> m_note;
    };

}; // namespace

OAuthManager::OAuthManager(NetworkManager &network_manager, DataStore &datastore, QObject *parent)
    : QObject(parent)
    , m_network_manager(network_manager)
    , m_data(datastore)
{
    // Create the the reply handler.
    m_handler = new ReplyHandler([this](QNetworkReply *reply) { noteTokenReply(reply); }, this);
    m_handler->setCallbackHost(CALLBACK_HOST);
    m_handler->setCallbackPath(CALLBACK_PATH);
    m_handler->setCallbackText("Acquisition has been authorized.");

    // Setup a connection to grab the details of a token before granted() has been received.
    connect(m_handler,
            &QAbstractOAuthReplyHandler::tokensReceived,
            this,
            &OAuthManager::receiveToken);

    // Create the scope tokens.
    const QSet<QByteArray> scope_tokens{SCOPE.begin(), SCOPE.end()};

    // Setup OAuth for the Path of Exile API.
    m_oauth = new QOAuth2AuthorizationCodeFlow(this);
    m_oauth->setNetworkAccessManager(&network_manager);
    m_oauth->setAuthorizationUrl(QUrl{AUTHORIZATION_URL});
    m_oauth->setTokenUrl(QUrl{TOKEN_URL});
    m_oauth->setClientIdentifier(CLIENT_ID);
    m_oauth->setPkceMethod(QOAuth2AuthorizationCodeFlow::PkceMethod::S256);
    m_oauth->setRequestedScopeTokens(scope_tokens);
    m_oauth->setReplyHandler(m_handler);
    m_oauth->setAutoRefresh(true);
    m_oauth->setRefreshLeadTime(REFRESH_LEAD_TIME);

    // Setup a hack to make token refresh work correctly.
    m_oauth->setModifyParametersFunction(
        [](QAbstractOAuth::Stage stage, QMultiMap<QString, QVariant> *parameters) {
            // During token refresh, if the client_secret parameter is present but
            // empty, then the POE server will give us an error (as of 3.26).
            if (stage == QAbstractOAuth::Stage::RefreshingAccessToken) {
                parameters->remove("client_secret");
            }
        });

    // Every request to the token endpoint says which stage it belongs to, so
    // that its reply is known for a refresh's or a code exchange's. The
    // flow's status does not say: starting a browser login resets it while a
    // refresh is still in flight.
    m_oauth->setNetworkRequestModifier(this,
                                       [](QNetworkRequest &request, QAbstractOAuth::Stage stage) {
                                           request.setAttribute(STAGE_ATTRIBUTE,
                                                                static_cast<int>(stage));
                                       });

    // Connect the oauth code flow and add error logging.
    connect(m_oauth, &QAbstractOAuth::authorizeWithBrowser, this, &QDesktopServices::openUrl);
    connect(m_oauth, &QAbstractOAuth::granted, this, &OAuthManager::receiveGrant);
    connect(m_oauth, &QAbstractOAuth::requestFailed, this, &OAuthManager::onRequestFailure);
    connect(m_oauth,
            &QAbstractOAuth2::serverReportedErrorOccurred,
            this,
            &OAuthManager::onServerError);
    // Connected before the first grant(), where Qt connects its own handling
    // of the redirect, so this slot sees a redirect before Qt judges it.
    connect(m_oauth,
            &QAbstractOAuth2::authorizationCallbackReceived,
            this,
            &OAuthManager::receiveCallback);

    // Check for an existing token.
    const QString token_str = m_data.Get("oauth_token", "");
    if (!token_str.isEmpty()) {
        const OAuthToken token = OAuthToken::fromJson(token_str);
        spdlog::info("OAuth: refreshing token for '{}'", token.username);
        m_oauth->setRefreshToken(token.refresh_token);
        m_oauth->refreshTokens();
    }
}

void OAuthManager::onRequestFailure(const QAbstractOAuth::Error error)
{
    const char *name = "unknown error";
    for (const auto &[known_error, known_name] : KNOWN_OAUTH_ERRORS) {
        if (error == known_error) {
            name = known_name;
            break;
        }
    }
    spdlog::error("OAuth: request failed: error {} ({})", static_cast<int>(error), name);

    // A refresh that failed. Before anything has been granted in this run it
    // can only be the refresh of the stored login, at start (#402); a later
    // one, of a token granted in this run, is left at the log line above.
    // Either way it is no failure of a browser login that may be waiting.
    if (m_token_reply && m_token_reply->refresh) {
        if (m_token.access_token.isEmpty()) {
            failStoredLogin(*m_token_reply);
        }
        return;
    }

    // Qt names no cause beyond `error`. A redirect it refused for its state
    // was noted by receiveCallback, and an error the token endpoint named by
    // noteTokenReply; anything else left the login without a token, whether
    // the redirect brought no code or the exchange got no usable answer.
    if (!m_login_in_progress) {
        return;
    }
    if (m_state_mismatch) {
        failLogin("Authentication failed: the reply from the browser did not match this login "
                  "attempt.");
    } else if (m_token_reply && !m_token_reply->error.isEmpty()) {
        failLogin(serverErrorReason(m_token_reply->error, m_token_reply->description));
    } else {
        failLogin("Authentication failed: no token was received from Path of Exile.");
    }
}

void OAuthManager::onServerError(const QString &error,
                                 const QString &errorDescription,
                                 const QUrl &uri)
{
    spdlog::error("Oauth: server reported error: '{}' ({}): {}",
                  error,
                  errorDescription,
                  uri.toDisplayString());

    // An error the token endpoint gave a refresh (Qt 6.11.3 and 6.12.0
    // report it here first) is not the browser login's: requestFailed, which
    // Qt emits next, deals with the refresh.
    if (m_token_reply && m_token_reply->refresh) {
        return;
    }

    // The error a redirect brought is shown only when the redirect is this
    // login's. With another state it is anybody's text: requestFailed then
    // reports the mismatch instead.
    if (m_login_in_progress && !m_state_mismatch) {
        failLogin(serverErrorReason(error, errorDescription));
    }
}

void OAuthManager::receiveCallback(const QVariantMap &data)
{
    // Qt refuses a redirect whose state is not the one it sent, and reports
    // that as a ServerError like any other.
    m_state_mismatch = (data.value("state").toString() != m_oauth->state());
}

void OAuthManager::noteTokenReply(QNetworkReply *reply)
{
    if (!reply) {
        m_token_reply.reset();
        return;
    }
    // peek(), not read(): the body is Qt's to read.
    const QJsonObject body = QJsonDocument::fromJson(reply->peek(ERROR_BODY_LIMIT)).object();
    m_token_reply = TokenReply{
        .refresh = (reply->request().attribute(STAGE_ATTRIBUTE).toInt()
                    == static_cast<int>(QAbstractOAuth::Stage::RefreshingAccessToken)),
        .error = body.value("error").toString(),
        .description = body.value("error_description").toString(),
        .http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        .network_error = static_cast<int>(reply->error()),
    };
}

void OAuthManager::failStoredLogin(const TokenReply &reply)
{
    if (reply.error == TOKEN_REFUSED) {
        // One refusal is final (N46): the token is of no further use, and
        // refreshing it at every start would spend a request each time.
        spdlog::error("OAuth: the server refused the stored login (HTTP {}): removing it",
                      reply.http_status);
        m_data.Set("oauth_token", "");
        emit storedLoginFailed("The stored login was refused by Path of Exile. "
                               "Please authenticate again.");
    } else {
        spdlog::error("OAuth: the stored login could not be refreshed (HTTP {}, network error "
                      "{}): keeping it",
                      reply.http_status,
                      reply.network_error);
        emit storedLoginFailed("Could not refresh the stored login: the Path of Exile server "
                               "could not be reached.");
    }
}

void OAuthManager::endLogin()
{
    m_login_in_progress = false;
    m_handler->close();
}

void OAuthManager::failLogin(const QString &reason)
{
    endLogin();
    emit loginFailed(reason);
}

void OAuthManager::receiveToken(const QVariantMap &tokens)
{
    // Qt 6.11.3 and 6.12.0 hand this signal the body of an error reply too,
    // before they report the error (6.11.1 does not). That is no token:
    // taking it for one would put an empty login in place of the stored one.
    if (tokens.value("access_token").toString().isEmpty()) {
        return;
    }

    m_token = OAuthToken::fromTokens(tokens);
    spdlog::info("OAuth: tokens recieved for {}", m_token.username);

    // Store the serialized token.
    std::string serialized_token;
    auto ec = glz::write_json(m_token, serialized_token);
    if (ec) {
        // The buffer holds the token, so log the error without it (F73).
        // OAuthToken contains only glaze-supported scalar fields, so this
        // defensive branch has no constructible runtime failure for a unit
        // test; keep its logging context-free by inspection.
        const std::string msg = glz::format_error(ec);
        spdlog::error("OAuthManager: error serializing received token: {}", msg);
    } else {
        spdlog::info("OAuth: storing token");
        m_data.Set("oauth_token", QString::fromStdString(serialized_token));
    }
    m_network_manager.setBearerToken(m_token.access_token);
}

void OAuthManager::receiveGrant()
{
    endLogin();
    auto requested = m_oauth->requestedScopeTokens();
    const auto &granted = m_oauth->grantedScopeTokens();
    const auto &diff = requested.subtract(granted);
    if (!diff.empty()) {
        spdlog::error("OAuth: was not granted these requested scopes: {}",
                      QStringList(diff.begin(), diff.end()).join(", "));
    }
    spdlog::info("OAuth: access was granted.");
    emit grantAccess(m_token);
}

bool OAuthManager::initLogin()
{
    // Every login binds its own listener on IPv4 loopback: the redirect goes
    // to CALLBACK_HOST (127.0.0.1), so a listener anywhere else could never
    // receive it. Whatever the handler was doing before (its constructor
    // listens on loopback and may fall back to ::1; an earlier login may
    // still be waiting on the browser) is closed first. The redirect uri,
    // port included, is built by grant() below.
    endLogin();
    if (!m_handler->listen(QHostAddress::LocalHost)) {
        spdlog::error("OAuth: cannot listen for the login callback on {}: not starting the login",
                      CALLBACK_HOST);
        return false;
    }
    // Every login has its own state: left alone, Qt sends the one it made
    // when the flow was built with every login, and a redirect for a login
    // the user gave up would pass for this one's.
    m_oauth->setState(makeState());
    m_login_in_progress = true;
    spdlog::info("OAuth: starting authentication.");
    m_oauth->grant();
    return true;
}
