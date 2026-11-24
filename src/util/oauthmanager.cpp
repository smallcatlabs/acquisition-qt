// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "util/oauthmanager.h"

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QOAuthHttpServerReplyHandler>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <chrono>
#include <initializer_list>
#include <utility>

#include "datastore/datastore.h"
#include "replytimeout.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

// The state machine. Each region changes only through enter().
//
// Login, what the session uses:
//   NoLogin --a stored token at start--> RefreshingStored
//   RefreshingStored --granted--> Holding
//   RefreshingStored --failed--> NoLogin, storedLoginFailed
//   Holding --the refresh timer--> Holding with a refresh on its way
//       --granted--> Holding the new token; --failed--> Holding, logged only
//   any --a code exchange granted--> Holding (a refresh on its way is dropped)
//
// Browser, the login in the browser:
//   any --initLogin--> Waiting, with a new state and verifier
//   Waiting --redirect with its state and a code--> Exchanging
//   Waiting --redirect with another state, an error or no code--> NoBrowser, loginFailed
//   Exchanging --granted--> NoBrowser, and the login is Holding
//   Exchanging --failed--> NoBrowser, loginFailed
//   any --endLogin--> NoBrowser
//
// An answer of the token endpoint is taken only when it is the reply the
// current state waits for. One that is not, the reply of a state left since,
// is dropped where it arrives (receiveAnswer).

namespace {

    using namespace std::chrono_literals;

    constexpr const char *AUTHORIZATION_URL = "https://www.pathofexile.com/oauth/authorize";
    constexpr const char *TOKEN_URL = "https://www.pathofexile.com/oauth/token";
    constexpr const char *CLIENT_ID = "acquisition";
    constexpr std::array SCOPE = {"account:leagues", "account:stashes", "account:characters"};

    // Acquisition as currently approved uses a plain HTTP callback.
    // This should be changed to HTTPS or a private URI scheme at some point.
    constexpr const char *CALLBACK_HOST = "127.0.0.1";
    constexpr const char *CALLBACK_PATH = "/auth/path-of-exile";

    // How long before the access token expires it is refreshed.
    constexpr std::chrono::seconds REFRESH_LEAD_TIME = 300s;

    // The error code with which the token endpoint refuses a token it was
    // sent (RFC 6749, 5.2; N46), and how much of a reply is read.
    constexpr const char *TOKEN_REFUSED = "invalid_grant";
    constexpr qint64 BODY_LIMIT = 64 * 1024;

