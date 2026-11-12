// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#include "util/networkmanager.h"

#include <QDir>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QStandardPaths>

#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitpolicy.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "version_defines.h"

constexpr const char *USER_AGENT = APP_NAME "/" APP_VERSION_STRING " (contact: " APP_PUBLISHER_EMAIL
                                            ")";

constexpr const char *POE_COOKIE_NAME = "POESESSID";
constexpr const char *POE_COOKIE_DOMAIN = ".pathofexile.com";
constexpr const char *POE_COOKIE_PATH = "/";

// Size of the network disk cache.
constexpr const int CACHE_SIZE_MEGABYTES = 100;

constexpr const char *POE_API_HOST = "api.pathofexile.com";
constexpr const char *POE_CDN_HOST = "web.poecdn.com";

// The OAuth token endpoint (TOKEN_URL in oauthmanager.cpp). Its requests are
// issued internally by QOAuth2AuthorizationCodeFlow, so this is the only
// place the client sees them.
constexpr const char *POE_WWW_HOST = "www.pathofexile.com";
constexpr const char *POE_OAUTH_TOKEN_PATH = "/oauth/token";

// This list is current as of Qt 6.10.0:
// https://doc.qt.io/qt-6/qnetworkrequest.html#Attribute-enum
//
// clang-format off
constexpr std::array<std::pair<QNetworkRequest::Attribute, const char *>, 28> KNOWN_ATTRIBUTES{{
    {QNetworkRequest::HttpStatusCodeAttribute, "HttpStatusCodeAttribute"}, //	0	Replies only, type: QMetaType::Int(no default) Indicates the HTTP status code received from the HTTP server(like 200, 304, 404, 401, etc.).If the connection was not HTTP - based, this attribute will not be present.
    {QNetworkRequest::HttpReasonPhraseAttribute, "HttpReasonPhraseAttribute"}, //	1	Replies only, type : QMetaType::QByteArray(no default) Indicates the HTTP reason phrase as received from the HTTP server(like "Ok", "Found", "Not Found", "Access Denied", etc.) This is the human - readable representation of the status code(see above).If the connection was not HTTP - based, this attribute will not be present.
    {QNetworkRequest::RedirectionTargetAttribute, "RedirectionTargetAttribute"}, //	2	Replies only, type : QMetaType::QUrl(no default) If present, it indicates that the server is redirecting the request to a different URL.The Network Access API does follow redirections by default, unless QNetworkRequest::ManualRedirectPolicy is used.Additionally, if QNetworkRequest::UserVerifiedRedirectPolicy is used, then this attribute will be set if the redirect was not followed.The returned URL might be relative.Use QUrl::resolved() to create an absolute URL out of it.
    {QNetworkRequest::ConnectionEncryptedAttribute, "ConnectionEncryptedAttribute"}, //	3	Replies only, type : QMetaType::Bool(default: false) Indicates whether the data was obtained through an encrypted(secure) connection.
    {QNetworkRequest::CacheLoadControlAttribute, "CacheLoadControlAttribute"}, //	4	Requests only, type : QMetaType::Int(default: QNetworkRequest::PreferNetwork) Controls how the cache should be accessed.The possible values are those of QNetworkRequest::CacheLoadControl.Note that the default QNetworkAccessManager implementation does not support caching.However, this attribute may be used by certain backends to modify their requests(for example, for caching proxies).
    {QNetworkRequest::CacheSaveControlAttribute, "CacheSaveControlAttribute"}, //	5	Requests only, type : QMetaType::Bool(default: true) Controls if the data obtained should be saved to cache for future uses.If the value is false, the data obtained will not be automatically cached.If true, data may be cached, provided it is cacheable(what is cacheable depends on the protocol being used).
    {QNetworkRequest::SourceIsFromCacheAttribute, "SourceIsFromCacheAttribute"}, //	6	Replies only, type : QMetaType::Bool(default: false) Indicates whether the data was obtained from cache or not.
    {QNetworkRequest::DoNotBufferUploadDataAttribute, "DoNotBufferUploadDataAttribute"}, //	7	Requests only, type : QMetaType::Bool(default: false) Indicates whether the QNetworkAccessManager code is allowed to buffer the upload data, e.g.when doing a HTTP POST.When using this flag with sequential upload data, the ContentLengthHeader header must be set.
    {QNetworkRequest::HttpPipeliningAllowedAttribute, "HttpPipeliningAllowedAttribute"}, //	8	Requests only, type : QMetaType::Bool(default: false) Indicates whether the QNetworkAccessManager code is allowed to use HTTP pipelining with this request.
    {QNetworkRequest::HttpPipeliningWasUsedAttribute, "HttpPipeliningWasUsedAttribute"}, //	9	Replies only, type : QMetaType::Bool Indicates whether the HTTP pipelining was used for receiving this reply.
    {QNetworkRequest::CustomVerbAttribute, "CustomVerbAttribute"}, //	10	Requests only, type : QMetaType::QByteArray Holds the value for the custom HTTP verb to send(destined for usage of other verbs than GET, POST, PUT and DELETE).This verb is set when calling QNetworkAccessManager::sendCustomRequest().
    {QNetworkRequest::CookieLoadControlAttribute, "CookieLoadControlAttribute"}, //	11	Requests only, type : QMetaType::Int(default: QNetworkRequest::Automatic) Indicates whether to send 'Cookie' headers in the request.This attribute is set to false by Qt WebKit when creating a cross - origin XMLHttpRequest where withCredentials has not been set explicitly to true by the Javascript that created the request.See here for more information. (This value was introduced in 4.7.)
    {QNetworkRequest::CookieSaveControlAttribute, "CookieSaveControlAttribute"}, //	13	Requests only, type : QMetaType::Int(default: QNetworkRequest::Automatic) Indicates whether to save 'Cookie' headers received from the server in reply to the request.This attribute is set to false by Qt WebKit when creating a cross - origin XMLHttpRequest where withCredentials has not been set explicitly to true by the Javascript that created the request.See here for more information. (This value was introduced in 4.7.)
    {QNetworkRequest::AuthenticationReuseAttribute, "AuthenticationReuseAttribute"}, //	12	Requests only, type : QMetaType::Int(default: QNetworkRequest::Automatic) Indicates whether to use cached authorization credentials in the request, if available.If this is set to QNetworkRequest::Manual and the authentication mechanism is 'Basic' or 'Digest', Qt will not send an 'Authorization' HTTP header with any cached credentials it may have for the request's URL. This attribute is set to QNetworkRequest::Manual by Qt WebKit when creating a cross-origin XMLHttpRequest where withCredentials has not been set explicitly to true by the Javascript that created the request. See here for more information. (This value was introduced in 4.7.)
    {QNetworkRequest::BackgroundRequestAttribute, "BackgroundRequestAttribute"}, //	17	Type : QMetaType::Bool(default: false) Indicates that this is a background transfer, rather than a user initiated transfer.Depending on the platform, background transfers may be subject to different policies.
    {QNetworkRequest::Http2AllowedAttribute, "Http2AllowedAttribute"}, //	19	Requests only, type : QMetaType::Bool(default: true) Indicates whether the QNetworkAccessManager code is allowed to use HTTP / 2 with this request.This applies to SSL requests or 'cleartext' HTTP / 2 if Http2CleartextAllowedAttribute is set.
    {QNetworkRequest::Http2WasUsedAttribute, "Http2WasUsedAttribute"}, //	20	Replies only, type : QMetaType::Bool(default: false) Indicates whether HTTP / 2 was used for receiving this reply. (This value was introduced in 5.9.)
    {QNetworkRequest::EmitAllUploadProgressSignalsAttribute, "EmitAllUploadProgressSignalsAttribute"}, //	18	Requests only, type : QMetaType::Bool(default: false) Indicates whether all upload signals should be emitted.By default, the uploadProgress signal is emitted only in 100 millisecond intervals. (This value was introduced in 5.5.)
    {QNetworkRequest::OriginalContentLengthAttribute, "OriginalContentLengthAttribute"}, //	21	Replies only, type QMetaType::Int Holds the original content - length attribute before being invalidated and removed from the header when the data is compressed and the request was marked to be decompressed automatically. (This value was introduced in 5.9.)
    {QNetworkRequest::RedirectPolicyAttribute, "RedirectPolicyAttribute"}, //	22	Requests only, type : QMetaType::Int, should be one of the QNetworkRequest::RedirectPolicy values(default: NoLessSafeRedirectPolicy). (This value was introduced in 5.9.)
    {QNetworkRequest::Http2DirectAttribute, "Http2DirectAttribute"}, //	23	Requests only, type : QMetaType::Bool(default: false) If set, this attribute will force QNetworkAccessManager to use HTTP / 2 protocol without initial HTTP / 2 protocol negotiation.Use of this attribute implies prior knowledge that a particular server supports HTTP / 2. The attribute works with SSL or with 'cleartext' HTTP / 2 if Http2CleartextAllowedAttribute is set.If a server turns out to not support HTTP / 2, when HTTP / 2 direct was specified, QNetworkAccessManager gives up, without attempting to fall back to HTTP / 1.1.If both Http2AllowedAttribute and Http2DirectAttribute are set, Http2DirectAttribute takes priority. (This value was introduced in 5.11.)
    {QNetworkRequest::AutoDeleteReplyOnFinishAttribute, "AutoDeleteReplyOnFinishAttribute"}, //	25	Requests only, type : QMetaType::Bool(default: false) If set, this attribute will make QNetworkAccessManager delete the QNetworkReply after having emitted "finished". (This value was introduced in 5.14.)
    {QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, "ConnectionCacheExpiryTimeoutSecondsAttribute"}, //	26	Requests only, type : QMetaType::Int To set when the TCP connections to a server(HTTP1 and HTTP2) should be closed after the last pending request had been processed. (This value was introduced in 6.3.)
    {QNetworkRequest::Http2CleartextAllowedAttribute, "Http2CleartextAllowedAttribute"}, //	27	Requests only, type : QMetaType::Bool(default: false) If set, this attribute will tell QNetworkAccessManager to attempt an upgrade to HTTP / 2 over cleartext(also known as h2c).Until Qt 7 the default value for this attribute can be overridden to true by setting the QT_NETWORK_H2C_ALLOWED environment variable.This attribute is ignored if the Http2AllowedAttribute is not set. (This value was introduced in 6.3.)
    {QNetworkRequest::UseCredentialsAttribute, "UseCredentialsAttribute"}, //	28	Requests only, type : QMetaType::Bool(default: false) Indicates if the underlying XMLHttpRequest cross - site Access - Control requests should be made using credentials.Has no effect on same - origin requests.This only affects the WebAssembly platform. (This value was introduced in 6.5.)
    {QNetworkRequest::FullLocalServerNameAttribute, "FullLocalServerNameAttribute"}, // 29	Requests only, type: QMetaType::String Holds the full local server name to be used for the underlying QLocalSocket. This attribute is used by the QNetworkAccessManager to connect to a specific local server, when QLocalSocket's behavior for a simple name isn't enough. The URL in the QNetworkRequest must still use unix+http: or local+http: scheme. And the hostname in the URL will be used for the Host header in the HTTP request. (This value was introduced in 6.8.)
    {QNetworkRequest::User, "User"}, //	1000	Special type.Additional information can be passed in QVariants with types ranging from User to UserMax.The default implementation of Network Access will ignore any request attributes in this range and it will not produce any attributes in this range in replies.The range is reserved for extensions of QNetworkAccessManager.
    // Custom user attributes can be in this range.
    {QNetworkRequest::UserMax, "UserMax"} //	32767
}};
// clang-format on

