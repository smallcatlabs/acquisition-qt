// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "ratelimit/ratelimit.h"

#include <QNetworkReply>

#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

using namespace RateLimit;

std::optional<int> RateLimit::ParseRetryAfter(QNetworkReply *const reply)
{
    if (!reply->hasRawHeader("Retry-After")) {
        return std::nullopt;
    }
    bool ok = false;
    const int seconds = reply->rawHeader("Retry-After").toInt(&ok);
    if (!ok || seconds < 0 || seconds > RETRY_AFTER_CAP_SECS) {
        return std::nullopt;
    }
    return seconds;
}

// Get a header field from an HTTP reply.
QByteArray RateLimit::ParseHeader(QNetworkReply *const reply, const QByteArray &name)
{
    if (!reply->hasRawHeader(name)) {
        spdlog::error("RateLimit: the network reply is missing a header: {}", name);
    }
    return reply->rawHeader(name);
}

// Get a header field and split into a list.
QByteArrayList RateLimit::ParseHeaderList(QNetworkReply *const reply,
                                          const QByteArray &name,
                                          const char delim)
{
    const QByteArray value = ParseHeader(reply, name);
    const QByteArrayList items = value.split(delim);
    if (items.isEmpty()) {
        spdlog::error("GetHeaderList(): {} is empty", name);
    }
    return items;
}

// Return the name of the policy from a network reply.
QByteArray RateLimit::ParseRateLimitPolicy(QNetworkReply *const reply)
{
    return ParseHeader(reply, "X-Rate-Limit-Policy");
}

// Return the name(s) of the rule(s) from a network reply.
QByteArrayList RateLimit::ParseRateLimitRules(QNetworkReply *const reply)
{
    return ParseHeaderList(reply, "X-Rate-Limit-Rules", ',');
}

// Return a list of one or more items that define a rule's limits.
QByteArrayList RateLimit::ParseRateLimit(QNetworkReply *const reply, const QByteArray &rule)
{
    return ParseHeaderList(reply, "X-Rate-Limit-" + rule, ',');
}

// Return a list of one or more items that define a rule's current state.
QByteArrayList RateLimit::ParseRateLimitState(QNetworkReply *const reply, const QByteArray &rule)
{
    return ParseHeaderList(reply, "X-Rate-Limit-" + rule + "-State", ',');
}

// Return the date from the HTTP reply headers. Date is optional: a reply
// without one (a transport failure has no headers at all) yields an invalid
// date without logging; only a Date that is present but unparseable is an
// error.
QDateTime RateLimit::ParseDate(QNetworkReply *const reply)
{
    if (!reply->hasRawHeader("Date")) {
        return QDateTime();
    }
    const QString timestamp = QString(Util::FixTimezone(ParseHeader(reply, "Date")));
    const QDateTime date = QDateTime::fromString(timestamp, Qt::RFC2822Date).toLocalTime();
    if (!date.isValid()) {
        spdlog::error("invalid date parsed from {}", timestamp);
    }
    return date;
}

// Return the HTTP status from the reply headers.
int RateLimit::ParseStatus(QNetworkReply *const reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}
