// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "ratelimit/ratelimitpolicy.h"

#include <algorithm>

#include <QByteArray>
#include <QDateTime>
#include <QNetworkReply>

#include "ratelimit/ratelimit.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

// GGG has stated that when they are keeping track of request times,
// they have a timing resolution, which they called a "bucket".
//
// This explained some otherwise mysterious rate violations that I
// was seeing very intermittently. Unless there's a away to find out
// where those timing buckets begin and end precisely, all we can do
// is use the bucket size as a minimum delay.
//
// GGG has also stated that this bucket resolution may be different
// for different policies, but the one I had been asking them about
// was 5.0 seconds. They also noted that this number is currently
// not documented or exposed to api users in any way.
//
// As of June 2025, GGG has confirmed that all endpoints used by
// acquisition have a 5 second timing bucket for the "fast" rate
// limit, and a 1 minute bucket for the "slow" rate limit.

constexpr const int INITIAL_TIMING_BUCKET_SECS = 5;
constexpr const int SUSTAINED_TIMING_BUCKET_SECS = 60;

// There's nothing in the rate limit policy that says there's only
// a fast and slow rate limit, but that's what email from GGG has
// implied, so this is used as a heuristic for determining which
// is which.

constexpr const int INITIAL_VS_SUSTAINED_PERIOD_CUTOFF = 75;

// Since we don't know how the server buckets are aligned or what
// the error is, let's add a buffer.

constexpr const int TIMING_BUCKET_BUFFER_SECS = 1;

//=========================================================================================
// RateLimitItem
//=========================================================================================

RateLimitItem::RateLimitItem(const RateLimitData &limit, const RateLimitData &state)
    : m_limit(limit)
    , m_state(state)
{
    // Determine the status of this item. The parse guarantees the periods
    // match, so status is purely a hits comparison.
    if (m_state.hits() > m_limit.hits()) {
        m_status = RateLimit::Status::VIOLATION;
    } else if (m_state.hits() == m_limit.hits()) {
        m_status = RateLimit::Status::BORDERLINE;
    } else {
        m_status = RateLimit::Status::OK;
    }
}

bool RateLimitItem::Check(const RateLimitItem &other) const
{
    if (m_limit.hits() != other.m_limit.hits()) {
        spdlog::warn("Rate Limit Policy: maximum hits hits changed form {} to {}",
                     m_limit.hits(),
                     other.m_limit.hits());
        return false;
    }
    if (m_limit.period() != other.m_limit.period()) {
        spdlog::warn("Rate Limit Policy: period changed from {} to {}",
                     m_limit.period(),
                     other.m_limit.period());
        return false;
    }
    if (m_limit.restriction() != other.m_limit.restriction()) {
        spdlog::warn("Rate Limit Policy: restriction changed from {} to {}",
                     m_limit.restriction(),
                     other.m_limit.restriction());
        return false;
    }
    return true;
}

//=========================================================================================
// RateLimitRule
//=========================================================================================

RateLimitRule::RateLimitRule(const QString &name, std::vector<RateLimitItem> items)
    : m_name(name)
    , m_items(std::move(items))
{}

bool RateLimitRule::Check(const RateLimitRule &other) const
{
    // Check the rule name
    if (m_name != other.m_name) {
        spdlog::warn("Rate Limit Policy: rule name changed from {} to {}", m_name, other.m_name);
        return false;
    }

    // Check the number of items in this rule
    if (m_items.size() != other.items().size()) {
        // The number of items changed
        spdlog::warn("Rate Limit Policy: rule {} went from {} items to {} items",
                     m_name,
                     m_items.size(),
                     other.items().size());
        return false;
    }

    // Check each item
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (!m_items[i].Check(other.items()[i])) {
            return false;
        }
    }
    return true;
}

//=========================================================================================
// RateLimitPolicy
//=========================================================================================

namespace {