NetworkManager::NetworkManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
    const auto data_dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const auto cache_dir = data_dir + QLatin1StringView("/network_cache/");
    m_diskCache = new QNetworkDiskCache(this);
    m_diskCache->setCacheDirectory(cache_dir);
    m_diskCache->setMaximumCacheSize(CACHE_SIZE_MEGABYTES * 1024 * 1024);
    setCache(m_diskCache);
}

void NetworkManager::clearPoeSessionId()
{
    spdlog::debug("NetworkManager: clearing POESESSID");
    QNetworkCookie cookie{POE_COOKIE_NAME, QByteArray{}};
    cookie.setPath(POE_COOKIE_PATH);
    cookie.setDomain(POE_COOKIE_DOMAIN);
    if (!cookieJar()->deleteCookie(cookie)) {
        spdlog::debug("NetworkManager: no POESESSID cookie was present");
    }
}

void NetworkManager::setPoeSessionId(const QString &poesessid)
{
    QString masked = poesessid;
    masked.fill('*');
    spdlog::debug("NetworkManager: setting POESESSID: {} (masked for security)", masked);

    QNetworkCookie cookie{POE_COOKIE_NAME, poesessid.toUtf8()};
    cookie.setPath(POE_COOKIE_PATH);
    cookie.setDomain(POE_COOKIE_DOMAIN);
    cookieJar()->insertCookie(cookie);
}

