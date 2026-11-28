// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "ratelimit/ratelimitmanager.h"

#include <QCoroNetworkReply>

#include <QNetworkReply>

#include <algorithm>

#include "ratelimit/fetcherror.h"
#include "ratelimit/gate.h"
#include "ratelimit/networkcapture.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitedrequest.h"
#include "ratelimit/ratelimitpolicy.h"
#include "ratelimit/scheduler.h"
#include "ratelimit/stopsleep.h"
#include "util/fatalerror.h"
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

// For debugging rate limit violations, keep around more history than should be needed
constexpr int HISTORY_BUFFER = 5;

// This HTTP status code means there was a rate limit violation.
constexpr int VIOLATION_STATUS = 429;

// A delay added to every send to avoid flooding the server.
constexpr int NORMAL_BUFFER_MSEC = 100;

// Total sends per entry: one original plus two retries (D3). Bounded so a
// systemically broken policy cannot hammer the API (N10).
constexpr int MAX_ATTEMPTS = 3;

// The retry deadline is max(now + Retry-After + pad + buffer,
// GetNextSafeSend(history)) — D3. The pad is unconditional: 60 s is the
// largest bucket tier GGG has named for any policy (N12), assumed to cover
// the legacy policy too; no policy inspection, so the retry path is immune
// to the initial-vs-sustained question (Q4). The buffer matches the timing
// bucket buffer in ratelimitpolicy.cpp.
constexpr int RETRY_BUCKET_PAD_SECS = 60;
constexpr int RETRY_BUFFER_SECS = 1;

// Maximum time we expect a request to take. This is used to detect
// issues like timezones and clock errors.
constexpr int MAXIMUM_API_RESPONSE_SEC = 60;

// This is another parameter used to check the system clock.
constexpr int MAXIMUM_EARLY_ARRIVAL_SEC = 30;

namespace {

    // The dispatch-time RAII reply owner (D3/R5-4): every attempt's reply is
    // released exactly once — by this guard for retried or abandoned
    // attempts, or by the caller after the final reply is handed over.
    struct DeferredDelete
    {
        void operator()(QNetworkReply *reply) const { reply->deleteLater(); }
    };
    using ReplyGuard = std::unique_ptr<QNetworkReply, DeferredDelete>;

} // namespace

RateLimitManager::RateLimitManager(SendFcn sender,
                                   RateLimit::Scheduler &scheduler,
                                   RateLimit::Gate &gate,
                                   NetworkCapture *capture)
    : m_sender(sender)
    , m_scheduler(scheduler)
    , m_gate(gate)
    , m_capture(capture)
    , m_policy(nullptr)
{
    spdlog::trace("RateLimitManager::RateLimitManager() entered");
}

RateLimitManager::~RateLimitManager() {}

void RateLimitManager::HoldUntil(std::chrono::milliseconds until)
{
    if (!m_earliest_send || *m_earliest_send < until) {
        m_earliest_send = until;
    }
}

const RateLimitPolicy &RateLimitManager::policy()
{
    if (!m_policy) {
        FatalError("The rate limit manager's policy is null!");
    }
    return *m_policy;
}

int RateLimitManager::msecToNextSend() const
{
    if (!m_next_send_deadline) {
        return -1;
    }
    const auto remaining = *m_next_send_deadline - m_scheduler.Now();
    return static_cast<int>(std::max<std::chrono::milliseconds::rep>(remaining.count(), 0));
}

