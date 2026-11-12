// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QDateTime>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <chrono>

#include "fakenetworkmanager.h"
#include "fakescheduler.h"
#include "ratelimit/gate.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimiter.h"

// Hub-level setup pins (network-redesign spec, D4 and "Testing plan"
// item 3): Unknown -> Probing -> Established, the async HEAD behind the
// gate's exclusive permit, parked-entry forwarding, and every setup-failure
// flavor failing parked entries cleanly under the cooldown — where the old
// hub blocked in a nested event loop and killed the app with FatalError.
// The injected FakeScheduler drives all timing; nothing here sleeps.
//
// As of phase 4a the hub speaks QFuture<FetchOutcome>: setup failures carry
// typed FetchError kinds, and a parked entry whose token stops is pruned and
// completed Canceled while the probe carries on. Phase 4b deleted the legacy
// Submit() wrapper, so that is now the only boundary here — every pin asserts
// the FetchError kind directly instead of the Qt error code the wrapper used
// to synthesize.

// moc-lexer note (see tst_workerupdate.cpp): declare the Q_OBJECT class
// before any helpers containing string literals with '//' in them.
class RateLimiterTest : public QObject
{
    Q_OBJECT

private slots:
    void headEstablishesEndpointAndForwardsParkedFifo();
    void headTransportFailureFailsParkedAndCoolsDown();
    void resubmitInsideFailureHandlerCannotProvokeProbe();
    void headHttpErrorFailsParked();
    void headTruncated2xxIsNetworkFailure();
    void headUnparseable2xxFailsParked();
    void cooldownExpiryAllowsReprobe();
    void headFull429WithValidRetryAfterEstablishesWithHold();
    void sharedPolicyJoin429HoldsEntriesWaitingAtGate();
    void nonFull429CooldownHonorsRetryAfter();
    void establishedEndpointSticksThroughRequestFailures();
    void concurrentSetupsSerializeHeadsAtGate();
    void setupFailureKindsSurfaceAsFetchErrors_data();
    void setupFailureKindsSurfaceAsFetchErrors();
    void cooldownFailFastCompletesFutureImmediately();
    void canceledParkedEntryIsPrunedAndProbeContinues();
    void cancelingEveryParkedEntryStillEstablishes();
    void resubmitFromCancellationHandlerIsSafe();
    void cancellationWinsOverAConcurrentSetupFailure();

    // #284: the status countdown ends when the pump stops waiting, whether
    // its wait ran out or was canceled.
    void pausedReportsZeroWhenTheHoldEnds();
    void pausedReportsZeroWhenTheWaitIsCanceled();
    void subSecondPacingIsNotReportedAsAPause();
};

namespace {

    using namespace std::chrono_literals;

    constexpr auto kGateSpacing = RateLimit::Gate::MIN_SEND_SPACING;
    constexpr int kNormalBufferMsec = 100;

    QByteArray rfcDateNow()
    {
        return QDateTime::currentDateTimeUtc().toString(Qt::RFC2822Date).toUtf8();
    }

    QList<QNetworkReply::RawHeaderPair> policyHeaders(
        const QByteArray &limit,
        const QByteArray &state,
        const QByteArray &policy_name = "test-request-limit")
    {
        return {
            {"X-Rate-Limit-Policy", policy_name},
            {"X-Rate-Limit-Rules", "Ip"},
            {"X-Rate-Limit-Ip", limit},
            {"X-Rate-Limit-Ip-State", state},
            {"Date", rfcDateNow()},
        };
    }

    QNetworkRequest request(const QString &leaf)
    {
        return QNetworkRequest(QUrl("https://api.example.test/" + leaf));
    }

