// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QByteArray>
#include <QByteArrayList>
#include <QDateTime>
#include <QObject>
#include <QString>

#include <optional>

#include "util/spdlog_qt.h"

class QNetworkReply;

//--------------------------------------------------------------------------
// Introduction to GGG's API Rate Limits
//--------------------------------------------------------------------------
//
// As of August 2023, GGG has implemented "new" rate limit policies that are
// incompatible with how acquisition used to throttle requests. There was no
// obvious way to fix the network request code used by the ItemsManagerWorker.
// Instead, what I've done instead is create a wrapper that accepts network
// requests from the ItemsManagerWorker and hidden the implementation details.
//
// See https://www.pathofexile.com/developer/docs for more details on the rate
// limit information that is included in the HTTP headers of the network reply
// to every API request.
//
// Those rate limit policies can change at any time, from one network
// call to the next. For example, GGG might decide to temporarily tighten rate
// limitations around a league launch.
//
// This means that any solution that hard-codes things like delays will
// eventually break.
//
// What this means for us is that we have to check the reply to every
// network request for the details of rate-limitation policy that were applied.
//
// Each policy can have mulitple rules that apply to it. Each rule
// can have multiple elments. Each element defines the specific number of
// API hits that are allowed, the period within which those hits are measured,
// and the timeout restriction if that item is violated.
//
// This header holds only the `RateLimit` namespace: the `Status` enum, the
// Retry-After grammar and product cap, and the header-parsing helpers that
// read the `X-Rate-Limit-*` fields off a reply. The machinery that acts on
// them — the hub, the gate, the per-endpoint pumps — lives in `ratelimiter.h`
// / `ratelimitmanager.h` and is specified in
// `docs/design/network-redesign.md`. The policy *limits* are never
// hard-coded — every rule, period, and hit count is read from the reply
// headers at runtime — but the redesign does hard-code a small set of
// documented safety constants on top of them (the gate's 250 ms send spacing,
// the 60 s retry padding and setup cooldown, the bucket sizes, and the
// buffers), so the derived delays are header-driven limits plus fixed margins.
//
// WARNING: throttling can still break where there are other sources of
// rate-limited API requests for the same account — e.g. two tools running on
// one machine.

namespace RateLimit {
    Q_NAMESPACE

    enum class Status { OK, BORDERLINE, VIOLATION, INVALID };
    Q_ENUM_NS(Status)

    // Product-policy cap on Retry-After (network-redesign spec, D3): the
    // grammar accepts any nonnegative integer, but obeying a never-observed
    // multi-hour wait would be continuation through the unexpected — longer
    // waits are declined. Longest observed restriction is 600 s (N23), plus
    // headroom.
    constexpr int RETRY_AFTER_CAP_SECS = 900;

    struct Event
    {
        unsigned long request_id;
        QString request_url;
        QDateTime request_time;
        QDateTime received_time;
        QDateTime reply_time;
        int reply_status;
    };

    // Retry-After grammar and product policy in one place (D3): accepted
    // iff present, an integer, nonnegative, and at or below the cap. 0 is
    // valid (the retry pad and GetNextSafeSend dominate the deadline
    // formula). Anything else is nullopt: the 429 is terminal.
    std::optional<int> ParseRetryAfter(QNetworkReply *const reply);

    QByteArray ParseHeader(QNetworkReply *const reply, const QByteArray &name);
    QByteArray ParseRateLimitPolicy(QNetworkReply *const reply);
    QDateTime ParseDate(QNetworkReply *const reply);
    int ParseStatus(QNetworkReply *const reply);
} // namespace RateLimit

// Create a formatting so we can print RateLimit::Status with spdlog.
template<>
struct fmt::formatter<RateLimit::Status, char> : QtEnumFormatter<RateLimit::Status>
{};
