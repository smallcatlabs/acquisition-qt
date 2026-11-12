// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#include "ratelimit/ratelimiter.h"

#include <QCoroNetworkReply>

#include <QCoreApplication>
#include <QNetworkReply>
#include <QThread>

#include <algorithm>

#include "ratelimit/fetcherror.h"
#include "ratelimit/networkcapture.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitmanager.h"
#include "ratelimit/ratelimitpolicy.h"
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

constexpr int UPDATE_INTERVAL_MSEC = 1000;
constexpr int MIN_REPORTED_PAUSE_MSEC = 1000;

// The setup-failure cooldown (D4, named, provisional): a caller
// resubmitting from its completion handler cannot loop HEAD probes against
// N16's narrow one-HEAD-at-boot sanction.
constexpr int SETUP_RETRY_COOLDOWN_SECS = 60;

// The HEAD-429 hold (D4): the D3 retry formula's degenerate case, applied
// to the established pump's first send. Mirrors the constants in
// ratelimitmanager.cpp.
constexpr int RETRY_BUCKET_PAD_SECS = 60;
constexpr int RETRY_BUFFER_SECS = 1;

RateLimiter::RateLimiter(NetworkManager &network_manager, RateLimit::Scheduler *scheduler)
    : m_network_manager(network_manager)
    , m_scheduler(scheduler ? *scheduler : m_own_scheduler)
{
    spdlog::trace("RateLimiter::RateLimiter() entered");
    m_update_timer.setObjectName("statusTimer");
    m_update_timer.setSingleShot(false);
    m_update_timer.setInterval(UPDATE_INTERVAL_MSEC);
    connect(&m_update_timer, &QTimer::timeout, this, &RateLimiter::SendStatusUpdate);
}

RateLimiter::~RateLimiter() {}

void RateLimiter::EnableCapture(const QString &file_path)
{
    spdlog::info("Network capture enabled: {}", file_path);
    m_capture = std::make_unique<NetworkCapture>(file_path);
}

QFuture<RateLimit::FetchOutcome> RateLimiter::SubmitFuture(const QString &endpoint,
                                                           QNetworkRequest network_request,
                                                           std::stop_token token)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    // Established: the endpoint is handled by an existing policy manager.
    auto it = m_manager_by_endpoint.find(endpoint);
    if (it != m_manager_by_endpoint.end()) {
        RateLimitManager &manager = *it->second;
        spdlog::trace("Rate Limit policy {} is handling '{}': {}",
                      manager.policy().name(),
                      endpoint,
                      network_request.url().toString());
        return manager.QueueRequest(endpoint, network_request, std::move(token));
    }

    // Unknown under a cooldown: fail fast with the recorded failure — no
    // probe is sent inside the window (D4). The completion is synchronous,
    // which futures make safe: a continuation attached after the fact still
    // runs (the queued hop the signal boundary needed is unnecessary here).
    auto cooldown = m_cooldowns.find(endpoint);
    if (cooldown != m_cooldowns.end()) {
        if (m_scheduler.Now() < cooldown->second.cooldown_until) {
            spdlog::error("Rate limiter: endpoint '{}' is cooling down after a setup failure; "
                          "failing fast: {}",
                          endpoint,
                          cooldown->second.message);
            RateLimitedRequest entry(endpoint, network_request, std::move(token));
            QFuture<RateLimit::FetchOutcome> future = entry.promise.future();
            CompleteWithFailure(entry, cooldown->second);
            return future;
        }
        m_cooldowns.erase(cooldown);
    }

    auto entry = std::make_unique<RateLimitedRequest>(endpoint, network_request, std::move(token));
    QFuture<RateLimit::FetchOutcome> future = entry->promise.future();

    // Probing: park behind the in-flight HEAD, keeping submission order.
    auto probing = m_probing.find(endpoint);
    const bool already_probing = probing != m_probing.end();
    if (!already_probing) {
        spdlog::debug("New endpoint encountered: '{}': {}",
                      endpoint,
                      network_request.url().toString());
    }
    ParkEntry(endpoint, std::move(entry));
    emit QueueUpdate(endpoint, static_cast<int>(m_probing[endpoint].size()));

    if (!already_probing) {
        // Sweep completed probe handles; a live one is never destroyed (S1-1).
        m_probe_tasks.remove_if([](const QCoro::Task<> &task) { return task.isReady(); });
        m_probe_tasks.push_back(ProbeEndpoint(endpoint, network_request));
    }

    return future;
}