    void drainEvents()
    {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

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

    // Caller-side observations for one SubmitFuture(): the only boundary
    // the hub speaks (D1).
    struct FutureSubmission
    {
        std::stop_source source;
        QFuture<RateLimit::FetchOutcome> future;
        int completions = 0;
        bool succeeded = false;
        std::optional<RateLimit::FetchError> error;

        // Runs at the end of the recording continuation, on the same stack.
        // A QFuture takes only ONE continuation — a second .then() on the
        // same future replaces the first rather than chaining — so tests
        // that need to act on completion hook in here.
        std::function<void()> on_complete;

        std::stop_token token() { return source.get_token(); }
        void abort() { source.request_stop(); }

        void attach(QFuture<RateLimit::FetchOutcome> f)
        {
            future = f;
            future.then([this](const RateLimit::FetchOutcome &outcome) {
                ++completions;
                succeeded = outcome.has_value();
                error = outcome ? std::nullopt : std::optional(outcome.error());
                if (on_complete) {
                    on_complete();
                }
            });
        }

        std::optional<RateLimit::FetchError::Kind> kind() const
        {
            return error ? std::optional(error->kind) : std::nullopt;
        }
    };

    // The network outlives the limiter (declared first).
    struct Rig
    {
        FakeScheduler scheduler;
        FakeNetworkManager network;
        RateLimiter limiter{network, &scheduler};
    };

} // namespace

void RateLimiterTest::headEstablishesEndpointAndForwardsParkedFifo()
{
    Rig rig;

    // Two submissions before the endpoint is known: the first fires the
    // probe, the second parks behind it. Nothing blocks and no completion
    // is synchronous — though the HEAD itself may go out during Submit
    // when the gate's fast path grants without suspending (S1-6).
    FutureSubmission s1;
    FutureSubmission s2;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);
    QCOMPARE(rig.network.sent(0).op, QNetworkAccessManager::HeadOperation);
    QCOMPARE(s1.completions, 0);
    QCOMPARE(s2.completions, 0);

    // The probe lands Full: the endpoint establishes and the parked
    // entries forward in submission order into the pump.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    drainEvents();
    QCOMPARE(rig.network.count(), 1);

    // First send: pacing ends at t=100, but the HEAD's dispatch at t=0
    // charged the gate's spacing floor, so it goes at t=250.
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.network.count(), 1);
    advanceAndSettle(rig.scheduler, kGateSpacing - std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.network.count(), 2);
    QCOMPARE(rig.network.sent(1).op, QNetworkAccessManager::GetOperation);
    QCOMPARE(rig.network.sent(1).request.url(), request("one").url());

    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QVERIFY(s1.succeeded);

    // The second parked entry follows behind the spacing floor.
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 3);
    QCOMPARE(rig.network.sent(2).request.url(), request("two").url());
    rig.network.sent(2).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QCOMPARE(s2.completions, 1);
}

void RateLimiterTest::headTransportFailureFailsParkedAndCoolsDown()
{
    Rig rig;

    FutureSubmission s1;
    FutureSubmission s2;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    // The probe dies in transport: every parked entry completes with the
    // failure, classified, and carrying the Qt code for diagnostics.
    rig.network.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(s1.error->network_error, QNetworkReply::ConnectionRefusedError);
    QCOMPARE(s2.completions, 1);
    QCOMPARE(s2.kind(), RateLimit::FetchError::Kind::Network);

    // Inside the cooldown a resubmission fails fast with the prior
    // failure's shape, and no probe is sent. The future it returns is
    // already finished, so the completion lands inside attach — see
    // cooldownFailFastCompletesFutureImmediately for why that is safe.
    FutureSubmission s3;
    s3.attach(rig.limiter.SubmitFuture("ep", request("three"), s3.token()));
    QCOMPARE(s3.completions, 1);
    QCOMPARE(s3.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(rig.network.count(), 1);
}

void RateLimiterTest::resubmitInsideFailureHandlerCannotProvokeProbe()
{
    // The re-entrant shape of the D4 cooldown clause: a caller that
    // resubmits synchronously from its failure-completion handler must
    // hit the fail-fast branch — the cooldown is installed before any
    // completion is emitted, so the handler can never find the endpoint
    // Unknown and start another HEAD.
    Rig rig;

    FutureSubmission s1;
    FutureSubmission s2;
    bool resubmitted = false;
    // Resubmit from inside s1's own completion, on the same stack.
    s1.on_complete = [&] {
        s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
        resubmitted = true;
    };
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 1);

    rig.network.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();

    QVERIFY(resubmitted);
    QCOMPARE(s1.completions, 1);
    // No second probe went out, in the handler or afterwards...
    QCOMPARE(rig.network.headCount(), 1);
    // ...and the re-entrant submission fail-fasted. On the future boundary
    // it completes synchronously inside the attach, with no queued hop: the
    // future it returns is already finished.
    QCOMPARE(s2.completions, 1);
    QCOMPARE(s2.kind(), RateLimit::FetchError::Kind::Network);
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.network.headCount(), 1);
}