    // 32 bytes from the system's generator, base64url: a state, or a PKCE
    // verifier (43 characters, RFC 7636, 4.1).
    QString randomString()
    {
        std::array<quint32, 8> words;
        QRandomGenerator::system()->fillRange(words.data(), qsizetype(words.size()));
        const QByteArray bytes(reinterpret_cast<const char *>(words.data()), sizeof(words));
        return QString::fromLatin1(
            bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    // The S256 code challenge of a PKCE verifier (RFC 7636, 4.2).
    QString challengeOf(const QString &verifier)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    }

    // An application/x-www-form-urlencoded body. Every value is percent
    // encoded, '+' included, which a form decoder would read as a space.
    QByteArray form(std::initializer_list<std::pair<const char *, QString>> fields)
    {
        QByteArray body;
        for (const auto &[name, value] : fields) {
            if (!body.isEmpty()) {
                body += '&';
            }
            body += name;
            body += '=';
            body += QUrl::toPercentEncoding(value);
        }
        return body;
    }

    // The line for the user when the server answered a login with an error.
    QString serverErrorReason(const QString &error, const QString &description)
    {
        return description.isEmpty()
                   ? QString("Authentication failed: %1").arg(error)
                   : QString("Authentication failed: %1 (%2)").arg(error, description);
    }

    // The request a state waits for an answer to, if any.
    template<class Variant>
    QNetworkReply *awaited(const Variant &state)
    {
        return std::visit(
            [](const auto &s) -> QNetworkReply * {
                if constexpr (requires { s.refresh; }) {
                    return s.refresh;
                } else if constexpr (requires { s.exchange; }) {
                    return s.exchange;
                } else {
                    return nullptr;
                }
            },
            state);
    }

    // Drops a request nothing waits for any more. Its answer, if it still
    // comes, finds no state waiting for it.
    void abandon(QNetworkReply *before, QNetworkReply *after, const char *what)
    {
        if (before && (before != after) && before->isRunning()) {
            spdlog::info("OAuth: abandoning the {} nothing waits for any more", what);
            before->abort();
        }
    }

} // namespace

OAuthManager::OAuthManager(NetworkManager &network_manager, DataStore &datastore, QObject *parent)
    : QObject(parent)
    , m_network_manager(network_manager)
    , m_data(datastore)
{
    // Qt's handler listens for the redirect, answers the browser with a page
    // and reports the redirect's query; the token requests are not its
    // business here. It listens as it is built, and is closed until a
    // browser login waits.
    m_listener = new QOAuthHttpServerReplyHandler(QHostAddress::LocalHost, 0, this);
    m_listener->close();
    m_listener->setCallbackHost(CALLBACK_HOST);
    m_listener->setCallbackPath(CALLBACK_PATH);
    // The handler answers every redirect it takes with this page, before
    // the redirect is judged, so it must hold whatever the outcome (#409):
    // the login window says whether the login succeeded.
    m_listener->setCallbackText("You can close this page and return to Acquisition.");
    connect(m_listener,
            &QAbstractOAuthReplyHandler::callbackReceived,
            this,
            &OAuthManager::receiveRedirect);

    m_refresh_timer.setSingleShot(true);
    connect(&m_refresh_timer, &QTimer::timeout, this, &OAuthManager::refreshNow);

    const QString stored = m_data.Get("oauth_token", "");
    if (!stored.isEmpty()) {
        const OAuthToken token = OAuthToken::fromJson(stored);
        spdlog::info("OAuth: refreshing token for '{}'", token.username);
        enter(RefreshingStored{post(form({{"grant_type", "refresh_token"},
                                          {"refresh_token", token.refresh_token},
                                          {"client_id", CLIENT_ID}}))});
    }
}

void OAuthManager::enter(Login next)
{
    QNetworkReply *before = awaited(m_login);
    m_login = std::move(next);
    abandon(before, awaited(m_login), "refresh");
}

void OAuthManager::enter(Browser next)
{
    QNetworkReply *before = awaited(m_browser);
    m_browser = std::move(next);
    abandon(before, awaited(m_browser), "code exchange");
    if (std::holds_alternative<NoBrowser>(m_browser)) {
        m_listener->close();
    }
}

bool OAuthManager::initLogin()
{
    // Every login binds its own listener on IPv4 loopback (#316): the
    // redirect goes to CALLBACK_HOST, so a listener anywhere else could
    // never receive it.
    enter(NoBrowser{});
    if (!m_listener->listen(QHostAddress::LocalHost)) {
        spdlog::error("OAuth: cannot listen for the login callback on {}: not starting the login",
                      CALLBACK_HOST);
        return false;
    }
    Waiting attempt{.state = randomString(),
                    .verifier = randomString(),
                    .redirect_uri = m_listener->callback()};

    QStringList scope;
    for (const char *token : SCOPE) {
        scope << token;
    }
    QUrlQuery query;
    query.addQueryItem("response_type", "code");
    query.addQueryItem("client_id", CLIENT_ID);
    query.addQueryItem("redirect_uri", attempt.redirect_uri);
    query.addQueryItem("scope", scope.join(' '));
    query.addQueryItem("state", attempt.state);
    query.addQueryItem("code_challenge", challengeOf(attempt.verifier));
    query.addQueryItem("code_challenge_method", "S256");
    QUrl url(AUTHORIZATION_URL);
    url.setQuery(query);

    enter(std::move(attempt));
    spdlog::info("OAuth: starting authentication.");
    QDesktopServices::openUrl(url);
    return true;
}

void OAuthManager::endLogin()
{
    enter(NoBrowser{});
}

void OAuthManager::receiveRedirect(const QVariantMap &query)
{
    const auto *waiting = std::get_if<Waiting>(&m_browser);
    if (!waiting) {
        // A redirect sent again while its code is exchanged (a reload of the
        // page), or one that reached a listener closing.
        spdlog::info("OAuth: ignoring a redirect no browser login waits for");
        return;
    }
    // A redirect with another state is anybody's: its error, if it has one,
    // is not shown.
    if (query.value("state").toString() != waiting->state) {
        spdlog::error("OAuth: the redirect does not carry this login's state");
        failLogin("Authentication failed: the reply from the browser did not match this login "
                  "attempt.");
        return;
    }
    const QString error = query.value("error").toString();
    if (!error.isEmpty()) {
        spdlog::error("OAuth: the redirect brought an error: {}", error);
        failLogin(serverErrorReason(error, query.value("error_description").toString()));
        return;
    }
    const QString code = query.value("code").toString();
    if (code.isEmpty()) {
        spdlog::error("OAuth: the redirect brought no code");
        failLogin("Authentication failed: no token was received from Path of Exile.");
        return;
    }
    spdlog::info("OAuth: exchanging the code");
    enter(Exchanging{post(form({{"grant_type", "authorization_code"},
                                {"code", code},
                                {"redirect_uri", waiting->redirect_uri},
                                {"client_id", CLIENT_ID},
                                {"code_verifier", waiting->verifier}}))});
}

void OAuthManager::refreshNow()
{
    const auto *held = std::get_if<Holding>(&m_login);
    if (!held || held->refresh) {
        return;
    }
    spdlog::info("OAuth: refreshing token for '{}'", held->token.username);
    enter(Holding{held->token,
                  post(form({{"grant_type", "refresh_token"},
                             {"refresh_token", held->token.refresh_token},
                             {"client_id", CLIENT_ID}}))});
}

QNetworkReply *OAuthManager::post(const QByteArray &body)
{
    QNetworkRequest request{QUrl{TOKEN_URL}};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(kPoeApiTimeout);
    QNetworkReply *reply = m_network_manager.post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply] { receiveAnswer(reply); });
    return reply;
}

