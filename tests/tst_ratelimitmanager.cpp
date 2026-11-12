// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QCoroFuture>
#include <QtTest>

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <chrono>
#include <sstream>
#include <stdexcept>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include "fakenetwork.h"
#include "fakescheduler.h"
#include "fakesender.h"
#include "ratelimit/gate.h"
#include "ratelimit/networkcapture.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitmanager.h"
#include "ratelimit/ratelimitpolicy.h"

// Phase-3 harness for the network redesign (docs/design/network-redesign.md,
// "Testing plan" item 2): the coroutine pump against the same fake sender as
// the phase-1 harness, now with the injected FakeScheduler and the gate, so
// every timing assertion is exact on the fake clock — no coarse-timer slack,
// no real sleeping. Pacing deadlines are still derived from the wall clock
// (GetNextSafeSend is untouched policy arithmetic), so bounds that involve a
// wall-to-fake conversion allow a small slop in the harmless direction.
//
// Deliberately flipped phase-1 pins, per the spec:
//  - F57: a 429 retry no longer destroys the caller's reply; the caller sees
//    exactly one final completion.
//  - F58: the dead 1s spacing is deleted; the gate's MIN_SEND_SPACING floor
//    is the deliberate replacement, pinned exactly below.
//  - Name mismatch: a Full reply naming a different policy leaves the
//    installed policy byte-for-byte un-updated AND completes the request
//    FetchError{Protocol} (D8/IR1 — both halves, as of phase 4a).
//  - Queue-before-install: Update() now starts the drain for held entries
//    instead of leaving them stuck until the next QueueRequest.
//  - F59: this boundary is QFuture<FetchOutcome>, so no reply object is
//    handed out here at all — Qt's shared state is the single owner, and the
//    pump releases every reply it creates, final ones included. Phase 4b
//    resolved F59 outright by deleting the last thing that still handed
//    callers an object: the hub's legacy Submit() adapter and its
//    RateLimitedReply.
//  - The terminal failed state completes its queued entries Internal instead
//    of dropping them: no caller waits on a promise that never settles.
// Kept pins: terminal 429s (missing or unacceptable Retry-After) and error
// surfacing, both now as typed FetchError values.

// moc-lexer note (see tst_workerupdate.cpp): declare the Q_OBJECT class
// before any helpers containing string literals with '//' in them.
class RateLimitManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void updateInstallsPolicyFromHeadReply();
    void updateAdoptsChangedPolicyDefinition();
    void policyShapeChangeClearsHistory();
    void nameMismatchLeavesPolicyUntouched();
    void queueBeforePolicyInstallDrainsOnInstall();
    void okPolicySendsAfterNormalBuffer();
    void queueDrainsFifoWithGateSpacing();
    void saturatedPolicyWaitsPeriodPlusBucket_data();
    void saturatedPolicyWaitsPeriodPlusBucket();
    void nonRetryableHttpErrorSurfacesReplyAndAdvances();
    void headerlessNetworkFailureSurfacesReplyAndAdvances();
    void successWithViolationStateEmitsViolation();
    void violation429WithoutRetryAfterSurfacesReplyAndPauses();
    void retry429CompletesCallerExactlyOnce();
    void retryAfterPadDominatesWhenLarger();
    void retriesExhaustedCompleteImmediately();
    void unacceptableRetryAfterIsTerminal_data();
    void unacceptableRetryAfterIsTerminal();
    void drainRestartsAfterQueueEmpties();
    void holdInstalledWhileWaitingAtGateIsHonored();
    void observationUpdatesPolicyOnErrorReplies();
    void retrySleepIsPermitFree();
    void senderExceptionFailsPumpTerminally();
    void protocolErrorOnUnparseableCleanReply();
    void cancelBeforeDequeueCompletesCanceled();
    void cancelDuringPacingSleepCompletesCanceled();
    void cancelWaitingAtGateCompletesCanceled();
    void cancelAfterGateGrantStillPreventsTheSend();
    void cancelDuringRetrySleepCompletesCanceled();
    void cancelInFlightLetsReplyLandAndRecordsViolation();
    void awaitingCoroutineResumesSafelyOnCancel();
    void siblingResumesCanceledWhenSharedSourceStops();
    // #281: Date is optional.
    void headerlessReplyLogsNoDateError();
    void unparseableDateIsStillAnError();
};

namespace {

    using namespace std::chrono_literals;

    // Mirrors NORMAL_BUFFER_MSEC in ratelimitmanager.cpp: the pad added to
    // every send while the policy is below BORDERLINE.
    constexpr int kNormalBufferMsec = 100;

    constexpr auto kGateSpacing = RateLimit::Gate::MIN_SEND_SPACING;

    constexpr const char *kPolicyName = "test-request-limit";
    constexpr const char *kEndpoint = "Test Endpoint";