RateLimitManager::Observation RateLimitManager::Update(QNetworkReply *reply)
{
    spdlog::trace("RateLimitManager::Update() entered");

    // Get the rate limit policy from this reply. The parse is total (D8):
    // a reply whose rate-limit headers fail the grammar updates nothing —
    // today's parser crashed on these inputs.
    auto parsed = RateLimitPolicy::Parse(reply);
    if (!parsed) {
        // A reply that already failed at the transport, or carries no
        // rate-limit headers at all, is expected to fail this parse: GGG
        // does not header every error response, and there is nothing to
        // diagnose. The loud error is reserved for the case that actually
        // indicates a protocol change — a would-be-clean 2xx, which
        // classification then completes as Protocol (F50/IR1).
        const bool degraded = reply->error() != QNetworkReply::NoError
                              || !reply->hasRawHeader("X-Rate-Limit-Policy");
        const auto level = degraded ? spdlog::level::debug : spdlog::level::err;
        spdlog::log(level,
                    "Rate Limit Policy: not updating '{}': the reply's rate-limit headers "
                    "failed to parse: {}",
                    m_policy ? m_policy->name() : QString("<no policy>"),
                    parsed.error());
        return Observation::Unparseable;
    }

    // If there was an existing policy, compare them.
    if (m_policy) {
        // A pump's policy name never changes after creation (D4/D8): a
        // mismatched name leaves the policy byte-for-byte un-updated, and
        // the affected request is surfaced as a Protocol error.
        if (m_policy->name() != parsed->name()) {
            spdlog::error("Rate Limit Policy: REFUSING to update '{}' from a reply naming a "
                          "different policy '{}' — the policy is left un-updated (D8)",
                          m_policy->name(),
                          parsed->name());
            return Observation::NameMismatch;
        }
        // A changed shape under the same name can occur when authentication
        // changes which counters the server applies. Adopt it, but discard
        // history recorded against the old counter set.
        if (!m_policy->HasSameShape(*parsed)) {
            spdlog::info("Rate limit policy '{}' changed shape; request history was cleared",
                         m_policy->name());
            spdlog::debug("Rate limit policy '{}' shape transition:\nOld Policy:\n{}\nNew "
                          "Policy:\n{}",
                          m_policy->name(),
                          m_policy->GetPolicyReport(),
                          parsed->GetPolicyReport());
            m_history.clear();
        } else if (!m_policy->Check(*parsed)) {
            // A changed definition with the same shape is adopted: dynamic
            // limit changes must update pacing state. Check() logs the diff.
            spdlog::error("Rate Limit Policy: the updated policy is mismatched:\nCurrent "
                          "Policy:\n{}\nNew Policy:\n{}",
                          m_policy->GetPolicyReport(),
                          parsed->GetPolicyReport());
        }
    }

    // Update the rate limit policy.
    m_policy = std::make_unique<RateLimitPolicy>(std::move(*parsed));

    // Grow the history capacity if needed.
    const size_t max_hits = m_policy->maximum_hits();
    const size_t history_size = max_hits + HISTORY_BUFFER;
    if (history_size > m_history_size) {
        spdlog::debug("{}: increasing history size from {} events to {} events.",
                      m_policy->name(),
                      m_history_size,
                      history_size);
        m_history_size = history_size;
    }

    emit PolicyUpdated(policy());

    // Entries queued before the policy arrived can drain now.
    if (!m_draining && !m_failed && !m_queue.empty()) {
        m_draining = true;
        m_drain_task = Drain();
    }
    return Observation::Updated;
}

QFuture<RateLimit::FetchOutcome> RateLimitManager::QueueRequest(const QString &endpoint,
                                                                const QNetworkRequest &request,
                                                                std::stop_token token)
{
    auto entry = std::make_unique<RateLimitedRequest>(endpoint, request, std::move(token));
    QFuture<RateLimit::FetchOutcome> future = entry->promise.future();
    Enqueue(std::move(entry));
    return future;
}

void RateLimitManager::Enqueue(std::unique_ptr<RateLimitedRequest> entry)
{
    if (m_failed) {
        // The terminal state refuses new work, but it refuses it cleanly:
        // the caller gets a completion, never a promise that never settles.
        spdlog::error("The rate limit pump for '{}' is in the terminal failed state; refusing "
                      "the request for {}",
                      m_policy ? m_policy->name() : QString("<no policy>"),
                      entry->endpoint);
        CompleteRequest(*entry,
                        RateLimit::FetchError::Kind::Internal,
                        "the rate limit pump is in a terminal failed state");
        return;
    }
    m_queue.push_back(std::move(entry));
    if (m_draining) {
        emit QueueUpdated(m_policy->name(), static_cast<int>(m_queue.size()));
        return;
    }
    if (!m_policy) {
        // Held until Update() installs a policy: the drain needs pacing
        // state to run (the hub's Update-before-Queue contract).
        return;
    }
    m_draining = true;
    m_drain_task = Drain();
}