void RateLimiterTest::headHttpErrorFailsParked()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    // Qt reports HTTP failures as reply errors too (InternalServerError
    // for a 500); the status governs (D8 precedence): the failure is
    // classified as an HTTP setup failure — the message says so — with
    // the reply's own Qt error code kept for diagnostics.
    rig.network.sent(0).reply->finish({}, 500, QNetworkReply::InternalServerError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Http);
    QCOMPARE(s1.error->http_status, 500);
    // The reply's own Qt error is kept for diagnostics, but does not classify.
    QCOMPARE(s1.error->network_error, QNetworkReply::InternalServerError);
}

void RateLimiterTest::headTruncated2xxIsNetworkFailure()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    // A 2xx status alongside a transport error (Qt can report a status
    // and then fail mid-body) is a network failure, not a success and
    // not an HTTP failure — D8 precedence rule 3 — even with parseable
    // rate-limit headers on the reply.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"),
                                      200,
                                      QNetworkReply::RemoteHostClosedError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(s1.error->network_error, QNetworkReply::RemoteHostClosedError);

    // A setup failure, not an establishment: the resubmission fail-fasts
    // inside the cooldown with no new probe.
    FutureSubmission s2;
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    drainEvents();
    QCOMPARE(s2.completions, 1);
    QCOMPARE(rig.network.headCount(), 1);
}

void RateLimiterTest::headUnparseable2xxFailsParked()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    // A clean 200 with no usable rate-limit headers: the endpoint must
    // never run unpaced, so this is a setup failure (Protocol flavor), not
    // a degrade-and-proceed.
    rig.network.sent(0).reply->finish({{"Content-Type", "text/html"}}, 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Protocol);

    // And no pump exists for the endpoint: a resubmission inside the
    // cooldown fails fast without any network traffic.
    FutureSubmission s2;
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    drainEvents();
    QCOMPARE(s2.completions, 1);
    QCOMPARE(rig.network.count(), 1);
}

void RateLimiterTest::cooldownExpiryAllowsReprobe()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    rig.network.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(rig.network.headCount(), 1);

    // Once SETUP_RETRY_COOLDOWN passes, a submission probes again.
    advanceAndSettle(rig.scheduler, 60s);
    FutureSubmission s2;
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 2);

    // And this probe can succeed: the failure was contained, not sticky.
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    drainEvents();
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 3);
    rig.network.sent(2).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s2.completions, 1);
    QVERIFY(s2.succeeded);
}

void RateLimiterTest::headFull429WithValidRetryAfterEstablishesWithHold()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    // A Full HEAD 429 with a valid Retry-After still teaches the topology
    // (N16/N24): the pump establishes, and its first send is held past
    // Retry-After + pad + buffer = 120+60+1 = 181s. The OK state isolates
    // the hold from policy pacing. The HEAD consumed no send attempt —
    // the request completes on its first real send.
    auto headers = policyHeaders("10:60:60", "0:60:0");
    headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "120"});
    rig.network.sent(0).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();
    QCOMPARE(s1.completions, 0); // established, not failed

    advanceAndSettle(rig.scheduler, 100s);
    QCOMPARE(rig.network.count(), 1); // still holding
    advanceAndSettle(rig.scheduler, 85s);
    QCOMPARE(rig.network.count(), 2);
    QCOMPARE(rig.network.sent(1).op, QNetworkAccessManager::GetOperation);
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QVERIFY(s1.succeeded);
}