    QByteArray rfcDateNow()
    {
        return QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date).toUtf8();
    }

    // Synthetic headers for a single-rule ("Ip") policy — the shape GGG
    // actually serves (see the captured example in tst_networkcapture.cpp).
    QList<QNetworkReply::RawHeaderPair> policyHeaders(const QByteArray &limit,
                                                      const QByteArray &state,
                                                      const QByteArray &policy_name = kPolicyName)
    {
        return {
            {"X-Rate-Limit-Policy", policy_name},
            {"X-Rate-Limit-Rules", "Ip"},
            {"X-Rate-Limit-Ip", limit},
            {"X-Rate-Limit-Ip-State", state},
            {"Date", rfcDateNow()},
        };
    }

    QList<QNetworkReply::RawHeaderPair> accountAndIpPolicyHeaders(const QByteArray &ip_limit,
                                                                  const QByteArray &ip_state)
    {
        return {
            {"X-Rate-Limit-Policy", kPolicyName},
            {"X-Rate-Limit-Rules", "Account,Ip"},
            {"X-Rate-Limit-Account", "30:60:60"},
            {"X-Rate-Limit-Account-State", "0:60:0"},
            {"X-Rate-Limit-Ip", ip_limit},
            {"X-Rate-Limit-Ip-State", ip_state},
            {"Date", rfcDateNow()},
        };
    }

    // Install a policy the way the hub's HEAD setup does: parse it out of a
    // finished (HEAD-style) reply and hand it to Update().
    void installPolicy(RateLimitManager &manager, const QByteArray &limit, const QByteArray &state)
    {
        FakeNetworkReply head(QNetworkRequest(QUrl("https://api.example.test/head")),
                              "",
                              QNetworkReply::NoError,
                              nullptr,
                              policyHeaders(limit, state),
                              200);
        manager.Update(&head);
    }

    // A caller awaiting a QFuture<FetchOutcome> (D1), plus the observations
    // the pins need: how many times the promise settled (exactly once, ever
    // — the F57 wedge is unconstructible), what it settled to, and the
    // caller's own stop_source so the cancellation pins can abort it.
    //
    // The continuation carries no context object and runs on whatever stack
    // finishes the promise — synchronously for a future that is already
    // finished (S1-6), which the fail-fast paths rely on.
    struct Caller
    {
        std::stop_source source;
        QFuture<RateLimit::FetchOutcome> future;

        int completions = 0;
        bool succeeded = false;
        QByteArray body;
        std::optional<RateLimit::FetchError> error;

        std::stop_token token() { return source.get_token(); }
        void abort() { source.request_stop(); }

        void attach(QFuture<RateLimit::FetchOutcome> f)
        {
            future = f;
            future.then([this](const RateLimit::FetchOutcome &outcome) {
                ++completions;
                succeeded = outcome.has_value();
                if (outcome) {
                    body = *outcome;
                    error.reset();
                } else {
                    body.clear();
                    error = outcome.error();
                }
            });
        }

        // Convenience accessors so the pins read close to the shape they had
        // when the boundary was a QNetworkReply.
        std::optional<RateLimit::FetchError::Kind> kind() const
        {
            return error ? std::optional(error->kind) : std::nullopt;
        }
        int last_status() const { return error ? error->http_status : (succeeded ? 200 : 0); }
        QNetworkReply::NetworkError last_error() const
        {
            return error ? error->network_error : QNetworkReply::NoError;
        }
    };

    QNetworkRequest request(const QString &leaf)
    {
        return QNetworkRequest(QUrl("https://api.example.test/" + leaf));
    }

    // Deliver queued coroutine resumptions (the QFutureWatcher hop) and
    // deferred deletions (deleteLater posted outside any event loop is not
    // delivered by nested processEvents alone). A fixed pass count keeps
    // this deterministic — no wall-clock dependence.
    void drainEvents()
    {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    // Interleave due-callback delivery with resumption delivery until a
    // chain (sleep fires -> drain resumes -> gate schedules -> grant fires
    // -> send) has fully played out at the current fake instant.
    void settle(FakeScheduler &scheduler)
    {
        for (int i = 0; i < 10; ++i) {
            drainEvents();
            scheduler.AdvanceBy(0ms);
        }
        drainEvents();
    }

    void advanceAndSettle(FakeScheduler &scheduler, std::chrono::milliseconds delta)
    {
        scheduler.AdvanceBy(delta);
        settle(scheduler);
    }

    // The standard fixture: one pump on a fake clock behind a fresh gate.
    // The sender outlives the manager (declared first).
    struct Rig
    {
        FakeScheduler scheduler;
        RateLimit::Gate gate{scheduler};
        FakeSender sender;
        RateLimitManager manager{sender.fcn(), scheduler, gate};
    };

    // A probe-shaped exclusive HEAD occupant for the gate, dispatched like
    // the hub's setup path.
    struct GateBlocker
    {
        bool done = false;
        RateLimit::Gate::Permit permit;
    };

    QCoro::Task<> takeHead(GateBlocker &blocker, RateLimit::Gate &gate)
    {
        blocker.permit = co_await gate.AcquireHead(std::stop_token{});
        blocker.permit.MarkDispatched();
        blocker.done = true;
    }

} // namespace

void RateLimitManagerTest::updateInstallsPolicyFromHeadReply()
{
    Rig rig;

    int policy_updates = 0;
    connect(&rig.manager, &RateLimitManager::PolicyUpdated, [&](const RateLimitPolicy &) {
        ++policy_updates;
    });

    installPolicy(rig.manager, "30:60:60", "0:60:0");

    QCOMPARE(policy_updates, 1);
    QCOMPARE(rig.manager.policy().name(), QString(kPolicyName));
    QCOMPARE(rig.manager.policy().status(), RateLimit::Status::OK);
    QCOMPARE(rig.manager.policy().maximum_hits(), 30);
    // Installing a policy alone never sends anything.
    QCOMPARE(rig.sender.count(), 0);
}

void RateLimitManagerTest::updateAdoptsChangedPolicyDefinition()
{
    Rig rig;

    int policy_updates = 0;
    connect(&rig.manager, &RateLimitManager::PolicyUpdated, [&](const RateLimitPolicy &) {
        ++policy_updates;
    });

    installPolicy(rig.manager, "2:60:60", "0:60:0");
    // A changed definition under the SAME name (limit 2 -> 5) is logged as
    // a mismatch by Check() but adopted: D8 requires dynamic limit changes
    // to update pacing state.
    installPolicy(rig.manager, "5:60:60", "0:60:0");

    QCOMPARE(policy_updates, 2);
    QCOMPARE(rig.manager.policy().maximum_hits(), 5);
}