void RateLimiter::ParkEntry(const QString &endpoint, std::unique_ptr<RateLimitedRequest> entry)
{
    const unsigned long id = entry->id;
    const std::stop_token token = entry->token;
    auto &parked = m_probing[endpoint];
    parked.push_back({std::move(entry), nullptr});

    if (!token.stop_possible()) {
        return;
    }
    // The callback runs synchronously inside request_stop(), so it may only
    // schedule: completing or destroying the entry here would run caller
    // code before request_stop() returned. The scheduled prune guards its
    // own lookup, so a pruned-in-the-meantime entry is simply not found.
    parked.back().prune = std::make_unique<PruneCallback>(token, [this, endpoint, id]() {
        m_scheduler.CallAt(m_scheduler.Now(), this, [this, endpoint, id]() {
            PruneParkedEntry(endpoint, id);
        });
    });
}

void RateLimiter::PruneParkedEntry(const QString &endpoint, unsigned long id)
{
    auto probing = m_probing.find(endpoint);
    if (probing == m_probing.end()) {
        // Setup resolved first: the entry was forwarded or failed, and its
        // own path completed it.
        return;
    }
    auto &parked = probing->second;
    const auto it = std::find_if(parked.begin(), parked.end(), [id](const ParkedEntry &p) {
        return p.entry && p.entry->id == id;
    });
    if (it == parked.end()) {
        return;
    }
    spdlog::debug("Rate limiter: a request parked for '{}' was canceled before setup finished",
                  endpoint);

    // Stabilize the deque BEFORE completing, the way EstablishEndpoint and
    // FailSetup already do. A default QFuture continuation runs
    // synchronously on the completing stack, so a caller can resubmit to
    // this very endpoint from its cancellation handler — that reaches
    // ParkEntry, which push_backs onto this same deque and invalidates
    // every iterator into it. Erasing after completing would then be
    // undefined behavior.
    auto slot = std::move(*it);
    slot.prune.reset();
    parked.erase(it);
    emit QueueUpdate(endpoint, static_cast<int>(parked.size()));

    CompleteRequest(*slot.entry,
                    RateLimit::FetchError::Kind::Canceled,
                    "canceled while waiting for endpoint setup");
    // The probe is deliberately left running: it teaches the topology even
    // with nothing behind it, and cancellation is not a setup failure — no
    // cooldown is installed (D4).
}

QCoro::Task<> RateLimiter::ProbeEndpoint(QString endpoint, QNetworkRequest request)
{
    // No exception escapes a root coroutine (IR4): an escape is contained
    // as a setup failure under the normal cooldown.
    try {
        // A HEAD takes the whole gate (D5): nothing else in flight while a
        // probe runs, and concurrent endpoint setups serialize here. The
        // probe proceeds even if every parked entry dies (D4), so the wait
        // is explicitly non-cancelable.
        auto permit = co_await m_gate.AcquireHead(std::stop_token{});

        spdlog::debug("Sending a HEAD for endpoint: {}", endpoint);
        permit.MarkDispatched();
        QNetworkReply *raw = m_network_manager.head(request);
        // The dispatch-time RAII owner (R5-4): the probe frame owns the
        // HEAD reply on every live-session path; parked entries are only
        // ever handed synthetic replies, never this one.
        std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)> reply(raw, [](QNetworkReply *r) {
            r->deleteLater();
        });

        co_await raw;

        // The permit span ends at reply-finish (D5).
        permit.Release();

        ProcessHeadResponse(endpoint, request, raw);
    } catch (const std::exception &e) {
        spdlog::error("Rate limiter: the HEAD probe for '{}' threw an exception: {}",
                      endpoint,
                      e.what());
        FailSetup(endpoint,
                  {m_scheduler.Now() + std::chrono::seconds(SETUP_RETRY_COOLDOWN_SECS),
                   RateLimit::FetchError::Kind::Internal,
                   QNetworkReply::UnknownNetworkError,
                   0,
                   QString("HEAD probe failed internally: %1").arg(e.what())});
    } catch (...) {
        spdlog::error("Rate limiter: the HEAD probe for '{}' threw an unknown exception", endpoint);
        FailSetup(endpoint,
                  {m_scheduler.Now() + std::chrono::seconds(SETUP_RETRY_COOLDOWN_SECS),
                   RateLimit::FetchError::Kind::Internal,
                   QNetworkReply::UnknownNetworkError,
                   0,
                   QString("HEAD probe failed internally")});
    }
}