void RateLimiterTest::sharedPolicyJoin429HoldsEntriesWaitingAtGate()
{
    // The review-#2 join path (untested until now): a HEAD-429 probe that
    // resolves to an EXISTING pump's policy (same-named policies share
    // counters, N6) installs its hold on that pump — and an entry already
    // suspended at the gate behind that very HEAD must honor it instead
    // of dispatching the moment the probe releases.
    Rig rig;

    // Establish ep-one under the shared policy and complete one request.
    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep-one", request("one"), s1.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0", "shared-limit"), 200);
    drainEvents();
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 2);
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0", "shared-limit"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);

    // A second, unknown endpoint starts probing (its HEAD waits out the
    // spacing floor), and a new ep-one request paces and then parks at
    // the gate behind the writer preference.
    FutureSubmission s2;
    FutureSubmission s3;
    s2.attach(rig.limiter.SubmitFuture("ep-two", request("two"), s2.token()));
    s3.attach(rig.limiter.SubmitFuture("ep-one", request("three"), s3.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.network.count(), 2);
    advanceAndSettle(rig.scheduler, kGateSpacing - std::chrono::milliseconds(kNormalBufferMsec));
    QCOMPARE(rig.network.count(), 3);
    QCOMPARE(rig.network.sent(2).op, QNetworkAccessManager::HeadOperation);

    // The probe 429s with Full headers naming the SAME policy and a valid
    // Retry-After: ep-two joins the existing pump and the 120+60+1=181s
    // hold lands while the ep-one entry is still suspended at the gate.
    auto headers = policyHeaders("10:60:60", "0:60:0", "shared-limit");
    headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "120"});
    rig.network.sent(2).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
    drainEvents();
    QCOMPARE(s2.completions, 0); // joined, not failed

    // The entry is granted once the HEAD releases and spacing passes, but
    // it must honor the hold: no send at the grant, none deep into it.
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 3);
    advanceAndSettle(rig.scheduler, 100s);
    QCOMPARE(rig.network.count(), 3);

    // The hold passes: the parked ep-one entry sends first (FIFO), then
    // ep-two's own forwarded entry follows on the same pump.
    advanceAndSettle(rig.scheduler, 85s);
    QCOMPARE(rig.network.count(), 4);
    QCOMPARE(rig.network.sent(3).request.url(), request("three").url());
    rig.network.sent(3).reply->finish(policyHeaders("10:60:60", "2:60:0", "shared-limit"), 200);
    drainEvents();
    QCOMPARE(s3.completions, 1);

    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 5);
    QCOMPARE(rig.network.sent(4).request.url(), request("two").url());
    rig.network.sent(4).reply->finish(policyHeaders("10:60:60", "3:60:0", "shared-limit"), 200);
    drainEvents();
    QCOMPARE(s2.completions, 1);
}

void RateLimiterTest::nonFull429CooldownHonorsRetryAfter()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);

    // A 429 without Full headers is a setup failure whose cooldown honors
    // the validly-present Retry-After: 120s instead of the 60s floor.
    rig.network.sent(0).reply->finish({{"Retry-After", "120"}},
                                      429,
                                      QNetworkReply::UnknownContentError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::RateLimited);

    // 70s in: still cooling down, no probe.
    advanceAndSettle(rig.scheduler, 70s);
    FutureSubmission s2;
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    drainEvents();
    QCOMPARE(s2.completions, 1);
    QCOMPARE(rig.network.headCount(), 1);

    // 125s in: the window has passed; a new probe goes out.
    advanceAndSettle(rig.scheduler, 55s);
    FutureSubmission s3;
    s3.attach(rig.limiter.SubmitFuture("ep", request("three"), s3.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 2);
}