QCoro::Task<> RateLimitManager::Drain()
{
    // No exception ever escapes a root coroutine (IR4/R5-1): the catch-all
    // is what implements the terminal failed state.
    QString failure;
    try {
        while (!m_queue.empty()) {
            auto entry = std::move(m_queue.front());
            m_queue.pop_front();
            emit QueueUpdated(m_policy->name(), static_cast<int>(m_queue.size()));
            // The scoped completion guard (D2): however ProcessEntry leaves
            // — normally, or by an exception unwinding through here — this
            // entry's promise is settled before it dies. CompleteRequest is
            // a no-op once the entry completed itself, so the guard costs
            // nothing on the normal path and closes the abandoned one.
            struct CompletionGuard
            {
                RateLimitedRequest &entry;
                ~CompletionGuard()
                {
                    CompleteRequest(entry,
                                    RateLimit::FetchError::Kind::Internal,
                                    "the rate limit pump abandoned the request without "
                                    "completing it");
                }
            } guard{*entry};
            co_await ProcessEntry(*entry);
        }
    } catch (const std::exception &e) {
        m_failed = true;
        failure = QString::fromUtf8(e.what());
        spdlog::error("The rate limit pump for '{}' FAILED with an exception: {} — {} queued "
                      "requests dropped; later submissions will be refused",
                      m_policy ? m_policy->name() : QString("<no policy>"),
                      e.what(),
                      m_queue.size());
    } catch (...) {
        m_failed = true;
        failure = QStringLiteral("unknown exception");
        spdlog::error("The rate limit pump for '{}' FAILED with an unknown exception — {} queued "
                      "requests dropped; later submissions will be refused",
                      m_policy ? m_policy->name() : QString("<no policy>"),
                      m_queue.size());
    }
    if (m_failed) {
        // Captured as well as logged (the exception policy): the first real
        // occurrence of an unexpected event is what the capture is for.
        if (m_capture) {
            m_capture->RecordPumpFailure(m_policy ? m_policy->name() : QString("<no policy>"),
                                         failure,
                                         static_cast<qsizetype>(m_queue.size()));
        }
        // Fail the remaining entries fast — each with a real completion, so
        // no caller is left awaiting a promise that never settles.
        for (auto &entry : m_queue) {
            CompleteRequest(*entry,
                            RateLimit::FetchError::Kind::Internal,
                            "the rate limit pump failed terminally before this request was sent");
        }
        m_queue.clear();
        m_next_send_deadline.reset();
    }
    m_draining = false;
}

