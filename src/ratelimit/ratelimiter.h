// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QCoroTask>

#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>

#include <QFuture>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "ratelimit/fetcherror.h"
#include "ratelimit/gate.h"
#include "ratelimit/ratelimitedrequest.h"
#include "ratelimit/scheduler.h"

class NetworkCapture;
class NetworkManager;
class RateLimitManager;
class RateLimitPolicy;
// The generated moc file registers signals/slots that take a RateLimitPolicy,
// so it needs the full type, not the forward declaration (GCC 16
// -Wsfinae-incomplete, issue #217).
Q_MOC_INCLUDE("ratelimit/ratelimitpolicy.h")

// The hub (network-redesign spec, D4/D5): endpoint-to-policy topology, HEAD
// setup, the gate, and capture. Every endpoint the hub knows is in exactly
// one state — Unknown -> Probing -> Established — with any setup failure
// returning to Unknown under a cooldown. The first submission for an
// unknown endpoint parks and fires the HEAD asynchronously (the old nested
// event loop is gone — F5); submissions arriving mid-setup park behind it
// in order. Setup failures fail the parked requests cleanly with errored
// replies instead of killing the application (the FatalError paths are
// deleted).
class RateLimiter : public QObject
{
    Q_OBJECT

public:
    // Create a rate limiter. The scheduler is injectable for tests; by
    // default the hub runs on its own precise timers.
    RateLimiter(NetworkManager &network_manager, RateLimit::Scheduler *scheduler = nullptr);

    ~RateLimiter();

    // Submit a request and get the future its outcome arrives on (D1): body
    // bytes on success, a FetchError value otherwise. The token is the
    // caller's cancellation channel (D2); callers with no abort story pass a
    // default, never-stopped one. Virtual so tests can substitute an offline
    // fake (tests/fakenetwork.h).
    //
    // The future may already be finished when it is returned (the cooldown
    // fail-fast path), which is safe: a continuation attached to a finished
    // future still runs.
    virtual QFuture<RateLimit::FetchOutcome> SubmitFuture(const QString &endpoint,
                                                          QNetworkRequest network_request,
                                                          std::stop_token token = {});

    // Start recording every rate-limited exchange to a JSONL capture file
    // (see docs/design/network-ground-truth.md). Call before the first
    // submission so no manager is created without the capture hook.
    void EnableCapture(const QString &file_path);

public slots:
    // Used by the GUI to request a manual refresh.
    void OnUpdateRequested();

signals:
    // Emitted when one of the policy managers has signalled a policy update.
    void PolicyUpdate(const RateLimitPolicy &policy);

    // Emitted when a request has been added to a queue. Parked traffic is
    // labelled by endpoint until the endpoint is established, then by
    // policy name.
    void QueueUpdate(const QString &policy_name, int queued_requests);

    // Signal sent to the UI so the user can see what's going on.
    void Paused(int seconds, const QString &policy_name);

private slots:

    void SendStatusUpdate();

    // Received from individual policy managers.
    void OnPolicyUpdated(const RateLimitPolicy &policy);

    // Received from individual policy managers.
    void OnQueueUpdated(const QString &policy_name, int queued_requests);

    // Received from individual policy managers.
    void OnManagerPaused(const QString &policy_name, const QDateTime &until);

    // Recieved from indivual policy managers.
    void OnViolation(const QString &policy_name);

private:
    // An entry parked while its endpoint is Probing. The hub owns the entry
    // (and therefore its promise) until setup resolves and it is either
    // forwarded to a pump or completed here.
    //
    // A parked entry whose token stops is pruned: the callback fires
    // synchronously inside request_stop(), so it only schedules the prune on
    // the scheduler — the same queued-resume discipline the gate and the
    // sleep primitive use, so request_stop() returns before anything is
    // completed or destroyed. Cancellation never installs a cooldown and
    // never abandons the probe: the HEAD continues and teaches the topology
    // even when every entry behind it is gone (D4).
    // std::stop_callback is neither movable nor copyable, so it is held
    // indirectly — the parked deque has to stay movable for erase().
    using PruneCallback = std::stop_callback<std::function<void()>>;