void RateLimiterTest::establishedEndpointSticksThroughRequestFailures()
{
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    drainEvents();
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 2);

    // The request itself dies (a timeout-shaped transport failure). The
    // endpoint stays Established: valid topology is never discarded and no
    // HEAD is re-burned (N16).
    rig.network.sent(1).reply->finish({}, 0, QNetworkReply::TimeoutError);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Network);
    QCOMPARE(s1.error->network_error, QNetworkReply::TimeoutError);

    FutureSubmission s2;
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.headCount(), 1); // no re-probe
    QCOMPARE(rig.network.count(), 3);
    QCOMPARE(rig.network.sent(2).op, QNetworkAccessManager::GetOperation);
    rig.network.sent(2).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s2.completions, 1);
}

void RateLimiterTest::concurrentSetupsSerializeHeadsAtGate()
{
    Rig rig;

    // Two unknown endpoints at once: their probes serialize at the gate's
    // exclusive HEAD permit — one HEAD in flight, ever (the F5 standing
    // constraint, now a property of the gate).
    FutureSubmission s1;
    FutureSubmission s2;
    s1.attach(rig.limiter.SubmitFuture("ep-one", request("one"), s1.token()));
    s2.attach(rig.limiter.SubmitFuture("ep-two", request("two"), s2.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);
    QCOMPARE(rig.network.sent(0).op, QNetworkAccessManager::HeadOperation);

    // While the second HEAD waits, writer preference also holds back the
    // first pump's sends — no ordinary permit is issued past a waiting
    // HEAD.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0", "policy-one"), 200);
    drainEvents();
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 2);
    QCOMPARE(rig.network.sent(1).op, QNetworkAccessManager::HeadOperation);

    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "0:60:0", "policy-two"), 200);
    drainEvents();

    // With both endpoints established, the parked GETs drain in arrival
    // order behind the spacing floor.
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 3);
    QCOMPARE(rig.network.sent(2).request.url(), request("one").url());
    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 4);
    QCOMPARE(rig.network.sent(3).request.url(), request("two").url());

    rig.network.sent(2).reply->finish(policyHeaders("10:60:60", "1:60:0", "policy-one"), 200);
    rig.network.sent(3).reply->finish(policyHeaders("10:60:60", "1:60:0", "policy-two"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s2.completions, 1);
}

void RateLimiterTest::setupFailureKindsSurfaceAsFetchErrors_data()
{
    // Every setup-failure flavor D4 distinguishes, now carrying a typed
    // kind rather than only an errored reply. The kind is what the worker
    // and the facade will branch on, so each flavor is pinned to the kind
    // the spec names for it.
    QTest::addColumn<QList<QNetworkReply::RawHeaderPair>>("headers");
    QTest::addColumn<int>("status");
    QTest::addColumn<QNetworkReply::NetworkError>("error");
    QTest::addColumn<RateLimit::FetchError::Kind>("kind");

    QTest::newRow("transport") << QList<QNetworkReply::RawHeaderPair>{} << 0
                               << QNetworkReply::ConnectionRefusedError
                               << RateLimit::FetchError::Kind::Network;
    QTest::newRow("http-500") << QList<QNetworkReply::RawHeaderPair>{} << 500
                              << QNetworkReply::InternalServerError
                              << RateLimit::FetchError::Kind::Http;
    QTest::newRow("unparseable-2xx")
        << QList<QNetworkReply::RawHeaderPair>{{"Content-Type", "text/html"}} << 200
        << QNetworkReply::NoError << RateLimit::FetchError::Kind::Protocol;
    QTest::newRow("429-without-usable-reply")
        << QList<QNetworkReply::RawHeaderPair>{} << 429 << QNetworkReply::UnknownContentError
        << RateLimit::FetchError::Kind::RateLimited;
}