void OAuthManager::receiveAnswer(QNetworkReply *reply)
{
    reply->deleteLater();
    if ((reply != awaited(m_browser)) && (reply != awaited(m_login))) {
        // Abandoned: the attempt or the login it was sent for has ended.
        spdlog::info("OAuth: ignoring an answer of the token endpoint nothing waits for");
        return;
    }
    // A reply Qt abandoned (a timeout, a lost connection) is closed, and has
    // no body to read.
    const QJsonObject body = reply->isOpen()
                                 ? QJsonDocument::fromJson(reply->read(BODY_LIMIT)).object()
                                 : QJsonObject();
    const Answer answer{
        .fields = (reply->error() == QNetworkReply::NoError) ? body.toVariantMap() : QVariantMap(),
        .error = body.value("error").toString(),
        .description = body.value("error_description").toString(),
        .http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        .network_error = reply->error(),
    };
    if (reply == awaited(m_browser)) {
        exchangeAnswered(answer);
    } else {
        refreshAnswered(answer);
    }
}

void OAuthManager::exchangeAnswered(const Answer &answer)
{
    if (answer.fields.value("access_token").toString().isEmpty()) {
        spdlog::error("OAuth: the code exchange failed (HTTP {}, network error {}): {}",
                      answer.http_status,
                      static_cast<int>(answer.network_error),
                      answer.error);
        failLogin(answer.error.isEmpty()
                      ? "Authentication failed: no token was received from Path of Exile."
                      : serverErrorReason(answer.error, answer.description));
        return;
    }
    enter(NoBrowser{});
    hold(OAuthToken::fromTokens(answer.fields));
}

void OAuthManager::refreshAnswered(const Answer &answer)
{
    if (!answer.fields.value("access_token").toString().isEmpty()) {
        hold(OAuthToken::fromTokens(answer.fields));
    } else if (std::holds_alternative<RefreshingStored>(m_login)) {
        failStoredLogin(answer);
    } else {
        // A token taken in this run stays held until it expires; the browser
        // login, if one waits, is not what failed (#405, #410).
        spdlog::error("OAuth: the refresh failed (HTTP {}, network error {}): {}",
                      answer.http_status,
                      static_cast<int>(answer.network_error),
                      answer.error);
        enter(Holding{std::get<Holding>(m_login).token});
    }
}

void OAuthManager::failStoredLogin(const Answer &answer)
{
    enter(NoLogin{});
    if (answer.error == TOKEN_REFUSED) {
        // One refusal is final (N46): the token is of no further use, and
        // refreshing it at every start would spend a request each time.
        spdlog::error("OAuth: the server refused the stored login (HTTP {}): removing it",
                      answer.http_status);
        m_data.Set("oauth_token", "");
        emit storedLoginFailed("The stored login was refused by Path of Exile. "
                               "Please authenticate again.");
    } else {
        spdlog::error("OAuth: the stored login could not be refreshed (HTTP {}, network error "
                      "{}): keeping it",
                      answer.http_status,
                      static_cast<int>(answer.network_error));
        emit storedLoginFailed("Could not refresh the stored login: the Path of Exile server "
                               "could not be reached.");
    }
}

void OAuthManager::failLogin(const QString &reason)
{
    enter(NoBrowser{});
    emit loginFailed(reason);
}

void OAuthManager::hold(const OAuthToken &token)
{
    enter(Holding{token});
    spdlog::info("OAuth: tokens received for {}", token.username);

    std::string serialized;
    if (const auto ec = glz::write_json(token, serialized)) {
        // The buffer holds the token, so log the error without it (F73).
        // OAuthToken contains only glaze-supported scalar fields, so this
        // defensive branch has no constructible runtime failure for a unit
        // test; keep its logging context-free by inspection.
        const std::string msg = glz::format_error(ec);
        spdlog::error("OAuthManager: error serializing received token: {}", msg);
    } else {
        m_data.Set("oauth_token", QString::fromStdString(serialized));
    }
    m_network_manager.setBearerToken(token.access_token);

    // A reply without a scope grants the scope requested (RFC 6749, 5.1).
    const QStringList granted = token.scope.split(' ', Qt::SkipEmptyParts);
    QStringList missing;
    for (const char *scope : SCOPE) {
        if (!granted.isEmpty() && !granted.contains(QLatin1StringView(scope))) {
            missing << scope;
        }
    }
    if (!missing.isEmpty()) {
        spdlog::error("OAuth: was not granted these requested scopes: {}", missing.join(", "));
    }

    const std::chrono::seconds lifetime{token.expires_in};
    if (lifetime > 0s) {
        m_refresh_timer.start(std::max(lifetime - REFRESH_LEAD_TIME, std::chrono::seconds{1}));
    } else {
        m_refresh_timer.stop();
    }
    spdlog::info("OAuth: access was granted.");
    emit grantAccess(token);
}