    struct ParkedEntry
    {
        std::unique_ptr<RateLimitedRequest> entry;
        std::unique_ptr<PruneCallback> prune;
    };

    // What a setup failure leaves behind: the fail-fast window and the
    // failure's shape, replayed to submissions arriving inside it.
    struct SetupFailure
    {
        std::chrono::milliseconds cooldown_until;
        RateLimit::FetchError::Kind kind;
        QNetworkReply::NetworkError error;
        int http_status;
        QString message;
    };

    // The setup coroutine for one endpoint: acquire the gate's exclusive
    // HEAD permit, probe, then establish or fail. Its body is catch-all
    // wrapped (no exception escapes a root coroutine — IR4).
    QCoro::Task<> ProbeEndpoint(QString endpoint, QNetworkRequest request);

    // Classify a landed HEAD reply and transition the endpoint.
    void ProcessHeadResponse(const QString &endpoint,
                             const QNetworkRequest &request,
                             QNetworkReply *reply);

    // Probing -> Established: create-or-join the pump by policy name,
    // update it from the HEAD reply, and forward the parked entries in
    // submission order. A HEAD 429's hold (a deadline on the scheduler's
    // clock) is applied first (D4).
    void EstablishEndpoint(const QString &endpoint,
                           QNetworkReply *head_reply,
                           const QString &policy_name,
                           std::optional<std::chrono::milliseconds> hold_until);

    // Probing -> Unknown under a cooldown: fail every parked entry with the
    // FetchError the failure's shape describes.
    void FailSetup(const QString &endpoint, SetupFailure failure);

    // Complete a parked entry with the setup failure's FetchError.
    void CompleteWithFailure(RateLimitedRequest &entry, const SetupFailure &failure);

    // Park an entry behind an in-flight (or about to be fired) HEAD probe,
    // installing its cancellation prune.
    void ParkEntry(const QString &endpoint, std::unique_ptr<RateLimitedRequest> entry);

    // Drop a parked entry whose token stopped, completing it Canceled.
    // Scheduled, never called from inside request_stop().
    void PruneParkedEntry(const QString &endpoint, unsigned long id);

    // Log extra details about the HEAD request and replies
    void LogSetupReply(const QNetworkRequest &request, const QNetworkReply *reply);

    // Get or create the rate limit policy manager for the given endpoint.
    RateLimitManager &GetManager(const QString &endpoint, const QString &policy_name);

    // This function is passed to individual managers via a bound
    // function so they can send network requests without having
    // to know anything about OAuth.
    QNetworkReply *SendRequest(const QNetworkRequest &network_request);

    // Reference to the Application's network access manager.
    NetworkManager &m_network_manager;

    // The clock/timer driving the gate and the pumps: the injected one, or
    // the owned production scheduler. Declared before everything that
    // references it.
    RateLimit::TimerScheduler m_own_scheduler;
    RateLimit::Scheduler &m_scheduler;

    // The layer-1 gate (D5): every send the pumps and the setup path
    // dispatch acquires it. Declared before the managers so it outlives
    // them.
    RateLimit::Gate m_gate{m_scheduler};

    // Research instrument shared by all policy managers; null unless
    // capture is enabled.
    std::unique_ptr<NetworkCapture> m_capture;

    QTimer m_update_timer;

    std::map<QDateTime, QString> m_pauses;

    // Probing endpoints and their parked entries, in submission order.
    std::map<QString, std::deque<ParkedEntry>> m_probing;

    // Probe task handles (owned, never fire-and-forget). Completed handles
    // are swept when a new probe starts; a live handle is never destroyed
    // mid-session (S1-1).
    std::list<QCoro::Task<>> m_probe_tasks;

    // Endpoints under a setup-failure cooldown (D4): submissions inside
    // the window fail fast with the recorded failure, no probe sent.
    std::map<QString, SetupFailure> m_cooldowns;

    std::list<std::unique_ptr<RateLimitManager>> m_managers;
    std::map<const QString, RateLimitManager *> m_manager_by_policy;
    std::map<const QString, RateLimitManager *> m_manager_by_endpoint;

    unsigned int m_violation_count{0};
};