void RateLimitManagerTest::policyShapeChangeClearsHistory()
{
    Rig rig;
    installPolicy(rig.manager, "1:60:60", "0:60:0");

    Caller first;
    first.attach(rig.manager.QueueRequest(kEndpoint, request("one"), first.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    rig.sender.sent(0).reply->finish(policyHeaders("1:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(first.completions, 1);

    // Authentication can change the same-named policy from Ip-only to
    // Account+Ip. The old request event must not saturate the new Ip rule.
    FakeNetworkReply authenticated(QNetworkRequest(QUrl("https://api.example.test/head")),
                                   "",
                                   QNetworkReply::NoError,
                                   nullptr,
                                   accountAndIpPolicyHeaders("1:60:60", "0:60:0"),
                                   200);
    rig.manager.Update(&authenticated);

    Caller second;
    second.attach(rig.manager.QueueRequest(kEndpoint, request("two"), second.token()));
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
}

void RateLimitManagerTest::nameMismatchLeavesPolicyUntouched()
{
    // The flipped phase-1 pin (D8/IR1): a steady-state reply carrying a
    // DIFFERENT policy name is refused loudly — the policy stays
    // byte-for-byte un-updated and no PolicyUpdated fires. The request
    // still completes normally in phase 3: this boundary can only express
    // failure as an errored reply, and this reply is a clean 200 — phase 4
    // turns the completion into FetchError{Protocol}.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    int policy_updates = 0;
    connect(&rig.manager, &RateLimitManager::PolicyUpdated, [&](const RateLimitPolicy &) {
        ++policy_updates;
    });

    Caller c1;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    rig.sender.sent(0).reply->finish(policyHeaders("20:60:60", "1:60:0", "renamed-request-limit"),
                                     200);
    drainEvents();
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 200);
    QCOMPARE(rig.manager.policy().name(), QString(kPolicyName));
    QCOMPARE(rig.manager.policy().maximum_hits(), 10);
    QCOMPARE(policy_updates, 0);
}

void RateLimitManagerTest::queueBeforePolicyInstallDrainsOnInstall()
{
    // Flipped phase-1 pin: entries queued before any policy is installed
    // are held (the drain needs pacing state), but Update() now starts the
    // drain for them — the old code left them stuck until the NEXT
    // QueueRequest arrived.
    Rig rig;

    Caller first;
    Caller second;
    first.attach(rig.manager.QueueRequest(kEndpoint, request("one"), first.token()));
    second.attach(rig.manager.QueueRequest(kEndpoint, request("two"), second.token()));
    QCOMPARE(rig.manager.msecToNextSend(), -1);
    settle(rig.scheduler);
    QCOMPARE(rig.sender.count(), 0);

    installPolicy(rig.manager, "10:60:60", "0:60:0");
    settle(rig.scheduler);
    QCOMPARE(rig.sender.count(), 0); // the normal buffer still applies

    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    // FIFO: the first held request goes out first.
    QCOMPARE(rig.sender.sent(0).request.url(), request("one").url());

    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(first.completions, 1);

    // The second held request follows, behind the gate's spacing floor
    // (measured from the first dispatch on the fake clock).
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
    QCOMPARE(rig.sender.sent(1).request.url(), request("two").url());
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(second.completions, 1);
}

void RateLimitManagerTest::okPolicySendsAfterNormalBuffer()
{
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    QSignalSpy paused_spy(&rig.manager, &RateLimitManager::Paused);

    Caller caller;
    const QDateTime before = QDateTime::currentDateTime();
    caller.attach(rig.manager.QueueRequest(kEndpoint, request("one"), caller.token()));

    // Nothing is sent synchronously: with an OK policy the send is
    // scheduled NORMAL_BUFFER_MSEC out, and the wait is announced.
    QCOMPARE(rig.sender.count(), 0);
    const int wait = rig.manager.msecToNextSend();
    QVERIFY2(wait > 0 && wait <= kNormalBufferMsec, qPrintable(QString::number(wait)));
    QCOMPARE(paused_spy.count(), 1);
    QCOMPARE(paused_spy.at(0).at(0).toString(), QString(kPolicyName));
    const qint64 pause = before.msecsTo(paused_spy.at(0).at(1).toDateTime());
    QVERIFY2(pause >= kNormalBufferMsec, qPrintable(QString::number(pause)));
    QVERIFY2(pause <= kNormalBufferMsec + 1000, qPrintable(QString::number(pause)));

    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    // In flight: no send is scheduled.
    QCOMPARE(rig.manager.msecToNextSend(), -1);

    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(caller.completions, 1);
    QCOMPARE(caller.last_status(), 200);
    QCOMPARE(caller.last_error(), QNetworkReply::NoError);
    // No caller-owned object to destroy at this boundary: the promise
    // settled exactly once and the shared state is the single owner of the
    // outcome. (F59's remaining half lives in the legacy adapter — see the
    // header note.)
    QVERIFY(caller.future.isFinished());
    QVERIFY(caller.succeeded);
}

void RateLimitManagerTest::queueDrainsFifoWithGateSpacing()
{
    // The F58 flip: consecutive sends are spaced by the gate's
    // MIN_SEND_SPACING floor, measured from the previous dispatch stamp on
    // the fake clock — deliberately, at the right scope, instead of the
    // dead per-manager 1s interval the old code never applied.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    QSignalSpy queue_spy(&rig.manager, &RateLimitManager::QueueUpdated);

    Caller c1;
    Caller c2;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));

    // The drain picks up the first request immediately (0 left waiting);
    // the second joins the queue behind it (1 waiting).
    QCOMPARE(queue_spy.count(), 2);
    QCOMPARE(queue_spy.at(0).at(1).toInt(), 0);
    QCOMPARE(queue_spy.at(1).at(1).toInt(), 1);

    // First dispatch at t=100 on the fake clock.
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    QCOMPARE(rig.sender.sent(0).request.url(), request("one").url());
    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(c1.completions, 1);

    // The second request's pacing sleep ends at ~t=200, but the gate holds
    // it to exactly t=100+MIN_SEND_SPACING=350.
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    advanceAndSettle(rig.scheduler,
                     kGateSpacing - std::chrono::milliseconds(kNormalBufferMsec) - 1ms);
    QCOMPARE(rig.sender.count(), 1);
    advanceAndSettle(rig.scheduler, 1ms);
    QCOMPARE(rig.sender.count(), 2);
    QCOMPARE(rig.sender.sent(1).request.url(), request("two").url());
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(c2.completions, 1);
}

void RateLimitManagerTest::saturatedPolicyWaitsPeriodPlusBucket_data()
{
    QTest::addColumn<QByteArray>("limit");
    QTest::addColumn<QByteArray>("ok_state");
    QTest::addColumn<QByteArray>("full_state");
    QTest::addColumn<int>("expected_wait_msec");

    // The wait is period + timing bucket + 1s buffer, measured from the
    // newest relevant history event (ratelimitpolicy.cpp). Periods at or
    // below the 75s cutoff use the 5s initial bucket; longer periods use
    // the 60s sustained bucket.
    QTest::newRow("short-period") << QByteArray("1:60:60") << QByteArray("0:60:0")
                                  << QByteArray("1:60:0") << 66000;
    QTest::newRow("long-period") << QByteArray("1:80:60") << QByteArray("0:80:0")
                                 << QByteArray("1:80:0") << 141000;
}

void RateLimitManagerTest::saturatedPolicyWaitsPeriodPlusBucket()
{
    QFETCH(QByteArray, limit);
    QFETCH(QByteArray, ok_state);
    QFETCH(QByteArray, full_state);
    QFETCH(int, expected_wait_msec);

    Rig rig;
    installPolicy(rig.manager, limit, ok_state);

    Caller c1;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    const QDateTime before_finish = QDateTime::currentDateTime();
    rig.sender.sent(0).reply->finish(policyHeaders(limit, full_state), 200);
    drainEvents();
    QCOMPARE(c1.completions, 1);
    QCOMPARE(rig.manager.policy().status(), RateLimit::Status::BORDERLINE);

    // The saturated policy schedules the next request a full
    // period-plus-bucket out, announced via Paused, and sends nothing in
    // the meantime. The deadline is derived from wall-clock history events,
    // so the fake-clock wait allows a small wall-elapsed slop downward.
    QSignalSpy paused_spy(&rig.manager, &RateLimitManager::Paused);
    Caller c2;
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));

    QCOMPARE(rig.sender.count(), 1);
    const int wait = rig.manager.msecToNextSend();
    QVERIFY2(wait >= expected_wait_msec - 1000, qPrintable(QString::number(wait)));
    QVERIFY2(wait <= expected_wait_msec, qPrintable(QString::number(wait)));

    QCOMPARE(paused_spy.count(), 1);
    const qint64 pause = before_finish.msecsTo(paused_spy.at(0).at(1).toDateTime());
    QVERIFY2(pause >= expected_wait_msec - 1000, qPrintable(QString::number(pause)));
    QVERIFY2(pause <= expected_wait_msec + 1000, qPrintable(QString::number(pause)));

    advanceAndSettle(rig.scheduler, 250ms);
    QCOMPARE(rig.sender.count(), 1);
    QCOMPARE(c2.completions, 0);
}