void NetworkManager::setBearerToken(const QString &token)
{
    m_bearerToken = token.isEmpty() ? QByteArrayLiteral("")
                                    : (QByteArrayLiteral("Bearer ") + token.toUtf8());
}

QNetworkReply *NetworkManager::createRequest(QNetworkAccessManager::Operation op,
                                             const QNetworkRequest &originalRequest,
                                             QIODevice *outgoingData)
{
    // Always set the user agent.
    QNetworkRequest request(originalRequest);
    request.setRawHeader("User-Agent", USER_AGENT);

    const auto host = request.url().host();

    if (host == POE_API_HOST) {
        // Add a bearer token for api calls.
        if (m_bearerToken.isEmpty()) {
            spdlog::error("API call may fail because the bearer token is empty: {}",
                          request.url().toString());
        } else {
            QString masked{QString::fromUtf8(m_bearerToken)};
            masked.fill('*');
            spdlog::trace("NetworkManager: setting bearer token: {} (masked for security)",
                          masked);
            request.setRawHeader("Authorization", m_bearerToken);
        }
    } else if (host == POE_CDN_HOST) {
        // Prefer the cache for cdn content.
        request
            .setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    }

    spdlog::trace("Network: requesting {}", request.url().toDisplayString());

    // Let the base class handle the rest.
    QNetworkReply *reply = QNetworkAccessManager::createRequest(op, request, outgoingData);

    // Watch token-endpoint replies for rate-limit trouble (N33).
    if ((host == POE_WWW_HOST) && (request.url().path() == POE_OAUTH_TOKEN_PATH)) {
        connect(reply, &QNetworkReply::finished, reply, [reply]() {
            logOAuthTokenRateLimits(reply);
        });
    }
    return reply;
}