    // One strictly-validated "hits:period:restriction" triplet. `is_limit`
    // selects the in-range rules that differ between limit and state
    // triplets (a zero-hit limit is meaningless as a lookback and was a
    // live divide-by-zero risk; state hits legitimately start at zero).
    std::expected<RateLimitData, QString> ParseTriplet(const QByteArray &fragment, bool is_limit)
    {
        const QByteArrayList parts = fragment.split(':');
        if (parts.size() != 3) {
            return std::unexpected(
                QString("triplet '%1' does not have exactly three fields").arg(fragment));
        }
        int values[3];
        for (int i = 0; i < 3; ++i) {
            bool ok = false;
            values[i] = parts[i].toInt(&ok);
            if (!ok) {
                return std::unexpected(
                    QString("triplet '%1' field %2 is not an integer").arg(fragment).arg(i + 1));
            }
        }
        const auto [hits, period, restriction] = std::tie(values[0], values[1], values[2]);
        if (is_limit ? (hits <= 0) : (hits < 0)) {
            return std::unexpected(QString("triplet '%1' hits out of range").arg(fragment));
        }
        if (period <= 0) {
            return std::unexpected(QString("triplet '%1' period out of range").arg(fragment));
        }
        if (restriction < 0) {
            return std::unexpected(QString("triplet '%1' restriction out of range").arg(fragment));
        }
        return RateLimitData(hits, period, restriction);
    }

} // namespace

std::expected<RateLimitPolicy, QString> RateLimitPolicy::Parse(QNetworkReply *const reply)
{
    if (!reply->hasRawHeader("X-Rate-Limit-Policy")) {
        return std::unexpected(QString("missing X-Rate-Limit-Policy header"));
    }
    const QString policy_name = QString::fromUtf8(reply->rawHeader("X-Rate-Limit-Policy"));
    if (policy_name.isEmpty()) {
        return std::unexpected(QString("empty policy name"));
    }

    if (!reply->hasRawHeader("X-Rate-Limit-Rules")) {
        return std::unexpected(QString("missing X-Rate-Limit-Rules header"));
    }
    const QByteArrayList rule_names = reply->rawHeader("X-Rate-Limit-Rules").split(',');
    // Qt's split turns an empty value into a one-element [""] list — the
    // shape behind the out-of-bounds read this parse replaces — so the
    // empty-name check below also rejects an empty rules list.
    for (const auto &rule_name : rule_names) {
        if (rule_name.isEmpty()) {
            return std::unexpected(QString("empty rule name in rules list"));
        }
    }

    std::vector<RateLimitRule> rules;
    rules.reserve(rule_names.size());
    for (const auto &rule_name : rule_names) {
        const QByteArray limit_header = "X-Rate-Limit-" + rule_name;
        const QByteArray state_header = limit_header + "-State";
        if (!reply->hasRawHeader(limit_header)) {
            return std::unexpected(QString("missing %1 header").arg(QString(limit_header)));
        }
        if (!reply->hasRawHeader(state_header)) {
            return std::unexpected(QString("missing %1 header").arg(QString(state_header)));
        }
        const QByteArrayList limit_fragments = reply->rawHeader(limit_header).split(',');
        const QByteArrayList state_fragments = reply->rawHeader(state_header).split(',');
        if (limit_fragments.size() != state_fragments.size()) {
            return std::unexpected(QString("rule '%1' has %2 limits but %3 states")
                                       .arg(QString(rule_name))
                                       .arg(limit_fragments.size())
                                       .arg(state_fragments.size()));
        }

        std::vector<RateLimitItem> items;
        items.reserve(limit_fragments.size());
        for (int j = 0; j < limit_fragments.size(); ++j) {
            const auto limit = ParseTriplet(limit_fragments[j], true);
            if (!limit) {
                return std::unexpected(
                    QString("rule '%1' limit: %2").arg(QString(rule_name), limit.error()));
            }
            const auto state = ParseTriplet(state_fragments[j], false);
            if (!state) {
                return std::unexpected(
                    QString("rule '%1' state: %2").arg(QString(rule_name), state.error()));
            }
            if (state->period() != limit->period()) {
                return std::unexpected(QString("rule '%1' state period %2 does not match "
                                               "limit period %3")
                                           .arg(QString(rule_name))
                                           .arg(state->period())
                                           .arg(limit->period()));
            }
            items.emplace_back(*limit, *state);
        }
        if (items.empty()) {
            return std::unexpected(QString("rule '%1' has no items").arg(QString(rule_name)));
        }
        rules.emplace_back(QString::fromUtf8(rule_name), std::move(items));
    }

    return RateLimitPolicy(policy_name, std::move(rules));
}

