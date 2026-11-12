// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QCoroTask>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>

#include <QDateTime>
#include <QFuture>
#include <QNetworkRequest>
#include <QObject>
#include <QString>

#include "ratelimit/fetcherror.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitedrequest.h"

class QNetworkReply;

class NetworkCapture;
class RateLimitPolicy;
// The generated moc file registers signals/slots that take a RateLimitPolicy,
// so it needs the full type, not the forward declaration (GCC 16
// -Wsfinae-incomplete, issue #217).
Q_MOC_INCLUDE("ratelimit/ratelimitpolicy.h")

namespace RateLimit {
    class Gate;
    class Scheduler;
} // namespace RateLimit

// Manages a single rate limit policy, which may apply to multiple endpoints.
//
// The policy pump (network-redesign spec, D3): a plain deque of entries and
// an on-demand drain coroutine — one linear loop (take, pace, gate, send,
// maybe retry, deliver) instead of the old timer-and-signal state machine.
// The 429 retry is a loop iteration: invisible to the caller, who sees
// exactly one final completion (the F57 wedge is unconstructible).
//
// Completion is a QFuture<FetchOutcome> (D1): body bytes on success, a
// FetchError value otherwise. Every entry's promise reaches a final state —
// payload, error, or Canceled — so awaiting is unconditionally safe (D2/ER1).
class RateLimitManager : public QObject
{
    Q_OBJECT

public:
    // This is the signature of the function used to send requests.
    using SendFcn = std::function<QNetworkReply *(QNetworkRequest &)>;

    RateLimitManager(SendFcn sender,
                     RateLimit::Scheduler &scheduler,
                     RateLimit::Gate &gate,
                     NetworkCapture *capture = nullptr);
    ~RateLimitManager();

    // Queue a request and return the future the caller awaits. The drain
    // starts (or is joined) if a policy is installed; entries queued before
    // the first Update() wait for it.
    QFuture<RateLimit::FetchOutcome> QueueRequest(const QString &endpoint,
                                                  const QNetworkRequest &request,
                                                  std::stop_token token);

    // Adopt an entry whose future was already handed to a caller — the hub's
    // path for forwarding requests parked during endpoint setup.
    void Enqueue(std::unique_ptr<RateLimitedRequest> entry);

    // What a landed reply's rate-limit headers told us. Separating
    // observation from classification (R6-3/D8) is what lets the pump keep
    // pacing current from a 429 or a 500 while still failing a would-be-clean
    // 2xx that carries unusable headers.
    enum class Observation {
        // Full headers naming this pump's policy: the policy was updated.
        Updated,
        // The headers failed the total parse (absent, malformed, partial).
        Unparseable,
        // Full headers naming a different policy: nothing was updated.
        NameMismatch,
    };

    // Install or update the policy from a reply's rate-limit headers.
    // Total-parse gated (D8): headers that fail the grammar update nothing,
    // and a Full set whose policy name does not match the installed
    // policy's is refused loudly — a pump's policy name never changes after
    // creation (D4). Definition changes under the same name are adopted;
    // shape changes also clear request history because it describes a
    // different set of counters.
    Observation Update(QNetworkReply *reply);

    const RateLimitPolicy &policy();

    // Hold the next send until no earlier than the given deadline on the
    // scheduler's clock — the D4 HEAD-429 case: the hub establishes (or
    // joins) the pump but the next send waits out Retry-After + pad +
    // buffer. A monotonic deadline, not a wall time: it is re-checked
    // after gate waits, so it must not drift against the sleep clock.
    // Sends pace normally once the hold has passed.
    void HoldUntil(std::chrono::milliseconds until);

    // Milliseconds until the next scheduled send: the remaining pacing or
    // retry sleep, measured on the injected scheduler; -1 when no send is
    // scheduled (idle, waiting at the gate, or in flight).
    int msecToNextSend() const;

signals:
    // Emitted when the underlying policy has been updated.
    void PolicyUpdated(const RateLimitPolicy &policy);

    // Emitted when a request has been added to the queue;
    void QueueUpdated(const QString policy_name, int queued_requests);

    // Emitted when a network request has to wait to be sent.
    void Paused(const QString &policy_name, const QDateTime &until);

    // Emitted when a rate limit violation has been detected.
    void Violation(const QString &policy_name);

private:
    // The drain coroutine: processes entries until the deque is empty,
    // then returns. Started by QueueRequest (or Update, for entries queued
    // before the policy arrived) when none is running. Its whole body is
    // wrapped in a catch-all — no exception ever escapes a root coroutine
    // (IR4/R5-1): an escape completes nothing, fails the queue, and puts
    // the pump in a terminal failed state, loudly.
    QCoro::Task<> Drain();

    // One entry, start to final outcome: pacing sleep, then up to
    // MAX_ATTEMPTS gate-acquire/send/classify rounds per the D3 attempt
    // table.
    QCoro::Task<> ProcessEntry(RateLimitedRequest &entry);

    // Per-send bookkeeping, always first (D3): every landed reply records
    // its history event and capture record, then is parse-attempted for a
    // policy update (observation precedes classification, R6-3/D8). Returns
    // what the headers said, which classification then consults.
    Observation RecordLandedReply(RateLimitedRequest &entry, QNetworkReply *reply);

    // Build a FetchError pre-filled with everything the entry and its reply
    // know: endpoint, URL, status, and the Qt error. The caller sets the
    // kind, the attempt count, and the message.
    RateLimit::FetchError MakeError(const RateLimitedRequest &entry,
                                    QNetworkReply *reply,
                                    int status) const;

    // Emit Paused and remember the deadline msecToNextSend() reports.
    void AnnouncePause(const QDateTime &until, std::chrono::milliseconds deadline);

    // Used to print log messages about rate limit violations.
    void LogPolicyHistory();

    // Function handle used to send network requests.
    const SendFcn m_sender;

    // Injected clock/timer and the hub's layer-1 gate (D5). Every send
    // acquires the gate; pacing and retry sleeps run on the scheduler.
    RateLimit::Scheduler &m_scheduler;
    RateLimit::Gate &m_gate;

    // Optional research instrument (see docs/design/network-ground-truth.md);
    // owned by the RateLimiter, null when capture is disabled.
    NetworkCapture *const m_capture;

    // Keep a unique_ptr to the policy associated with this manager,
    // which will be updated whenever a reply with valid rate limit
    // headers and a matching policy name is received.
    std::unique_ptr<RateLimitPolicy> m_policy;

    // Requests that are waiting to be processed by the drain.
    std::deque<std::unique_ptr<RateLimitedRequest>> m_queue;

    // The drain task handle (owned member, never fire-and-forget). Only
    // replaced while no drain is running — replacing it then destroys a
    // completed frame, never detaches a live one (S1-1).
    QCoro::Task<> m_drain_task;
    bool m_draining = false;

    // Set when the drain's catch-all trips: the pump failed terminally and
    // loudly; later submissions are refused (no restart — a restart on
    // throw risks a tight crash loop; the next session starts clean).
    bool m_failed = false;

    // The deadline of the pacing or retry sleep currently in progress, on
    // the scheduler's clock; empty when no send is scheduled.
    std::optional<std::chrono::milliseconds> m_next_send_deadline;

    // Sends are held until this scheduler-clock deadline when set
    // (HoldUntil).
    std::optional<std::chrono::milliseconds> m_earliest_send;

    // We use a history of the received reply times so that we can calculate
    // when the next safe send time will be. This allows us to calculate the
    // least delay necessary to stay compliant.
    std::deque<RateLimit::Event> m_history;
    size_t m_history_size{0};
};