void RateLimitManagerTest::nonRetryableHttpErrorSurfacesReplyAndAdvances()
{
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller c1;
    Caller c2;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A non-429 HTTP error is not retried: the errored reply is surfaced to
    // the caller as a completion and the queue advances.
    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"),
                                     500,
                                     QNetworkReply::InternalServerError);
    drainEvents();
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 500);
    QCOMPARE(c1.last_error(), QNetworkReply::InternalServerError);
    QCOMPARE(c1.kind(), RateLimit::FetchError::Kind::Http);
    QCOMPARE(violation_spy.count(), 0);

    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(c2.completions, 1);
}

void RateLimitManagerTest::headerlessNetworkFailureSurfacesReplyAndAdvances()
{
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller c1;
    Caller c2;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A network-level failure produces a reply without rate-limit headers.
    // It is surfaced to the caller as a failed completion and the queue
    // advances; the headers fail the total parse, so the policy is left
    // alone (the exchange still records its history event — D3).
    rig.sender.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 0);
    QCOMPARE(c1.last_error(), QNetworkReply::ConnectionRefusedError);
    QCOMPARE(c1.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(rig.manager.policy().status(), RateLimit::Status::OK);

    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(c2.completions, 1);
}

void RateLimitManagerTest::successWithViolationStateEmitsViolation()
{
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller c1;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A 200 whose headers report state above the limit is still completed
    // normally, but the violation is detected and signalled.
    rig.sender.sent(0).reply->finish(policyHeaders("2:60:60", "3:60:60"), 200);
    drainEvents();
    QCOMPARE(violation_spy.count(), 1);
    QCOMPARE(violation_spy.at(0).at(0).toString(), QString(kPolicyName));
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 200);
}

void RateLimitManagerTest::violation429WithoutRetryAfterSurfacesReplyAndPauses()
{
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller c1;
    Caller c2;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A 429 with no Retry-After is NOT retried: it is a terminal 429 (D3)
    // and the reply is surfaced to the caller. The violation is recorded
    // regardless.
    rig.sender.sent(0).reply->finish(policyHeaders("2:60:60", "3:60:60"),
                                     429,
                                     QNetworkReply::UnknownContentError);
    drainEvents();
    QCOMPARE(violation_spy.count(), 1);
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 429);
    QCOMPARE(c1.kind(), RateLimit::FetchError::Kind::RateLimited);
    // A terminal 429 with no acceptable Retry-After says so in the value.
    QCOMPARE(c1.error->retry_after_acceptable, false);
    QCOMPARE(c1.error->attempts, 1);

    // The violated policy then paces the next request a full
    // period-plus-bucket out (60 + 5 + 1 seconds) instead of sending it.
    QCOMPARE(rig.sender.count(), 1);
    const int wait = rig.manager.msecToNextSend();
    QVERIFY2(wait >= 65000, qPrintable(QString::number(wait)));
    QVERIFY2(wait <= 66000, qPrintable(QString::number(wait)));
    advanceAndSettle(rig.scheduler, 200ms);
    QCOMPARE(rig.sender.count(), 1);
    QCOMPARE(c2.completions, 0);
}

void RateLimitManagerTest::retry429CompletesCallerExactlyOnce()
{
    // The F57 flip: the retry is a drain-loop iteration, invisible to the
    // caller — no destroyed reply, no dropped completion, exactly one final
    // completion when the retried send succeeds. The wedge is gone.
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    auto retry_headers = policyHeaders("2:60:60", "3:60:60");
    retry_headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "1"});
    rig.sender.sent(0).reply->finish(retry_headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();

    // Nothing has completed yet: the retry is invisible to the caller.
    QVERIFY(!victim.future.isFinished());
    QCOMPARE(victim.completions, 0);
    QCOMPARE(violation_spy.count(), 1);

    // The retry deadline is max(now + RetryAfter + pad + buffer,
    // GetNextSafeSend): here the violated policy's 66s dominates the
    // 1+60+1=62s Retry-After formula.
    const int wait = rig.manager.msecToNextSend();
    QVERIFY2(wait >= 65000, qPrintable(QString::number(wait)));
    QVERIFY2(wait <= 66000, qPrintable(QString::number(wait)));

    advanceAndSettle(rig.scheduler, 66s);
    QCOMPARE(rig.sender.count(), 2);
    // The same request is resent, and the intermediate 429 reply has been
    // released by the pump's dispatch-time owner (exactly once — the
    // fake sender's QPointer nulls).
    QCOMPARE(rig.sender.sent(1).request.url(), request("one").url());
    QVERIFY(rig.sender.sent(0).reply.isNull());

    // The retried request succeeds: the caller sees exactly one final
    // completion, carrying bytes rather than a reply. The FINAL reply is now
    // released by the pump's dispatch-time owner too: no reply object
    // escapes the PUMP any more, so there is nothing for a caller to own,
    // leak, or destroy at the wrong moment at this boundary.
    rig.sender.sent(1).reply->finish(policyHeaders("2:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.last_status(), 200);
    QVERIFY(victim.succeeded);
    QVERIFY(rig.sender.sent(1).reply.isNull());
}

void RateLimitManagerTest::retryAfterPadDominatesWhenLarger()
{
    // The N19 pad pin: when Retry-After + RETRY_BUCKET_PAD + buffer exceeds
    // GetNextSafeSend, the retry honors the padded server deadline —
    // RA=10 gives 10+60+1=71s against the policy's 66s.
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    auto retry_headers = policyHeaders("2:60:60", "3:60:60");
    retry_headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "10"});
    rig.sender.sent(0).reply->finish(retry_headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();

    QCOMPARE(victim.completions, 0);
    const int wait = rig.manager.msecToNextSend();
    QVERIFY2(wait >= 70000, qPrintable(QString::number(wait)));
    QVERIFY2(wait <= 71000, qPrintable(QString::number(wait)));

    // Not resent before the padded deadline.
    advanceAndSettle(rig.scheduler, 66s);
    QCOMPARE(rig.sender.count(), 1);
    advanceAndSettle(rig.scheduler, 5s);
    QCOMPARE(rig.sender.count(), 2);
}

void RateLimitManagerTest::retriesExhaustedCompleteImmediately()
{
    // MAX_ATTEMPTS is 3 total sends: after the third 429 the entry
    // completes immediately with the final 429 — never a sleep on an
    // exhausted attempt (D3). Retry-After 0 is valid (the pad dominates).
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));

    auto retry_headers = policyHeaders("2:60:60", "3:60:60");
    retry_headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "0"});

    for (int attempt = 0; attempt < 3; ++attempt) {
        QCOMPARE(rig.sender.count(), attempt + 1);
        rig.sender.sent(attempt).reply->finish(retry_headers,
                                               429,
                                               QNetworkReply::UnknownContentError);
        drainEvents();
        if (attempt < 2) {
            QCOMPARE(victim.completions, 0);
            advanceAndSettle(rig.scheduler, 70s);
        }
    }

    // Terminal, immediately: one completion carrying the final 429, no
    // resend scheduled.
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.last_status(), 429);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::RateLimited);
    // Retries were exhausted, not refused: an acceptable Retry-After was
    // present on every attempt.
    QCOMPARE(victim.error->retry_after_acceptable, true);
    QCOMPARE(victim.error->attempts, 3);
    QCOMPARE(rig.sender.count(), 3);
    QCOMPARE(rig.manager.msecToNextSend(), -1);
}