RateLimitPolicy::RateLimitPolicy(const QString &name, std::vector<RateLimitRule> rules)
    : m_name(name)
    , m_rules(std::move(rules))
    , m_status(RateLimit::Status::OK)
    , m_maximum_hits(0)
{
    for (const auto &rule : m_rules) {
        for (const auto &item : rule.items()) {
            // Log any violations.
            if (item.status() >= RateLimit::Status::VIOLATION) {
                spdlog::error("Rate limit policy '{}/{}[{}s]' is {})",
                              m_name,
                              rule.name(),
                              item.limit().period(),
                              Util::toString(item.status()));
            }
            // Update metrics for this rule.
            if (m_maximum_hits < item.limit().hits()) {
                m_maximum_hits = item.limit().hits();
            }
            if (m_status < item.status()) {
                m_status = item.status();
            }
        }
    }
}

bool RateLimitPolicy::HasSameShape(const RateLimitPolicy &other) const
{
    if (m_name != other.m_name || m_rules.size() != other.rules().size()) {
        return false;
    }
    for (size_t rule_index = 0; rule_index < m_rules.size(); ++rule_index) {
        const auto &rule = m_rules[rule_index];
        const auto &other_rule = other.rules()[rule_index];
        if (rule.name() != other_rule.name() || rule.items().size() != other_rule.items().size()) {
            return false;
        }
        for (size_t item_index = 0; item_index < rule.items().size(); ++item_index) {
            if (rule.items()[item_index].limit().period()
                != other_rule.items()[item_index].limit().period()) {
                return false;
            }
        }
    }
    return true;
}

bool RateLimitPolicy::Check(const RateLimitPolicy &other) const
{
    // Check the policy name.
    if (m_name != other.m_name) {
        spdlog::warn("The rate limit policy name change from {} to {}", m_name, other.m_name);
        return false;
    }

    // Check the number of rules.
    if (m_rules.size() != other.rules().size()) {
        // The number of rules changed
        spdlog::warn("The rate limit policy {} had {} rules, but now has {}",
                     m_name,
                     m_rules.size(),
                     other.rules().size());
        return false;
    }

    // The number of rules is the same, so check each one
    for (size_t i = 0; i < m_rules.size(); ++i) {
        if (!m_rules[i].Check(other.rules()[i])) {
            return false;
        }
    }
    return true;
}

QString RateLimitPolicy::Timestamp(const QDateTime &t)
{
    return t.toString("yyyy-MMM-dd HH:mm:ss.zzz");
}

QString RateLimitPolicy::GetPolicyReport() const
{
    QStringList lines;
    lines.append(QString("<POLICY_STATE policy='%1'>").arg(m_name));
    lines.append(QString("  status = %1:").arg(Util::toString(m_status)));
    for (const auto &rule : m_rules) {
        for (const auto &item : rule.items()) {
            lines.append(QString("  %1/%2[%3s] = (%4/%5):%6:%7")
                             .arg(m_name, rule.name())
                             .arg(item.limit().period())
                             .arg(item.state().hits())
                             .arg(item.limit().hits())
                             .arg(item.limit().period())
                             .arg(QString::number(item.limit().restriction())));
        }
    }
    lines.append("</POLICY_STATE>");
    return lines.join("\n");
}

QString RateLimitPolicy::GetHistoryReport(const std::deque<RateLimit::Event> &history) const
{
    QStringList lines;
    lines.append(QString("<HISTORY_STATE policy='%1'>").arg(m_name));
    for (size_t i = 0; i < history.size(); ++i) {
        const auto &event = history[i];
        const QString line = QString("  %1 #%2 (request_id=%3): sent %4, received %5, reply %6 "
                                     "(status=%7, url='%8')")
                                 .arg(m_name)
                                 .arg(i + 1)
                                 .arg(event.request_id)
                                 .arg(Timestamp(event.request_time),
                                      Timestamp(event.received_time),
                                      Timestamp(event.reply_time))
                                 .arg(event.reply_status)
                                 .arg(event.request_url);
        lines.append(line);
    }
    lines.append("</HISTORY_STATE>");
    return lines.join("\n");
}