QCoro::Task<> RateLimitManager::ProcessEntry(RateLimitedRequest &entry)
{
    // Stop checkpoint (D2/ER4): dequeue. Every checkpoint completes Canceled
    // and returns — Canceled is an outcome, not a failure.
    if (entry.token.stop_requested()) {
        CompleteRequest(entry,
                        RateLimit::FetchError::Kind::Canceled,
                        "canceled before the request was sent");
        co_return;
    }

    // Pacing sleep: the same GetNextSafeSend arithmetic as ever (N25/N26),
    // plus the normal buffer while the policy is below borderline.
    QDateTime next_send = m_policy->GetNextSafeSend(m_history);
    if (m_policy->status() < RateLimit::Status::BORDERLINE) {
        next_send = next_send.addMSecs(NORMAL_BUFFER_MSEC);
    }
    std::chrono::milliseconds deadline
        = m_scheduler.Now()
          + std::chrono::milliseconds(
              std::max<qint64>(QDateTime::currentDateTime().msecsTo(next_send), 0));
    // An externally-imposed hold (the D4 HEAD-429 case) is a deadline on
    // the scheduler's clock; when it is later than the pacing deadline it
    // governs, and the announced wall time is derived from it.
    if (m_earliest_send && *m_earliest_send > deadline) {
        deadline = *m_earliest_send;
        next_send = QDateTime::currentDateTime().addMSecs((deadline - m_scheduler.Now()).count());
    }
    entry.scheduled_time = next_send;
    if (deadline > m_scheduler.Now()) {
        spdlog::trace("{} waiting {} msecs to send request {}",
                      m_policy->name(),
                      (deadline - m_scheduler.Now()).count(),
                      entry.id);
        AnnouncePause(next_send, deadline);
        co_await RateLimit::SleepUntil(m_scheduler, deadline, entry.token);
        m_next_send_deadline.reset();
        // Stop checkpoint: after the pacing sleep.
        if (entry.token.stop_requested()) {
            CompleteRequest(entry,
                            RateLimit::FetchError::Kind::Canceled,
                            "canceled while waiting to be paced");
            co_return;
        }
    }

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        // Every send acquires the gate (D5). A stopped wait yields an
        // invalid permit without ever holding a slot — that is the stop
        // checkpoint for gate acquisition.
        auto permit = co_await m_gate.Acquire(entry.token);
        // An invalid permit means the wait lost to the token. A VALID permit
        // does not mean the token is still live: the gate settles a grant
        // and the waiter resumes through the event loop, so a stop landing
        // in that window still yields a granted permit (gate.cpp's
        // GrantPass says so explicitly — a granted waiter always resumes).
        // Checking only validity would dispatch an already-stopped request,
        // which is the one thing this checkpoint exists to prevent.
        // Releasing without dispatching charges no spacing interval.
        if (!permit.valid() || entry.token.stop_requested()) {
            permit.Release();
            CompleteRequest(entry,
                            RateLimit::FetchError::Kind::Canceled,
                            "canceled while waiting at the gate");
            co_return;
        }

        // A hold may have been installed while this entry waited at the
        // gate: a HEAD-429 probe joining this pump (shared policy, D4)
        // installs its hold behind the very HEAD the entry is queued
        // behind, and the pacing check above ran long before. Release
        // without dispatching (an undispatched release charges no spacing
        // interval, so other lanes proceed while we wait), sleep out the
        // hold, and reacquire — re-checking, in case it was extended
        // meanwhile. A hold-wait consumes no attempt: nothing was sent.
        while (m_earliest_send && *m_earliest_send > m_scheduler.Now()) {
            const std::chrono::milliseconds hold = *m_earliest_send;
            const QDateTime hold_until = QDateTime::currentDateTime().addMSecs(
                (hold - m_scheduler.Now()).count());
            // The hold is now this entry's scheduling decision: keep the
            // capture's expected-send timestamp honest (the same rule as
            // the retry path), or the record would portray the deliberate
            // hold as unexplained lateness.
            entry.scheduled_time = hold_until;
            permit.Release();
            AnnouncePause(hold_until, hold);
            co_await RateLimit::SleepUntil(m_scheduler, hold, entry.token);
            m_next_send_deadline.reset();
            if (entry.token.stop_requested()) {
                CompleteRequest(entry,
                                RateLimit::FetchError::Kind::Canceled,
                                "canceled while waiting out a rate limit hold");
                co_return;
            }
            permit = co_await m_gate.Acquire(entry.token);
            // Same grant-then-stop window as the acquisition above.
            if (!permit.valid() || entry.token.stop_requested()) {
                permit.Release();
                CompleteRequest(entry,
                                RateLimit::FetchError::Kind::Canceled,
                                "canceled while waiting at the gate");
                co_return;
            }
        }

        entry.send_time = QDateTime::currentDateTime().toLocalTime();
        permit.MarkDispatched();
        ReplyGuard reply(m_sender(entry.network_request));
        if (!reply) {
            // A sender that cannot send is a wiring bug; contained as a
            // pump failure rather than a null dereference.
            throw std::runtime_error("the rate limit manager's sender returned no reply");
        }

        co_await reply.get();

        // The permit span ends at reply-finish (D5): a permit is never
        // held across a retry sleep (R4-1).
        permit.Release();

        // Bookkeeping first, then observation, then classification (D3).
        const Observation observation = RecordLandedReply(entry, reply.get());

        const int status = RateLimit::ParseStatus(reply.get());
        const auto retry_after = RateLimit::ParseRetryAfter(reply.get());

        // Violation accounting runs before any stop or retry decision: the
        // server counted the exchange whatever the client does next (N25).
        if (status == VIOLATION_STATUS) {
            spdlog::error("Rate limit violation detected for policy '{}':\n{}",
                          m_policy->name(),
                          m_policy->GetBorderlineReport());
            LogPolicyHistory();
            emit Violation(m_policy->name());
        } else if (reply->error() == QNetworkReply::NoError
                   && m_policy->status() >= RateLimit::Status::VIOLATION) {
            // The reply looked fine but the headers report a violation.
            spdlog::error("Reply did not have an error, but the rate limit policy shows a "
                          "violation occured.");
            spdlog::error("Rate limit violation detected for policy '{}':\n{}",
                          m_policy->name(),
                          m_policy->GetBorderlineReport());
            LogPolicyHistory();
            emit Violation(m_policy->name());
        }

        // Stop checkpoint: post-land, AFTER bookkeeping and violation
        // accounting, before any retry decision (D2/N25 — let it land). A
        // canceled entry has still taught the pump everything its reply had
        // to teach; only the caller's interest in the result is gone.
        if (entry.token.stop_requested()) {
            CompleteRequest(entry,
                            RateLimit::FetchError::Kind::Canceled,
                            "canceled after the reply landed");
            co_return;
        }

        // D8 classification, in precedence order: 429, then any other
        // non-2xx status, then any remaining Qt error (including alongside
        // a 2xx — the truncated-body case), then the header observation,
        // then success.
        if (status == VIOLATION_STATUS) {
            if (retry_after && attempt < MAX_ATTEMPTS) {
                const QDateTime now = QDateTime::currentDateTime();
                QDateTime retry_at = now.addSecs(*retry_after + RETRY_BUCKET_PAD_SECS
                                                 + RETRY_BUFFER_SECS);
                const QDateTime safe_send = m_policy->GetNextSafeSend(m_history);
                if (retry_at < safe_send) {
                    retry_at = safe_send;
                }
                spdlog::error("Rate limit VIOLATION for policy {} (retrying at {}, attempt "
                              "{} of {})",
                              m_policy->name(),
                              retry_at.toString(),
                              attempt + 1,
                              MAX_ATTEMPTS);
                entry.scheduled_time = retry_at;
                const qint64 retry_delay = std::max<qint64>(now.msecsTo(retry_at), 0);
                AnnouncePause(retry_at, m_scheduler.Now() + std::chrono::milliseconds(retry_delay));
                co_await RateLimit::SleepUntil(m_scheduler, *m_next_send_deadline, entry.token);
                m_next_send_deadline.reset();
                // Stop checkpoint: after the retry sleep.
                if (entry.token.stop_requested()) {
                    CompleteRequest(entry,
                                    RateLimit::FetchError::Kind::Canceled,
                                    "canceled while waiting to retry after a rate limit "
                                    "violation");
                    co_return;
                }
                // The guard releases this attempt's 429 reply; the next
                // attempt re-acquires the gate like every send (ER7).
                continue;
            }
            // Terminal 429: retries exhausted, or no acceptable Retry-After
            // (missing, non-numeric, negative, or above the cap). Complete
            // immediately — never sleep on an exhausted attempt (D3). One
            // kind for every terminal 429 (R7); the fields say which case
            // this was.
            if (!retry_after) {
                spdlog::error("Rate limit violation for policy {} is not retryable: no "
                              "acceptable Retry-After",
                              m_policy->name());
            } else {
                spdlog::error("Rate limit violation for policy {}: retries exhausted after {} "
                              "attempts",
                              m_policy->name(),
                              attempt);
            }
            RateLimit::FetchError error = MakeError(entry, reply.get(), status);
            error.kind = RateLimit::FetchError::Kind::RateLimited;
            error.attempts = attempt;
            error.retry_after_acceptable = retry_after.has_value();
            error.message = retry_after
                                ? QString("rate limited by policy '%1': retries exhausted after "
                                          "%2 attempts")
                                      .arg(m_policy->name(), QString::number(attempt))
                                : QString("rate limited by policy '%1' with no acceptable "
                                          "Retry-After")
                                      .arg(m_policy->name());
            CompleteRequest(entry, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
            co_return;
        }

        if (status != 0 && (status < 200 || status > 299)) {
            // Non-429 failures are not retried. The status governs even
            // though Qt reports many of these as reply errors too (D8
            // precedence rule 2).
            spdlog::error("policy manager for {} request {} reply status was {} and error was {}",
                          m_policy->name(),
                          entry.id,
                          status,
                          reply->error());
            NetworkManager::logRequest(entry.network_request);
            NetworkManager::logReply(reply.get());
            RateLimit::FetchError error = MakeError(entry, reply.get(), status);
            error.kind = RateLimit::FetchError::Kind::Http;
            error.attempts = attempt;
            error.message = QString("HTTP status %1").arg(QString::number(status));
            CompleteRequest(entry, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
            co_return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            // A transport failure with no status at all, or an error
            // alongside a 2xx — a body truncated after the status arrived
            // (D8 precedence rule 3).
            spdlog::error("policy manager for {} request {} reply status was {} and error was {}",
                          m_policy->name(),
                          entry.id,
                          status,
                          reply->error());
            NetworkManager::logRequest(entry.network_request);
            NetworkManager::logReply(reply.get());
            RateLimit::FetchError error = MakeError(entry, reply.get(), status);
            error.kind = RateLimit::FetchError::Kind::Network;
            error.attempts = attempt;
            error.message = QString("network error: %1").arg(reply->errorString());
            CompleteRequest(entry, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
            co_return;
        }

        if (observation != Observation::Updated) {
            // A clean 2xx whose rate-limit headers are unusable: the pump
            // cannot pace on them, and a payload cannot be delivered
            // alongside an anomaly flag, so this is a strict error (IR1).
            // The endpoint stays Established and self-heals.
            spdlog::error("Rate limit policy '{}': a clean 2xx reply for request {} carried "
                          "{} rate-limit headers — completing the request as a protocol error "
                          "(D8/IR1)",
                          m_policy->name(),
                          entry.id,
                          observation == Observation::NameMismatch ? "mismatched" : "unusable");
            NetworkManager::logRequest(entry.network_request);
            NetworkManager::logReply(reply.get());
            RateLimit::FetchError error = MakeError(entry, reply.get(), status);
            error.kind = RateLimit::FetchError::Kind::Protocol;
            error.attempts = attempt;
            error.message = observation == Observation::NameMismatch
                                ? QString("a 2xx reply named a policy other than '%1'")
                                      .arg(m_policy->name())
                                : QString("a 2xx reply for policy '%1' carried unusable "
                                          "rate-limit headers")
                                      .arg(m_policy->name());
            CompleteRequest(entry, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
            co_return;
        }

        // Success: the body bytes are the outcome. Read before completing —
        // the reply guard releases it as this frame unwinds.
        CompleteRequest(entry, RateLimit::FetchOutcome(reply->readAll()));
        co_return;
    }
}

RateLimit::FetchError RateLimitManager::MakeError(const RateLimitedRequest &entry,
                                                  QNetworkReply *reply,
                                                  int status) const
{
    RateLimit::FetchError error;
    error.endpoint = entry.endpoint;
    error.url = entry.network_request.url();
    error.http_status = status;
    error.network_error = reply->error();
    if (reply->error() != QNetworkReply::NoError) {
        error.network_error_string = reply->errorString();
    }
    return error;
}

RateLimitManager::Observation RateLimitManager::RecordLandedReply(RateLimitedRequest &entry,
                                                                  QNetworkReply *reply)
{
    const QDateTime now = QDateTime::currentDateTime().toLocalTime();

    // Capture before any validation, so degraded replies are recorded too.
    if (m_capture) {
        m_capture->RecordReply(m_policy->name(), entry, reply, now);
    }

    // Every landed reply records its history event (D3) — the server
    // counted the exchange whatever the client does next (N25).
    RateLimit::Event event;
    event.request_id = entry.id;
    event.request_url = entry.network_request.url().toString();
    event.request_time = entry.send_time;
    event.received_time = now;
    event.reply_time = RateLimit::ParseDate(reply).toLocalTime();
    event.reply_status = RateLimit::ParseStatus(reply);
    m_history.push_front(event);
    if (m_history.size() > m_history_size) {
        m_history.pop_back();
    }

    if (event.reply_time.isValid()) {
        const int response_sec = event.request_time.secsTo(event.reply_time);
        if (response_sec > MAXIMUM_API_RESPONSE_SEC) {
            spdlog::error("WARNING: The system clock may be wrong: an API call seems to have "
                          "taken too long: {} seconds."
                          " This may lead to API rate limit violations.",
                          response_sec);
        } else if (response_sec < -MAXIMUM_EARLY_ARRIVAL_SEC) {
            spdlog::error("WARNING: The system clock may be wrong: an API call seems to have "
                          "been answered {}s before it was made."
                          " This may lead to API rate limit violations.",
                          -response_sec);
        }
    }

    spdlog::trace("RateLimitManager {} received reply for request {} with status {}",
                  m_policy->name(),
                  event.request_id,
                  event.reply_status);

    // Observation follows bookkeeping and precedes classification
    // (R6-3/D8): the headers are parse-attempted on every landed reply,
    // whatever its status — a 429, a 500, and a 2xx-plus-transport-error
    // all keep the policy current. Update() applies the Full-and-matching
    // gate and logs failures loudly.
    const Observation observation = Update(reply);

    if (m_policy->status() == RateLimit::Status::BORDERLINE) {
        spdlog::info("Rate limit policy '{}' is BORDERLINE and the next safe send is at {}",
                     m_policy->name(),
                     m_policy->GetNextSafeSend(m_history).toString());
    }
    return observation;
}

void RateLimitManager::AnnouncePause(const QDateTime &until, std::chrono::milliseconds deadline)
{
    m_next_send_deadline = deadline;
    emit Paused(m_policy->name(), until);
}

void RateLimitManager::LogPolicyHistory()
{
    const QString status = Util::toString(m_policy->status());
    QStringList lines;
    lines.append("Rate Limit Policy details:");
    lines.append(
        QString("<RATE_LIMIT_POLICY policy_name='%1' status='%2'>").arg(m_policy->name(), status));
    for (const auto &rule : m_policy->rules()) {
        for (const auto &item : rule.items()) {
            lines.append(QString("%1:%2(%3s) = %4/%5")
                             .arg(m_policy->name(),
                                  rule.name(),
                                  QString::number(item.limit().period()),
                                  QString::number(item.state().hits()),
                                  QString::number(item.limit().hits())));
        }
    }
    for (size_t i = 0; i < m_history.size(); ++i) {
        const auto &item = m_history[i];
        lines.append(QString("#%1: request %2 sent %3, received %4, status %5: %6")
                         .arg(QString::number(i + 1),
                              QString::number(item.request_id),
                              item.request_time.toString("yyyy-MMM-dd HH:mm:ss.zzz"),
                              item.reply_time.toString("yyyy-MMM-dd HH:mm:ss.zzz"),
                              QString::number(item.reply_status),
                              item.request_url));
    }
    lines.append("</RATE_LIMIT_POLICY>");
    spdlog::error(lines.join("\n"));
}