void RateLimitManagerTest::unacceptableRetryAfterIsTerminal_data()
{
    // The R7 grammar/product-policy vectors: negative and non-numeric fail
    // the grammar; above-the-cap is declined as product policy. All are
    // terminal 429s — the violation is recorded, no retry is scheduled.
    QTest::addColumn<QByteArray>("retry_after");

    QTest::newRow("negative") << QByteArray("-5");
    QTest::newRow("non-numeric") << QByteArray("soon");
    QTest::newRow("above-cap") << QByteArray("901");
}

void RateLimitManagerTest::unacceptableRetryAfterIsTerminal()
{
    QFETCH(QByteArray, retry_after);

    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    auto headers = policyHeaders("2:60:60", "3:60:60");
    headers.append(QNetworkReply::RawHeaderPair{"Retry-After", retry_after});
    rig.sender.sent(0).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.last_status(), 429);
    QCOMPARE(violation_spy.count(), 1);
    QCOMPARE(rig.manager.msecToNextSend(), -1);
    QCOMPARE(rig.sender.count(), 1);
}

void RateLimitManagerTest::drainRestartsAfterQueueEmpties()
{
    // Drain lifecycle: the drain exits when the deque empties; a later
    // submission starts a new one (submit-while-running joins are pinned by
    // the FIFO test above).
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller c1;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(c1.completions, 1);

    Caller c2;
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    QVERIFY(rig.manager.msecToNextSend() > 0);
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(c2.completions, 1);
}

void RateLimitManagerTest::holdInstalledWhileWaitingAtGateIsHonored()
{
    // The review-#2 pin: a hold installed while an entry is already
    // suspended in the gate wait (a HEAD-429 probe joining this pump —
    // the shared-policy case, D4) must still be honored. The entry
    // releases its permit undispatched, sleeps out the hold, and
    // reacquires. The capture rides along to pin that the hold becomes
    // the recorded scheduling decision.
    QTemporaryDir capture_dir;
    QVERIFY(capture_dir.isValid());
    const QString capture_path = capture_dir.filePath("capture.jsonl");

    FakeScheduler scheduler;
    RateLimit::Gate gate{scheduler};
    FakeSender sender;
    NetworkCapture capture{capture_path};
    RateLimitManager manager{sender.fcn(), scheduler, gate, &capture};
    installPolicy(manager, "10:60:60", "0:60:0");

    // An exclusive HEAD holds the whole gate, dispatched at t=0.
    GateBlocker head;
    auto head_task = takeHead(head, gate);
    QVERIFY(head.done && head.permit.valid());

    // The entry paces normally, then parks in Acquire() behind the HEAD.
    Caller caller;
    caller.attach(manager.QueueRequest(kEndpoint, request("one"), caller.token()));
    advanceAndSettle(scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(sender.count(), 0);

    // The probe 429s and joins this pump: a 180s hold lands while the
    // entry is still suspended at the gate; then the HEAD releases.
    manager.HoldUntil(scheduler.Now() + 180s);
    head.permit.Release();

    // The entry is granted at the spacing floor (t=250, from the HEAD's
    // dispatch stamp) but must not send: it gives the permit back
    // undispatched and sleeps to the hold deadline — installed at t=100,
    // so exactly 179750ms remain when queried at t=350.
    advanceAndSettle(scheduler, kGateSpacing);
    QCOMPARE(sender.count(), 0);
    QCOMPARE(manager.msecToNextSend(), 179750);

    advanceAndSettle(scheduler, 180s);
    QCOMPARE(sender.count(), 1);
    sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(caller.completions, 1);
    QCOMPARE(caller.last_status(), 200);

    // The capture recorded the hold as the entry's scheduling decision:
    // 'scheduled' must be the hold deadline, not the stale pre-hold
    // pacing time. Fake time swallowed the 180s wait while wall time
    // barely moved, so the honest record puts 'scheduled' far AFTER
    // 'sent' (both wall-clock) — the stale value would sit within
    // milliseconds of it.
    QFile capture_file(capture_path);
    QVERIFY(capture_file.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = capture_file.readAll().split('\n');
    QCOMPARE(lines.size(), 2); // one record plus the trailing newline
    const QJsonObject record = QJsonDocument::fromJson(lines[0]).object();
    QCOMPARE(record["kind"].toString(), QString("reply"));
    const QDateTime scheduled = QDateTime::fromString(record["scheduled"].toString(),
                                                      Qt::ISODateWithMs);
    const QDateTime sent = QDateTime::fromString(record["sent"].toString(), Qt::ISODateWithMs);
    QVERIFY(scheduled.isValid() && sent.isValid());
    const qint64 announced_hold_msec = sent.msecsTo(scheduled);
    QVERIFY2(announced_hold_msec > 170000, qPrintable(QString::number(announced_hold_msec)));
    QVERIFY2(announced_hold_msec <= 180000, qPrintable(QString::number(announced_hold_msec)));
}

void RateLimitManagerTest::observationUpdatesPolicyOnErrorReplies()
{
    // Observation precedes classification (R6-3/D8): a 500 and a
    // 2xx-plus-transport-error carrying Full matching headers update the
    // policy while completing their status/network-driven outcomes.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller c1;
    Caller c2;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A 500 with a changed limit definition: the error is surfaced, the
    // policy is updated anyway.
    rig.sender.sent(0).reply->finish(policyHeaders("20:60:60", "1:60:0"),
                                     500,
                                     QNetworkReply::InternalServerError);
    drainEvents();
    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.last_status(), 500);
    QCOMPARE(rig.manager.policy().maximum_hits(), 20);

    // A 2xx whose transport failed mid-body (Qt reports both a status and
    // an error): completes as a failure, still updates the policy.
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.sender.count(), 2);
    rig.sender.sent(1).reply->finish(policyHeaders("25:60:60", "2:60:0"),
                                     200,
                                     QNetworkReply::RemoteHostClosedError);
    drainEvents();
    QCOMPARE(c2.completions, 1);
    QCOMPARE(c2.last_error(), QNetworkReply::RemoteHostClosedError);
    QCOMPARE(rig.manager.policy().maximum_hits(), 25);
}