QDateTime RateLimitPolicy::GetNextSafeSend(const std::deque<RateLimit::Event> &history)
{
    const QDateTime now = QDateTime::currentDateTime().toLocalTime();

    // We can send immediately if the status is OK.
    if (m_status < RateLimit::Status::BORDERLINE) {
        return now;
    }

    spdlog::debug("Rate Limiting: calculating next send for BORDERLINE policy: {}", m_name);

    QDateTime next_send(now);

    QStringList lines;
    lines.append(QString("===== BORDERLINE_REPORT(%1) =====").arg(Timestamp(now)));
    lines.append(GetPolicyReport());
    lines.append(GetHistoryReport(history));

    for (const auto &rule : m_rules) {
        for (const auto &item : rule.items()) {
            const auto period = item.limit().period();
            const auto max_hits = item.limit().hits();
            const auto current_hits = item.state().hits();

            const QString tag = QString("%1/%2[%3s]").arg(m_name, rule.name()).arg(period);

            // If this item is not limiting, we can skip it.
            if (current_hits < max_hits) {
                lines.append(QString("%1: skipping rule because state is %2/%3")
                                 .arg(tag)
                                 .arg(current_hits)
                                 .arg(max_hits));
                continue;
            }

            // Determine how far back into the history we can look.
            const size_t hits = static_cast<size_t>(max_hits);
            const size_t len = history.size();
            const size_t n = (len < hits) ? len : hits;
            lines.append(QString("%1: n=%2/%3").arg(tag).arg(n).arg(len));

            // Start with the timestamp of the earliest known
            // reply relevant to this limitation.
            QDateTime t;
            if (n < 1) {
                t = now;
                lines.append(QString("%1: using current time: %1").arg(tag, Timestamp(t)));
            } else {
                const auto &event = history[n - 1];
                lines.append(QString("%1: using history event:").arg(tag));
                lines.append(QString("<EVENT index=%1, history_size=%2>").arg(n).arg(len));
                lines.append(QString("  request_id    = %1").arg(event.request_id));
                lines.append(QString("  request_url   = %1").arg(event.request_url));
                lines.append(QString("  request_time  = %1").arg(Timestamp(event.request_time)));
                lines.append(QString("  received_time = %1").arg(Timestamp(event.received_time)));
                lines.append(QString("  reply_time    = %1").arg(Timestamp(event.reply_time)));
                lines.append(QString("  reply_status  = %1").arg(event.reply_status));
                lines.append(QString("</EVENT>"));
                // Find the latest time and use it. This helps us avoid violations due
                // to things like clock differences and network delays.
                const QDateTime &request_time = event.request_time.isValid() ? event.request_time
                                                                             : now;
                const QDateTime &received_time = event.received_time.isValid() ? event.received_time
                                                                               : now;
                const QDateTime &reply_time = event.reply_time.isValid() ? event.reply_time : now;
                t = std::max({request_time, received_time, reply_time});
                lines.append(QString("%1: using most recent time: %2").arg(tag, Timestamp(t)));
            }
            // Add the measurement period.
            t = t.addSecs(period);
            lines.append(QString("%1: send is %2 after adding %3 seconds for period")
                             .arg(tag, Timestamp(t))
                             .arg(period));

            // Determine which timing resolution applies.
            const int delay = ((period <= INITIAL_VS_SUSTAINED_PERIOD_CUTOFF)
                                   ? INITIAL_TIMING_BUCKET_SECS
                                   : SUSTAINED_TIMING_BUCKET_SECS)
                              + TIMING_BUCKET_BUFFER_SECS;

            // Add the timing resolution.
            t = t.addSecs(delay);
            lines.append(QString("%1: send is %2 after adding %3 seconds for timing bucket")
                             .arg(tag, Timestamp(t))
                             .arg(delay));

            // Check to see if we need to update the final result.
            if (next_send < t) {
                lines.append(QString("%1: updating next send from %2 to %3")
                                 .arg(tag, Timestamp(next_send), Timestamp(t)));
                next_send = t;
            } else {
                lines.append(QString("Next send is unchanged").arg(tag));
            }
        }
    }
    spdlog::debug("Rate Limiting: next send for '{}' is {}", m_name, Timestamp(next_send));
    lines.append(QString("Next send for '%1' is %2").arg(m_name, Timestamp(next_send)));
    lines.append(QString("================================="));

    m_report = lines.join("\n");

    return next_send;
}