void RateLimiter::ProcessHeadResponse(const QString &endpoint,
                                      const QNetworkRequest &request,
                                      QNetworkReply *reply)
{
    spdlog::trace("RateLimiter::ProcessHeadResponse() endpoint='{}', url='{}'",
                  endpoint,
                  request.url().toString());

    // Capture the probe before any validation, so degraded HEAD replies are
    // recorded too.
    if (m_capture) {
        m_capture->RecordHeadResponse(endpoint, reply);
    }

    const int status = RateLimit::ParseStatus(reply);
    const auto parsed = RateLimitPolicy::Parse(reply);
    const auto retry_after = RateLimit::ParseRetryAfter(reply);

    // The failure-side cooldown honors a validly-present Retry-After (D4).
    const auto cooldown_until = m_scheduler.Now()
                                + std::chrono::seconds(
                                    std::max(SETUP_RETRY_COOLDOWN_SECS, retry_after.value_or(0)));

    // A HEAD 429 is genuinely reachable (counters persist across restarts,
    // N24) and is a real server-side violation either way: record it.
    if (status == 429) {
        ++m_violation_count;
        spdlog::error("Rate limiter: the HEAD probe for '{}' was 429ed (violation {} this "
                      "session)",
                      endpoint,
                      m_violation_count);
        if (parsed && retry_after) {
            // Full headers with a valid Retry-After still teach the
            // topology: establish the pump with its next send held past
            // Retry-After + pad + buffer; the HEAD consumes no attempt.
            const auto hold = m_scheduler.Now()
                              + std::chrono::seconds(*retry_after + RETRY_BUCKET_PAD_SECS
                                                     + RETRY_BUFFER_SECS);
            EstablishEndpoint(endpoint, reply, parsed->name(), hold);
            return;
        }
        LogSetupReply(request, reply);
        FailSetup(endpoint,
                  {cooldown_until,
                   RateLimit::FetchError::Kind::RateLimited,
                   QNetworkReply::UnknownServerError,
                   status,
                   QString("HEAD probe for '%1' was rate limited without a usable reply "
                           "(Full headers: %2, acceptable Retry-After: %3)")
                       .arg(endpoint, parsed ? "yes" : "no", retry_after ? "yes" : "no")});
        return;
    }

    // Any other non-2xx status. The status governs even though Qt also
    // reports these as reply errors — InternalServerError for a 500, and
    // so on (D8 precedence rule 2). A pure transport failure carries no
    // status line at all (ParseStatus yields 0) and belongs to the
    // network branch below. The reply's own Qt error code is kept on the
    // synthetic for diagnostics; the message carries the classification.
    if (status != 0 && (status < 200 || status > 299)) {
        spdlog::error("The HEAD request for '{}' failed with HTTP status {}", endpoint, status);
        LogSetupReply(request, reply);
        FailSetup(endpoint,
                  {cooldown_until,
                   RateLimit::FetchError::Kind::Http,
                   reply->error() != QNetworkReply::NoError ? reply->error()
                                                            : QNetworkReply::UnknownServerError,
                   status,
                   QString("HTTP status %1 in HEAD reply for '%2'")
                       .arg(QString::number(status), endpoint)});
        return;
    }

    // Transport failures — no status at all (connection refused, timeout,
    // SSL, ...) or an error alongside a 2xx status (the truncated-reply
    // case; D8 precedence rule 3).
    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("The HEAD reply for '{}' had a network error: {} ({})",
                      endpoint,
                      reply->error(),
                      reply->errorString());
        LogSetupReply(request, reply);
        FailSetup(endpoint,
                  {cooldown_until,
                   RateLimit::FetchError::Kind::Network,
                   reply->error(),
                   status,
                   QString("Network error in HEAD reply for '%1': %2")
                       .arg(endpoint, reply->errorString())});
        return;
    }

    // A clean 2xx whose rate-limit headers fail the total parse is a setup
    // failure too (D4/D8): the endpoint must never run unpaced, and a
    // synthetic default policy is rejected by design.
    if (!parsed) {
        spdlog::error("The HEAD response for '{}' did not contain a usable rate limit policy: {}",
                      endpoint,
                      parsed.error());
        LogSetupReply(request, reply);
        FailSetup(endpoint,
                  {cooldown_until,
                   RateLimit::FetchError::Kind::Protocol,
                   QNetworkReply::ProtocolFailure,
                   status,
                   QString("The HEAD response for '%1' did not contain a usable rate limit "
                           "policy: %2")
                       .arg(endpoint, parsed.error())});
        return;
    }

    // Log the rate-limit headers the probe taught us.
    QStringList lines;
    lines.append(QString("<HEAD_RESPONSE_HEADERS policy_name='%1'>").arg(parsed->name()));
    const auto raw_headers = reply->rawHeaderList();
    for (const auto &name : raw_headers) {
        if (QString::fromUtf8(name).startsWith("X-Rate-Limit", Qt::CaseInsensitive)) {
            lines.append(QString("%1 = '%2'").arg(name, reply->rawHeader(name)));
        }
    }
    lines.append("</HEAD_RESPONSE_HEADERS>");
    spdlog::debug("HEAD response received for {}:\n{}", parsed->name(), lines.join("\n"));

    EstablishEndpoint(endpoint, reply, parsed->name(), std::nullopt);
}