void RateLimitManagerTest::retrySleepIsPermitFree()
{
    // R4-1: a permit is released at reply-finish and never held across a
    // retry sleep. With the gate's cap at 2, two pumps parked in retry
    // sleeps would otherwise hold both permits — the third pump's send
    // proves the slots were freed.
    FakeScheduler scheduler;
    RateLimit::Gate gate{scheduler};
    FakeSender sender_a;
    FakeSender sender_b;
    FakeSender sender_c;
    RateLimitManager manager_a{sender_a.fcn(), scheduler, gate};
    RateLimitManager manager_b{sender_b.fcn(), scheduler, gate};
    RateLimitManager manager_c{sender_c.fcn(), scheduler, gate};
    installPolicy(manager_a, "2:60:60", "0:60:0");
    installPolicy(manager_b, "2:60:60", "0:60:0");
    installPolicy(manager_c, "2:60:60", "0:60:0");

    Caller ca;
    Caller cb;
    ca.attach(manager_a.QueueRequest(kEndpoint, request("a"), ca.token()));
    cb.attach(manager_b.QueueRequest(kEndpoint, request("b"), cb.token()));
    advanceAndSettle(scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    advanceAndSettle(scheduler, kGateSpacing);
    QCOMPARE(sender_a.count(), 1);
    QCOMPARE(sender_b.count(), 1);

    // Both pumps take a retryable 429 and enter ~66s retry sleeps.
    auto retry_headers = policyHeaders("2:60:60", "3:60:60");
    retry_headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "1"});
    sender_a.sent(0).reply->finish(retry_headers, 429, QNetworkReply::UnknownContentError);
    sender_b.sent(0).reply->finish(retry_headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();
    QVERIFY(manager_a.msecToNextSend() > 60000);
    QVERIFY(manager_b.msecToNextSend() > 60000);

    // While both sleep, the third pump acquires the gate and sends.
    Caller cc;
    cc.attach(manager_c.QueueRequest(kEndpoint, request("c"), cc.token()));
    advanceAndSettle(scheduler, kGateSpacing);
    QCOMPARE(sender_c.count(), 1);
    QVERIFY(manager_a.msecToNextSend() > 0);
    QVERIFY(manager_b.msecToNextSend() > 0);
}

void RateLimitManagerTest::senderExceptionFailsPumpTerminally()
{
    // The exception pins (IR4/R5-1): an exception escaping into the drain
    // is contained — the pump fails terminally and loudly, every queued
    // entry is completed Internal, and later submissions are refused the
    // same way. No crash, no restart, and no caller left waiting forever.
    FakeScheduler scheduler;
    RateLimit::Gate gate{scheduler};
    RateLimitManager manager{[](QNetworkRequest &) -> QNetworkReply * {
                                 throw std::runtime_error("sender exploded");
                             },
                             scheduler,
                             gate};
    installPolicy(manager, "10:60:60", "0:60:0");

    Caller victim;
    Caller queued;
    victim.attach(manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    queued.attach(manager.QueueRequest(kEndpoint, request("two"), queued.token()));
    advanceAndSettle(scheduler, std::chrono::milliseconds(kNormalBufferMsec));

    // The active entry and everything queued behind it are completed
    // Internal rather than dropped: the terminal state fails callers, it
    // does not strand them on a promise that never settles. The active
    // entry's completion comes from the drain's scoped completion guard.
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Internal);
    QCOMPARE(queued.completions, 1);
    QCOMPARE(queued.kind(), RateLimit::FetchError::Kind::Internal);
    QCOMPARE(manager.msecToNextSend(), -1);

    // Later submissions are refused — but cleanly and immediately: one
    // Internal completion, no send attempt, and nothing scheduled that could
    // complete it a second time later.
    Caller after;
    after.attach(manager.QueueRequest(kEndpoint, request("after"), after.token()));
    settle(scheduler);
    QCOMPARE(after.completions, 1);
    QCOMPARE(after.kind(), RateLimit::FetchError::Kind::Internal);
    advanceAndSettle(scheduler, 1s);
    QCOMPARE(after.completions, 1);
}

void RateLimitManagerTest::protocolErrorOnUnparseableCleanReply()
{
    // The other Protocol flavor (D8 rule 4): a clean 2xx whose rate-limit
    // headers fail the total parse. The pump cannot pace on them and cannot
    // deliver a payload alongside an anomaly flag, so the request fails —
    // while the endpoint stays established and self-heals.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller c1;
    c1.attach(rig.manager.QueueRequest(kEndpoint, request("one"), c1.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // A 200 carrying a policy name and rule list but no rule headers: Full
    // is unreachable, so the parse fails.
    rig.sender.sent(0).reply->finish({{"X-Rate-Limit-Policy", kPolicyName},
                                      {"X-Rate-Limit-Rules", "Ip"},
                                      {"Date", rfcDateNow()}},
                                     200);
    drainEvents();

    QCOMPARE(c1.completions, 1);
    QCOMPARE(c1.kind(), RateLimit::FetchError::Kind::Protocol);
    // The policy is left exactly as it was — nothing was adopted from the
    // unusable reply.
    QCOMPARE(rig.manager.policy().maximum_hits(), 10);

    // Self-healing: the next request goes out and succeeds normally.
    Caller c2;
    c2.attach(rig.manager.QueueRequest(kEndpoint, request("two"), c2.token()));
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.sender.count(), 2);
    rig.sender.sent(1).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(c2.completions, 1);
    QVERIFY(c2.succeeded);
}

void RateLimitManagerTest::cancelBeforeDequeueCompletesCanceled()
{
    // Stop checkpoint 1 (D2/ER4): an entry stopped before the drain reaches
    // it never sends. Canceled is an outcome, not a failure — but it is
    // still a completion: the promise always settles (ER1).
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller first;
    Caller victim;
    first.attach(rig.manager.QueueRequest(kEndpoint, request("one"), first.token()));
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("two"), victim.token()));

    // Stop the second entry while the first is still being paced.
    victim.abort();
    QCOMPARE(victim.completions, 0);

    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
    rig.sender.sent(0).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    advanceAndSettle(rig.scheduler, 1s);

    QCOMPARE(first.completions, 1);
    QVERIFY(first.succeeded);
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    // The canceled entry was never sent.
    QCOMPARE(rig.sender.count(), 1);
}

