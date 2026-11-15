// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QCoroTask>

#include <optional>
#include <stop_token>

#include "ratelimit/scheduler.h"
#include "ratelimit/stopsleep.h"
#include "testsuite.h"

// Smoke tests for TimerScheduler, the production Scheduler adapter. This is
// real-timer code, so these tests live apart from the primitive suites
// (tst_stopsleep, tst_gate), which run purely on the injected fake clock and
// never sleep. Everything here is condition-driven — no timing bounds (the
// ±6% coarse-timer lesson from tst_ratelimitmanager.cpp).

class TimerSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void destroyedContextDropsCallback();
    void liveContextFires();
    void nullContextFires();
    void sleepComposesWithProductionScheduler();
};

namespace {

    using namespace std::chrono_literals;

    struct Probe
    {
        bool done = false;
        std::optional<RateLimit::SleepResult> result;
    };

    QCoro::Task<> Run(Probe &probe,
                      RateLimit::Scheduler &scheduler,
                      std::chrono::milliseconds deadline,
                      std::stop_token token)
    {
        probe.result = co_await RateLimit::SleepUntil(scheduler, deadline, token);
        probe.done = true;
    }

} // namespace

void TimerSchedulerTest::destroyedContextDropsCallback()
{
    RateLimit::TimerScheduler scheduler;
    bool fired = false;
    {
        QObject context;
        scheduler.CallAt(scheduler.Now() + 5ms, &context, [&] { fired = true; });
    }
    QTest::qWait(30);
    QVERIFY(!fired);
}

void TimerSchedulerTest::liveContextFires()
{
    RateLimit::TimerScheduler scheduler;
    bool fired = false;
    QObject context;
    scheduler.CallAt(scheduler.Now() + 5ms, &context, [&] { fired = true; });
    QTRY_VERIFY(fired);
}

void TimerSchedulerTest::nullContextFires()
{
    // A null context is the self-guarding-callback contract (the sleep
    // primitive's shared state).
    RateLimit::TimerScheduler scheduler;
    bool fired = false;
    scheduler.CallAt(scheduler.Now(), nullptr, [&] { fired = true; });
    QTRY_VERIFY(fired);
}

void TimerSchedulerTest::sleepComposesWithProductionScheduler()
{
    RateLimit::TimerScheduler scheduler;
    std::stop_source stop;
    Probe probe;
    auto task = Run(probe, scheduler, scheduler.Now() + 10ms, stop.get_token());
    QTRY_VERIFY(probe.done);
    QCOMPARE(*probe.result, RateLimit::SleepResult::Deadline);
}

ACQ_TEST_GUILESS_MAIN(TimerSchedulerTest)

#include "tst_timerscheduler.moc"