void RateLimiter::EstablishEndpoint(const QString &endpoint,
                                    QNetworkReply *head_reply,
                                    const QString &policy_name,
                                    std::optional<std::chrono::milliseconds> hold_until)
{
    RateLimitManager &manager = GetManager(endpoint, policy_name);
    manager.Update(head_reply);
    if (hold_until) {
        manager.HoldUntil(*hold_until);
    }

    // Forward the parked entries in submission order. A probe that
    // succeeded with no live entries simply leaves the endpoint
    // established and idle.
    auto parked = std::move(m_probing.at(endpoint));
    m_probing.erase(endpoint);
    for (auto &slot : parked) {
        // Drop the prune first: once the pump owns the entry, the hub must
        // not reach for it again.
        slot.prune.reset();
        manager.Enqueue(std::move(slot.entry));
    }
}

void RateLimiter::FailSetup(const QString &endpoint, SetupFailure failure)
{
    spdlog::error("Rate limiter: endpoint setup FAILED for '{}': {} — cooling down before any "
                  "re-probe",
                  endpoint,
                  failure.message);

    // Install the cooldown BEFORE any completion is emitted: completions
    // run caller code synchronously, and a caller that resubmits from its
    // completion handler must hit the fail-fast branch, not find the
    // endpoint Unknown and start another HEAD — the exact probe loop the
    // cooldown exists to prevent (D4).
    //
    // The re-entrancy is bounded by state, not by a queued hop: the
    // fail-fast path completes SYNCHRONOUSLY on the future boundary
    // (SubmitFuture), which is safe precisely because the cooldown here and
    // the parked deque below are both stabilized before anything settles, so
    // a re-entrant submission always sees consistent state.
    m_cooldowns[endpoint] = failure;

    auto probing = m_probing.find(endpoint);
    if (probing != m_probing.end()) {
        auto parked = std::move(probing->second);
        m_probing.erase(probing);
        for (auto &slot : parked) {
            slot.prune.reset();
            // A stopped entry completes Canceled even though setup also
            // failed. Its prune was already scheduled and would have said
            // Canceled; without this check the outcome would depend on
            // whether the HEAD failed before that callback ran — and on the
            // success path it always ends up Canceled. Cancellation is the
            // caller's own decision, so it wins over a failure the caller
            // never waited to see.
            if (slot.entry->token.stop_requested()) {
                CompleteRequest(*slot.entry,
                                RateLimit::FetchError::Kind::Canceled,
                                "canceled while waiting for endpoint setup");
                continue;
            }
            CompleteWithFailure(*slot.entry, failure);
        }
    }
}