void RateLimitManagerTest::cancelDuringPacingSleepCompletesCanceled()
{
    // Stop checkpoint 2: the pacing sleep is stop-interruptible, so the
    // entry resolves as soon as the token stops rather than at the deadline.
    Rig rig;
    // A saturated policy: the first send waits a long time.
    installPolicy(rig.manager, "2:60:60", "2:60:0");

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    settle(rig.scheduler);
    QVERIFY(rig.manager.msecToNextSend() > 1000);
    QCOMPARE(rig.sender.count(), 0);

    victim.abort();
    settle(rig.scheduler);

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    QCOMPARE(rig.sender.count(), 0);
    // Nothing is left scheduled: the pump went idle rather than sleeping out
    // a deadline for a request nobody wants.
    QCOMPARE(rig.manager.msecToNextSend(), -1);
}

void RateLimitManagerTest::cancelWaitingAtGateCompletesCanceled()
{
    // Stop checkpoint 3: a stopped gate wait yields an invalid permit
    // without ever holding a slot, so the entry completes Canceled and no
    // spacing interval is charged against the other lanes.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    // Occupy the gate exclusively so the entry cannot be granted.
    GateBlocker blocker;
    auto head = takeHead(blocker, rig.gate);
    settle(rig.scheduler);
    QVERIFY(blocker.done);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    // Paced through, now waiting behind the HEAD.
    QCOMPARE(rig.sender.count(), 0);
    QCOMPARE(victim.completions, 0);

    victim.abort();
    settle(rig.scheduler);

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    QCOMPARE(rig.sender.count(), 0);

    // The gate is undamaged: releasing the HEAD lets a fresh request through.
    blocker.permit.Release();
    Caller after;
    after.attach(rig.manager.QueueRequest(kEndpoint, request("two"), after.token()));
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.sender.count(), 1);
}

void RateLimitManagerTest::cancelAfterGateGrantStillPreventsTheSend()
{
    // The grant-then-stop window, which the gate deliberately allows: it
    // settles a grant, and the winner resumes through the event loop, so a
    // stop landing in between still yields a VALID permit (GrantPass says
    // so outright — a granted waiter always resumes). Checking only
    // permit.valid() would let the pump dispatch an already-stopped
    // request, which is the one thing this checkpoint exists to prevent.
    //
    // cancelWaitingAtGateCompletesCanceled does NOT cover this: it aborts
    // while a HEAD occupies the gate, so its waiter is never granted at all.
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    // Occupy the gate so the entry queues as a waiter rather than taking
    // the fast path.
    GateBlocker blocker;
    auto head = takeHead(blocker, rig.gate);
    settle(rig.scheduler);
    QVERIFY(blocker.done);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    // Past both the pacing deadline and the gate's spacing floor, so the
    // release below grants immediately instead of scheduling a later pass.
    advanceAndSettle(rig.scheduler, kGateSpacing + std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 0);

    // Releasing the HEAD runs GrantPass synchronously, which settles the
    // waiter's grant true — but its resume is queued. Stop the token in
    // exactly that window, before any events are delivered.
    blocker.permit.Release();
    victim.abort();

    // Now let the granted waiter resume: it holds a VALID permit and a
    // stopped token.
    settle(rig.scheduler);

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    // Nothing was sent: the permit was released without dispatching.
    QCOMPARE(rig.sender.count(), 0);

    // The gate charged no spacing for the undispatched permit, so a fresh
    // request goes out on the next pacing deadline rather than waiting out
    // a phantom send interval.
    Caller after;
    after.attach(rig.manager.QueueRequest(kEndpoint, request("two"), after.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);
}

void RateLimitManagerTest::cancelDuringRetrySleepCompletesCanceled()
{
    // Stop checkpoint 5: the 429 retry sleep is interruptible too. The
    // violation was already recorded when the 429 landed — cancellation
    // never rewrites what the server counted.
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    auto headers = policyHeaders("2:60:60", "3:60:60");
    headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "5"});
    rig.sender.sent(0).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();

    // Sleeping until the padded retry deadline, nothing completed yet.
    QCOMPARE(violation_spy.count(), 1);
    QCOMPARE(victim.completions, 0);
    QVERIFY(rig.manager.msecToNextSend() > 1000);

    victim.abort();
    settle(rig.scheduler);

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    // No retry was sent, and the violation record stands.
    QCOMPARE(rig.sender.count(), 1);
    QCOMPARE(violation_spy.count(), 1);
}

void RateLimitManagerTest::cancelInFlightLetsReplyLandAndRecordsViolation()
{
    // The let-it-land rule (N25) and checkpoint 4's placement: a request
    // stopped while in flight is NOT abandoned mid-air. The reply lands, its
    // history event and violation are recorded — the server counted the
    // exchange whatever the client wants — and only then does the entry
    // complete Canceled instead of surfacing the outcome.
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "0:60:0");

    QSignalSpy violation_spy(&rig.manager, &RateLimitManager::Violation);

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    // Stop while the request is in flight, then let the 429 land.
    victim.abort();
    settle(rig.scheduler);
    QCOMPARE(victim.completions, 0); // still in flight — nothing to do but wait

    auto headers = policyHeaders("2:60:60", "3:60:60");
    headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "5"});
    rig.sender.sent(0).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();

    // The violation was recorded from the landed reply...
    QCOMPARE(violation_spy.count(), 1);
    QCOMPARE(rig.manager.policy().status(), RateLimit::Status::VIOLATION);
    // ...and the caller sees Canceled, not RateLimited: the retry decision
    // is never reached.
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    QCOMPARE(rig.sender.count(), 1);
}

namespace {