void RateLimiterTest::setupFailureKindsSurfaceAsFetchErrors()
{
    QFETCH(QList<QNetworkReply::RawHeaderPair>, headers);
    QFETCH(int, status);
    QFETCH(QNetworkReply::NetworkError, error);
    QFETCH(RateLimit::FetchError::Kind, kind);

    Rig rig;

    FutureSubmission s1;
    FutureSubmission s2;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    s2.attach(rig.limiter.SubmitFuture("ep", request("two"), s2.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.count(), 1);

    rig.network.sent(0).reply->finish(headers, status, error);
    drainEvents();

    // Every parked entry fails with the same shape, and each error names
    // the endpoint it belongs to.
    QCOMPARE(s1.completions, 1);
    QCOMPARE(s1.kind(), kind);
    QCOMPARE(s1.error->endpoint, QString("ep"));
    QCOMPARE(s2.completions, 1);
    QCOMPARE(s2.kind(), kind);
}

void RateLimiterTest::cooldownFailFastCompletesFutureImmediately()
{
    // The fail-fast completion is synchronous on the future boundary, and
    // that is safe by construction: a continuation attached to an already
    // finished future still runs. The queued hop the signal boundary needs
    // exists only because a signal emitted before the caller connects is
    // lost — a value in shared state is not.
    Rig rig;

    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    rig.network.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();
    QCOMPARE(s1.kind(), RateLimit::FetchError::Kind::Network);

    // Inside the cooldown: the returned future is already finished, and
    // attaching afterwards still delivers.
    FutureSubmission s2;
    QFuture<RateLimit::FetchOutcome> future = rig.limiter.SubmitFuture("ep",
                                                                       request("two"),
                                                                       s2.token());
    QVERIFY(future.isFinished());
    s2.attach(future);
    QCOMPARE(s2.completions, 1);
    QCOMPARE(s2.kind(), RateLimit::FetchError::Kind::Network);
    // No probe was sent inside the window.
    QCOMPARE(rig.network.count(), 1);
}

void RateLimiterTest::canceledParkedEntryIsPrunedAndProbeContinues()
{
    // A parked entry whose token stops is pruned and completed Canceled.
    // The entries around it are untouched, and setup carries on: the probe
    // is already in flight and teaches the topology regardless.
    Rig rig;

    FutureSubmission victim;
    FutureSubmission survivor;
    victim.attach(rig.limiter.SubmitFuture("ep", request("one"), victim.token()));
    survivor.attach(rig.limiter.SubmitFuture("ep", request("two"), survivor.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 1);

    victim.abort();
    settle(rig.scheduler);
    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    QCOMPARE(survivor.completions, 0);

    // Setup completes normally and forwards only the survivor.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    // Settle BEFORE advancing: establishment has to run so the forwarded
    // entry's pacing deadline exists on the clock. Advancing first would
    // move past an instant nothing was scheduled at yet, and the deadline
    // would then be set a full second into the future.
    settle(rig.scheduler);
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.network.count(), 2);
    QCOMPARE(rig.network.sent(1).request.url(), request("two").url());

    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(survivor.completions, 1);
    QVERIFY(survivor.succeeded);
    // Cancellation is not a setup failure: the entry completed exactly once
    // and nothing else changed.
    QCOMPARE(victim.completions, 1);
}

void RateLimiterTest::cancelingEveryParkedEntryStillEstablishes()
{
    // The D4 clause stated directly: the probe proceeds even when every
    // parked entry is gone. Cancellation installs no cooldown, so the next
    // submission finds an established endpoint rather than a fail-fast
    // window — the opposite of what treating cancellation as a failure
    // would produce.
    Rig rig;

    FutureSubmission victim;
    victim.attach(rig.limiter.SubmitFuture("ep", request("one"), victim.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 1);

    victim.abort();
    settle(rig.scheduler);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);

    // The HEAD still lands and still establishes the endpoint.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    settle(rig.scheduler);

    FutureSubmission after;
    after.attach(rig.limiter.SubmitFuture("ep", request("two"), after.token()));
    advanceAndSettle(rig.scheduler, std::chrono::milliseconds(kNormalBufferMsec) + kGateSpacing);
    // Sent by the established pump — no second HEAD, no fail-fast.
    QCOMPARE(rig.network.headCount(), 1);
    QCOMPARE(rig.network.count(), 2);
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QVERIFY(after.succeeded);
}

void RateLimiterTest::resubmitFromCancellationHandlerIsSafe()
{
    // A default QFuture continuation runs SYNCHRONOUSLY on the completing
    // stack, so a caller can resubmit to the same probing endpoint from
    // inside its own cancellation handler. That reaches ParkEntry, which
    // push_backs onto the very deque the prune is walking — and
    // deque::push_back invalidates all iterators into it. The prune must
    // therefore stabilize the deque before completing, not after.
    //
    // HONEST SCOPE: this pins the observable behavior — one completion, one
    // probe, nothing lost, order preserved. It does NOT reproduce the
    // iterator-invalidation UB in a plain build: with a handful of parked
    // entries libc++'s deque does not reallocate, so the stale iterator
    // happens to still address the right element. Forcing a reallocation
    // would need hundreds of parked entries. The fix rests on the standard's
    // guarantee, not on this test failing without it.
    Rig rig;

    FutureSubmission victim;
    FutureSubmission survivor;
    victim.attach(rig.limiter.SubmitFuture("ep", request("one"), victim.token()));
    survivor.attach(rig.limiter.SubmitFuture("ep", request("two"), survivor.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 1);

    // Resubmit from the cancellation handler, the re-entrant shape.
    FutureSubmission resubmitted;
    bool resubmit_ran = false;
    victim.on_complete = [&] {
        resubmitted.attach(rig.limiter.SubmitFuture("ep", request("three"), resubmitted.token()));
        resubmit_ran = true;
    };

    victim.abort();
    settle(rig.scheduler);

    QVERIFY(resubmit_ran);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    // Still exactly one probe: the resubmission parked behind the in-flight
    // HEAD rather than starting another.
    QCOMPARE(rig.network.headCount(), 1);

    // Both survivors forward, in order, and the queue is intact.
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    settle(rig.scheduler);
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.network.count(), 2);
    QCOMPARE(rig.network.sent(1).request.url(), request("two").url());
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    settle(rig.scheduler);
    advanceAndSettle(rig.scheduler, 1s);
    QCOMPARE(rig.network.count(), 3);
    QCOMPARE(rig.network.sent(2).request.url(), request("three").url());
    rig.network.sent(2).reply->finish(policyHeaders("10:60:60", "2:60:0"), 200);
    drainEvents();
    QVERIFY(survivor.succeeded);
    QVERIFY(resubmitted.succeeded);
}

void RateLimiterTest::cancellationWinsOverAConcurrentSetupFailure()
{
    // Stopping a parked entry only SCHEDULES its prune. If the HEAD fails
    // before that callback runs, FailSetup would otherwise complete the
    // stopped entry with the setup failure's kind — while on the success
    // path the same entry always ends up Canceled. Cancellation is the
    // caller's own decision, so it must win either way; otherwise the
    // outcome depends on event ordering.
    Rig rig;

    FutureSubmission victim;
    FutureSubmission bystander;
    victim.attach(rig.limiter.SubmitFuture("ep", request("one"), victim.token()));
    bystander.attach(rig.limiter.SubmitFuture("ep", request("two"), bystander.token()));
    settle(rig.scheduler);
    QCOMPARE(rig.network.headCount(), 1);

    // Stop, then fail the HEAD, and only THEN advance the scheduler — so
    // the setup failure is processed while the prune is still pending.
    victim.abort();
    rig.network.sent(0).reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
    drainEvents();

    QCOMPARE(victim.completions, 1);
    QCOMPARE(victim.kind(), RateLimit::FetchError::Kind::Canceled);
    // The entry that never cancelled still gets the real failure.
    QCOMPARE(bystander.completions, 1);
    QCOMPARE(bystander.kind(), RateLimit::FetchError::Kind::Network);

    // The pending prune fires against an endpoint that is no longer
    // probing: it must find nothing and complete nothing a second time.
    settle(rig.scheduler);
    QCOMPARE(victim.completions, 1);
}

namespace {

    // A HEAD 429 with Full headers and Retry-After: 120 holds the pump's
    // first send for 120 + 60 + 1 = 181s (D4), which it announces as a
    // pause.
    void holdFirstSend(Rig &rig, FutureSubmission &submission)
    {
        submission.attach(rig.limiter.SubmitFuture("ep", request("one"), submission.token()));
        settle(rig.scheduler);
        auto headers = policyHeaders("10:60:60", "0:60:0");
        headers.append(QNetworkReply::RawHeaderPair{"Retry-After", "120"});
        rig.network.sent(0).reply->finish(headers, 429, QNetworkReply::UnknownContentError);
        drainEvents();
    }

    // One tick of the hub's once-a-second status timer.
    void tick(RateLimiter &limiter)
    {
        QVERIFY(QMetaObject::invokeMethod(&limiter, "SendStatusUpdate"));
    }

} // namespace

void RateLimiterTest::pausedReportsZeroWhenTheHoldEnds()
{
    Rig rig;
    QSignalSpy paused(&rig.limiter, &RateLimiter::Paused);
    FutureSubmission s1;
    holdFirstSend(rig, s1);

    tick(rig.limiter);
    QVERIFY(!paused.isEmpty());
    QCOMPARE(paused.last().at(0).toInt(), 181);
    QCOMPARE(paused.last().at(1).toString(), QString("test-request-limit"));

    // The hold runs out and the request is sent: nothing is paused any more,
    // which is reported once.
    advanceAndSettle(rig.scheduler, 185s);
    QCOMPARE(rig.network.count(), 2);
    tick(rig.limiter);
    QCOMPARE(paused.last().at(0).toInt(), 0);
    const auto reported = paused.count();
    tick(rig.limiter);
    QCOMPARE(paused.count(), reported);

    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
}

void RateLimiterTest::pausedReportsZeroWhenTheWaitIsCanceled()
{
    Rig rig;
    QSignalSpy paused(&rig.limiter, &RateLimiter::Paused);
    FutureSubmission s1;
    holdFirstSend(rig, s1);

    tick(rig.limiter);
    QVERIFY(!paused.isEmpty());
    QVERIFY(paused.last().at(0).toInt() > 0);

    // The caller gives up (an aborted update) while the pump waits: the
    // countdown must not keep running.
    s1.abort();
    settle(rig.scheduler);
    QCOMPARE(s1.kind(), std::optional(RateLimit::FetchError::Kind::Canceled));
    tick(rig.limiter);
    QCOMPARE(paused.last().at(0).toInt(), 0);
}

// Every send waits out a short pacing buffer; that is not rate limiting, and
// the status button is left alone.
void RateLimiterTest::subSecondPacingIsNotReportedAsAPause()
{
    Rig rig;
    QSignalSpy paused(&rig.limiter, &RateLimiter::Paused);
    FutureSubmission s1;
    s1.attach(rig.limiter.SubmitFuture("ep", request("one"), s1.token()));
    settle(rig.scheduler);
    rig.network.sent(0).reply->finish(policyHeaders("10:60:60", "0:60:0"), 200);
    drainEvents();

    // The request is in its pacing wait (100ms) when the status timer ticks.
    tick(rig.limiter);
    QCOMPARE(paused.count(), 0);

    advanceAndSettle(rig.scheduler, kGateSpacing);
    QCOMPARE(rig.network.count(), 2);
    rig.network.sent(1).reply->finish(policyHeaders("10:60:60", "1:60:0"), 200);
    drainEvents();
    QCOMPARE(s1.completions, 1);
    tick(rig.limiter);
    QCOMPARE(paused.count(), 0);
}

QTEST_GUILESS_MAIN(RateLimiterTest)

#include "tst_ratelimiter.moc"
