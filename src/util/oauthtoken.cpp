// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "util/oauthtoken.h"

#include "util/json_readers.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

// Hard-code the token refresh lifetime for a public client:
// https://www.pathofexile.com/developer/docs/authorization#clients-public
constexpr long int REFRESH_LIFETIME_DAYS = 7;

OAuthToken OAuthToken::fromJson(const QString &json)
{
    const auto result = json::readOAuthToken(json.toUtf8());
    if (!result) {
        return {};
    }
    return *result;
}

OAuthToken OAuthToken::fromTokens(const QVariantMap &tokens)
{
    // clang-format off
    OAuthToken token{
        .access_token  = tokens["access_token"].toString(),
        .expires_in    = tokens["expires_in"].toLongLong(),
        .refresh_token = tokens["refresh_token"].toString(),
        .scope         = tokens["scope"].toString(),
        .username      = tokens["username"].toString(),
        .sub           = tokens["sub"].toString(),
        .token_type    = tokens["token_type"].toString(),
    };
    // clang-format on

    token.setBirthday(QDateTime::currentDateTime());
    return token;
}

void OAuthToken::setBirthday(const QDateTime &date)
{
    // clang-format off
    this->birthday           = date.toLocalTime();
    this->access_expiration  = this->birthday->addSecs(this->expires_in);
    this->refresh_expiration = this->birthday->addDays(REFRESH_LIFETIME_DAYS);
    // clang-format on
}