void NetworkManager::logOAuthTokenRateLimits(QNetworkReply *reply)
{
    const int status = RateLimit::ParseStatus(reply);
    if (status == 429) {
        const auto retry_after = RateLimit::ParseRetryAfter(reply);
        const QString wait = retry_after ? QString("%1s").arg(*retry_after)
                                         : QString("absent or invalid");
        spdlog::error("OAuth token endpoint returned 429 (policy '{}'): Retry-After {}",
                      RateLimit::ParseRateLimitPolicy(reply),
                      wait);
        return;
    }
    const auto policy = RateLimitPolicy::Parse(reply);
    if (!policy) {
        // No usable rate-limit headers — e.g. a Cloudflare-mitigated reply
        // (N28) or a non-HTTP transport failure.
        spdlog::debug("OAuth token endpoint reply (HTTP status {}) had no parseable rate-limit "
                      "policy: {}",
                      status,
                      policy.error());
        return;
    }
    if (policy->status() >= RateLimit::Status::BORDERLINE) {
        spdlog::warn("OAuth token endpoint rate limit '{}' is {}:\n{}",
                     policy->name(),
                     policy->status(),
                     policy->GetPolicyReport());
    }
}

void NetworkManager::logRequest(const QNetworkRequest &request)
{
    auto attributes = [=](QNetworkRequest::Attribute code) { return request.attribute(code); };
    spdlog::debug("Network: request url = {}", request.url().toDisplayString());
    logHeaders("request", request.headers());
    logAttributes("request", attributes);
}

void NetworkManager::logReply(const QNetworkReply *reply)
{
    auto attributes = [=](QNetworkRequest::Attribute code) { return reply->attribute(code); };
    logHeaders("reply", reply->headers());
    logAttributes("reply", attributes);
}

void NetworkManager::logAttributes(const QString &name, AttributeGetter getAttribute)
{
    QStringList lines;
    for (const auto &[code, attribute] : KNOWN_ATTRIBUTES) {
        const QVariant value = getAttribute(code);
        if (value.isValid()) {
            lines.append(QString("%1 attribute %2 = %3").arg(name, attribute, value.toString()));
        }
    }
    if (lines.isEmpty()) {
        spdlog::debug("Network: {} has 0 attributes.", name);
    } else {
        spdlog::debug("Network: {} has {} attributes:\n{}", name, lines.size(), lines.join("\n"));
    }
}

void NetworkManager::logHeaders(const QString &name, const QHttpHeaders &headers)
{
    QStringList lines;
    const auto headerPairs = headers.toListOfPairs();
    for (const auto &[header, value] : headerPairs) {
        const bool is_authorization = (0 == header.compare("Authorization", Qt::CaseInsensitive));
        QString v = value;
        if (is_authorization) {
            // Mask the OAuth bearer token so it's not written to the log.
            v.fill('*');
            v += " (masked for security)";
        }
        lines.append(QString("%1 %2 = '%3'").arg(name, header, v));
    }
    if (lines.isEmpty()) {
        spdlog::debug("Network: {} has 0 headers.", name);
    } else {
        spdlog::debug("Network: {} has {} headers:\n{}", name, lines.size(), lines.join("\n"));
    }
}