void RateLimiter::CompleteWithFailure(RateLimitedRequest &entry, const SetupFailure &failure)
{
    RateLimit::FetchError error;
    error.kind = failure.kind;
    error.endpoint = entry.endpoint;
    error.url = entry.network_request.url();
    error.http_status = failure.http_status;
    error.network_error = failure.error;
    if (failure.error != QNetworkReply::NoError) {
        error.network_error_string = failure.message;
    }
    error.message = failure.message;
    CompleteRequest(entry, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
}

void RateLimiter::LogSetupReply(const QNetworkRequest &request, const QNetworkReply *reply)
{
    NetworkManager::logRequest(request);
    NetworkManager::logReply(reply);
}

RateLimitManager &RateLimiter::GetManager(const QString &endpoint, const QString &policy_name)
{
    spdlog::trace("RateLimiter::GetManager() entered");
    spdlog::trace("RateLimiter::GetManager() endpoint = {}", endpoint);
    spdlog::trace("RateLimiter::GetManager() policy_name = {}", policy_name);

    auto it = m_manager_by_policy.find(policy_name);
    if (it == m_manager_by_policy.end()) {
        // Create a new policy manager.
        spdlog::debug("Creating rate limit policy {} for {}", policy_name, endpoint);
        auto sender = std::bind_front(&RateLimiter::SendRequest, this);
        auto mgr = std::make_unique<RateLimitManager>(sender, m_scheduler, m_gate, m_capture.get());
        auto &manager = m_managers.emplace_back(std::move(mgr));
        connect(manager.get(),
                &RateLimitManager::PolicyUpdated,
                this,
                &RateLimiter::OnPolicyUpdated);
        connect(manager.get(), &RateLimitManager::QueueUpdated, this, &RateLimiter::OnQueueUpdated);
        connect(manager.get(), &RateLimitManager::Paused, this, &RateLimiter::OnManagerPaused);
        connect(manager.get(), &RateLimitManager::Violation, this, &RateLimiter::OnViolation);
        m_manager_by_policy[policy_name] = manager.get();
        m_manager_by_endpoint[endpoint] = manager.get();
        return *manager;
    } else {
        // Use an existing policy manager.
        spdlog::debug("Using an existing rate limit policy {} for {}", policy_name, endpoint);
        RateLimitManager *manager = it->second;
        m_manager_by_endpoint[endpoint] = manager;
        return *manager;
    }
}

QNetworkReply *RateLimiter::SendRequest(const QNetworkRequest &request)
{
    return m_network_manager.get(request);
}

void RateLimiter::OnPolicyUpdated(const RateLimitPolicy &policy)
{
    spdlog::trace("RateLimiter::OnPolicyUpdated() entered");
    emit PolicyUpdate(policy);
}

void RateLimiter::OnQueueUpdated(const QString &policy_name, int queued_requests)
{
    emit QueueUpdate(policy_name, queued_requests);
}

void RateLimiter::OnManagerPaused()
{
    if (!m_update_timer.isActive()) {
        m_update_timer.start();
    }
}

void RateLimiter::OnViolation(const QString &policy_name)
{
    ++m_violation_count;
    spdlog::error(
        "RateLimiter: {} was violated. So far {} rate limit violations have been detected.",
        policy_name,
        m_violation_count);
}

void RateLimiter::SendStatusUpdate()
{
    // The pause is read from the pumps, never stored here: a pump's wait
    // ends when its deadline passes or its caller gives up, and either way
    // it stops reporting one (#284). Sub-second waits are ordinary pacing,
    // not rate limiting, so they are not reported.
    RateLimitManager *soonest = nullptr;
    int soonest_msec = 0;
    for (const auto &manager : m_managers) {
        const int msec = manager->msecToNextSend();
        if ((msec >= MIN_REPORTED_PAUSE_MSEC) && (!soonest || (msec < soonest_msec))) {
            soonest = manager.get();
            soonest_msec = msec;
        }
    }

    if (soonest) {
        // Rounded up, so a live pause never reads as zero.
        const int seconds = (soonest_msec + 999) / 1000;
        m_reporting_pause = true;
        emit Paused(seconds, soonest->policy().name());
        return;
    }

    spdlog::trace("RateLimiter::SendStatusUpdate() stopping status updates");
    m_update_timer.stop();
    if (m_reporting_pause) {
        // Reported once: nothing is paused any more.
        m_reporting_pause = false;
        emit Paused(0, QString());
    }
}