    // A coroutine suspended on a fetch future — the ER1 shape. If a canceled
    // outcome were delivered as a QFuture cancellation rather than as a
    // value, QCoro's awaiter would resume and call result() on a resultless
    // future: undefined behavior no guard written after co_await could
    // prevent. Every promise reaching a final state is what makes this safe.
    QCoro::Task<> awaitOutcome(QFuture<RateLimit::FetchOutcome> future,
                               std::optional<RateLimit::FetchError::Kind> &kind,
                               bool &resumed)
    {
        const RateLimit::FetchOutcome outcome = co_await qCoro(future).takeResult();
        resumed = true;
        if (!outcome) {
            kind = outcome.error().kind;
        }
    }

} // namespace

void RateLimitManagerTest::awaitingCoroutineResumesSafelyOnCancel()
{
    // The ER1 regression pin: a coroutine suspended on its future when the
    // update is aborted resumes normally and reads a Canceled value.
    Rig rig;
    installPolicy(rig.manager, "2:60:60", "2:60:0");

    Caller victim;
    victim.attach(rig.manager.QueueRequest(kEndpoint, request("one"), victim.token()));

    bool resumed = false;
    std::optional<RateLimit::FetchError::Kind> kind;
    auto waiter = awaitOutcome(victim.future, kind, resumed);
    settle(rig.scheduler);
    QVERIFY(!resumed);

    victim.abort();
    settle(rig.scheduler);

    QVERIFY(resumed);
    QCOMPARE(kind, RateLimit::FetchError::Kind::Canceled);
}

void RateLimitManagerTest::siblingResumesCanceledWhenSharedSourceStops()
{
    // M-ER1, the manager-level guarantee the phase-5 worker leans on: a whole
    // update runs several fetches off one shared std::stop_source, and a
    // first-failure handler requests stop the instant any one of them fails.
    // A sibling suspended awaiting its own future at that moment must resume
    // from a *finished* Canceled value — never a QFuture cancellation, which
    // would make QCoro call result() on a resultless future (the ER1 UB; see
    // awaitingCoroutineResumesSafelyOnCancel for the single-fetch shape).
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    // Both fetches share one source, the way an update's requests will.
    std::stop_source shared;

    // The first fetch is awaited by a plain Caller so its terminal failure is
    // observable and can drive the shared stop.
    Caller first;
    first.attach(rig.manager.QueueRequest(kEndpoint, request("one"), shared.get_token()));

    // The second shares the source and suspends inside a coroutine, held back
    // by the gate's send spacing behind the first.
    auto second = rig.manager.QueueRequest(kEndpoint, request("two"), shared.get_token());
    bool resumed = false;
    std::optional<RateLimit::FetchError::Kind> kind;
    auto waiter = awaitOutcome(second, kind, resumed);

    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1); // only the first sent; the second is spacing-gated
    QVERIFY(!resumed);

    // The first fetch fails terminally — a real network failure, not a cancel.
    rig.sender.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();
    QCOMPARE(first.completions, 1);
    QCOMPARE(first.kind(), RateLimit::FetchError::Kind::Network);
    QVERIFY(!resumed); // the sibling is still suspended, waiting to send

    // The worker-style first-failure handler stops the shared source. No clock
    // advance: the gate spacing has not elapsed, so the sibling could not send
    // on its own — the only thing that moves it is the stop.
    shared.request_stop();
    settle(rig.scheduler);

    QVERIFY(resumed);
    QCOMPARE(kind, RateLimit::FetchError::Kind::Canceled);
    QCOMPARE(rig.sender.count(), 1); // the sibling never sent
}

namespace {

    // Captures the error-level lines the default logger writes while in
    // scope, then restores the previous default logger.
    class ErrorLogCapture
    {
    public:
        ErrorLogCapture()
            : m_previous(spdlog::default_logger())
        {
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_stream);
            sink->set_pattern("%v");
            auto logger = std::make_shared<spdlog::logger>("ratelimit-date-capture", sink);
            logger->set_level(spdlog::level::err);
            spdlog::set_default_logger(logger);
        }
        ~ErrorLogCapture() { spdlog::set_default_logger(m_previous); }
        ErrorLogCapture(const ErrorLogCapture &) = delete;
        ErrorLogCapture &operator=(const ErrorLogCapture &) = delete;

        QStringList lines() const
        {
            return QString::fromStdString(m_stream.str()).split('\n', Qt::SkipEmptyParts);
        }

    private:
        std::ostringstream m_stream;
        std::shared_ptr<spdlog::logger> m_previous;
    };

    QStringList dateLines(const QStringList &lines)
    {
        QStringList matched;
        for (const QString &line : lines) {
            if (line.contains("date", Qt::CaseInsensitive)) {
                matched.append(line);
            }
        }
        return matched;
    }

} // namespace

// #281: a transport failure lands with no headers at all. Its missing Date
// is not an error of its own: no error-level line about the date is logged
// on top of the real failure.
void RateLimitManagerTest::headerlessReplyLogsNoDateError()
{
    Rig rig;
    installPolicy(rig.manager, "10:60:60", "0:60:0");

    Caller caller;
    caller.attach(rig.manager.QueueRequest(kEndpoint, request("one"), caller.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.sender.count(), 1);

    QStringList errors;
    {
        ErrorLogCapture capture;
        rig.sender.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
        drainEvents();
        errors = capture.lines();
    }
    QCOMPARE(caller.completions, 1);
    QCOMPARE(caller.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(dateLines(errors), QStringList());
    // Only the real failure is reported.
    QCOMPARE(errors.size(), 1);
}

// A Date that is present but unparseable is still reported at error level,
// and parses to an invalid time, as before.
void RateLimitManagerTest::unparseableDateIsStillAnError()
{
    FakeNetworkReply reply(request("date"),
                           "",
                           QNetworkReply::NoError,
                           nullptr,
                           {{"Date", "not a date"}},
                           200);
    QDateTime date;
    QStringList errors;
    {
        ErrorLogCapture capture;
        date = RateLimit::ParseDate(&reply);
        errors = capture.lines();
    }
    QVERIFY(!date.isValid());
    QCOMPARE(dateLines(errors).size(), 1);

    FakeNetworkReply good(request("date"),
                          "",
                          QNetworkReply::NoError,
                          nullptr,
                          {{"Date", "Wed, 11 Nov 2026 12:00:00 GMT"}},
                          200);
    QCOMPARE(RateLimit::ParseDate(&good),
             QDateTime(QDate(2026, 11, 11), QTime(12, 0), QTimeZone::UTC).toLocalTime());
}

QTEST_GUILESS_MAIN(RateLimitManagerTest)

#include "tst_ratelimitmanager.moc"
