// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QCoroTask>

#include <stop_token>
#include <vector>

#include "fakescheduler.h"
#include "ratelimit/gate.h"
#include "testsuite.h"

// Standalone tests for the gate (network-redesign spec, D5 and "Testing
// plan" item 4): in-flight cap, HEAD exclusivity with writer preference,
// ordinary-waiter FIFO, the spacing floor, the permit span, and
// stop-interruptible waits. The injected FakeScheduler makes every timing
// assertion exact: the tests never sleep and no coarse-timer slack applies.

// moc-lexer note (see tst_workerupdate.cpp): declare the Q_OBJECT class
// before any helpers containing string literals with '//' in them.
class GateTest : public QObject
{
    Q_OBJECT

private slots:
    void capBoundsInFlightPermits();
    void spacingFloorIsExactAndResumeIsQueued();
    void spacingIsMeasuredFromDispatchNotGrant();
    void ordinaryWaitersGrantInFifoOrder();
    void contendingLanesAlternateWithoutStarvation();
    void headIsExclusiveWithWriterPreference();
    void headsSerializeFifoAmongThemselves();
    void stoppedWaiterYieldsNoPermitAndFreesTheQueue();
    void stoppedHeadReleasesTheWriterPreference();
    void preStoppedAcquireNeverEnqueues();
    void destroyedPermitReleasesItsSlot();
    void releaseWithoutDispatchChargesNoSpacing();
};

namespace {

    using namespace std::chrono_literals;

    constexpr auto SPACING = RateLimit::Gate::MIN_SEND_SPACING;

    struct Taker
    {
        bool done = false;
        RateLimit::Gate::Permit permit;
    };

    // Takers model the pump: acquire, then stamp the dispatch at the "send
    // site" (the moment after resume, same event-loop iteration). A stopped
    // wait yields an invalid permit, which MarkDispatched ignores.
    QCoro::Task<> Take(Taker &taker,
                       RateLimit::Gate &gate,
                       std::vector<int> *order = nullptr,
                       int id = -1,
                       std::stop_token token = {})
    {
        taker.permit = co_await gate.Acquire(std::move(token));
        taker.permit.MarkDispatched();
        if (order) {
            order->push_back(id);
        }
        taker.done = true;
    }

    QCoro::Task<> TakeHead(Taker &taker,
                           RateLimit::Gate &gate,
                           std::vector<int> *order = nullptr,
                           int id = -1,
                           std::stop_token token = {})
    {
        taker.permit = co_await gate.AcquireHead(std::move(token));
        taker.permit.MarkDispatched();
        if (order) {
            order->push_back(id);
        }
        taker.done = true;
    }

    // A pump-shaped lane: acquire, "send", release, repeat. Used to pin the
    // R7 alternation property (two contending pumps alternate; neither
    // starves).
    QCoro::Task<> Lane(std::vector<int> &order, int id, RateLimit::Gate &gate, int rounds)
    {
        for (int i = 0; i < rounds; ++i) {
            auto permit = co_await gate.Acquire(std::stop_token{});
            permit.MarkDispatched();
            order.push_back(id);
            permit.Release();
        }
    }

    // Deliver queued coroutine resumptions (the QFutureWatcher hop). A fixed
    // pass count keeps this deterministic — no wall-clock dependence.
    void drainEvents()
    {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
        }
    }

} // namespace

void GateTest::capBoundsInFlightPermits()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    Taker a;
    Taker b;
    Taker c;
    auto ta = Take(a, gate);
    auto tb = Take(b, gate);
    auto tc = Take(c, gate);

    // First grant is immediate; the second waits out the spacing floor.
    drainEvents();
    QVERIFY(a.done && a.permit.valid());
    QVERIFY(!b.done);

    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QVERIFY(b.done && b.permit.valid());

    // The third waiter is blocked by the cap, not by time: however far the
    // clock advances, no third permit exists while two are held.
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!c.done);

    // Releasing one slot grants it (spacing long since satisfied).
    a.permit.Release();
    drainEvents();
    QVERIFY(c.done && c.permit.valid());
}

void GateTest::spacingFloorIsExactAndResumeIsQueued()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    Taker a;
    auto ta = Take(a, gate);
    drainEvents();
    QVERIFY(a.done);
    a.permit.Release();

    // The slot is free; only the spacing floor holds B back now.
    Taker b;
    auto tb = Take(b, gate);
    drainEvents();
    QVERIFY(!b.done);

    scheduler.AdvanceBy(SPACING - 1ms);
    drainEvents();
    QVERIFY(!b.done);

    // The grant lands exactly at the floor — and the resume is queued: the
    // scheduler callback has settled the promise, but the waiter has not
    // run until the event loop delivers it.
    scheduler.AdvanceBy(1ms);
    QVERIFY(!b.done);
    drainEvents();
    QVERIFY(b.done && b.permit.valid());
}

void GateTest::spacingIsMeasuredFromDispatchNotGrant()
{
    // The P1 review pin: a grant settles a promise, but the winner
    // dispatches only after its queued resume, which a busy main thread
    // can delay. The floor must space actual sends, so the next grant is
    // decided 250 ms after the previous winner's RESUME (its dispatch
    // stamp), not 250 ms after its grant. Here the fake clock advances
    // 750 ms between A's grant and A's resume — the undelivered-resume
    // window a blocked event loop would produce.
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    // A blocker takes the immediate synchronous grant so A suspends.
    Taker blocker;
    auto tblocker = Take(blocker, gate);
    QVERIFY(blocker.done);
    blocker.permit.Release();

    Taker a;
    Taker b;
    auto ta = Take(a, gate);
    auto tb = Take(b, gate);
    drainEvents();
    QVERIFY(!a.done && !b.done);

    // A is granted at t=250 — but its resume is deliberately NOT
    // delivered yet (no drain): the grant is outstanding and unstamped.
    scheduler.AdvanceBy(SPACING);
    QVERIFY(!a.done);

    // Time passes far beyond grant-time spacing. B must not be granted:
    // there is no dispatch stamp to space it from, and granting it now
    // could produce two back-to-back sends once the loop unblocks.
    scheduler.AdvanceBy(3 * SPACING);
    QVERIFY(!b.done);

    // The loop unblocks: A resumes and stamps its dispatch at t=1000.
    drainEvents();
    QVERIFY(a.done && a.permit.valid());
    QVERIFY(!b.done);

    // B's grant lands exactly one spacing interval after A's DISPATCH
    // (t=1250), not after A's grant (t=250, long past).
    scheduler.AdvanceBy(SPACING - 1ms);
    drainEvents();
    QVERIFY(!b.done);
    scheduler.AdvanceBy(1ms);
    drainEvents();
    QVERIFY(b.done && b.permit.valid());
}

void GateTest::ordinaryWaitersGrantInFifoOrder()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);
    std::vector<int> order;

    Taker a;
    Taker b;
    Taker c;
    Taker d;
    Taker e;
    auto ta = Take(a, gate, &order, 1);
    auto tb = Take(b, gate, &order, 2);
    auto tc = Take(c, gate, &order, 3);
    auto td = Take(d, gate, &order, 4);
    auto te = Take(e, gate, &order, 5);

    drainEvents();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QCOMPARE(order, (std::vector<int>{1, 2}));

    // Whichever permit is released, the next grant goes to the queue head —
    // release order must not reorder waiters.
    b.permit.Release();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    a.permit.Release();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    c.permit.Release();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QCOMPARE(order, (std::vector<int>{1, 2, 3, 4, 5}));
}

void GateTest::contendingLanesAlternateWithoutStarvation()
{
    // R7's FIFO rationale, pinned end-to-end: two hot lanes that re-acquire
    // the moment they release must alternate — without arrival-order
    // grants, one lane could beat the other at every release (lane
    // starvation in miniature).
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);
    std::vector<int> order;

    // A blocker takes the immediate grant so both lanes are queued — in
    // order 1, 2 — before the first contended grant is decided.
    Taker blocker;
    auto tblocker = Take(blocker, gate);
    QVERIFY(blocker.done);

    auto lane1 = Lane(order, 1, gate, 3);
    auto lane2 = Lane(order, 2, gate, 3);
    blocker.permit.Release();

    for (int i = 0; i < 12 && order.size() < 6; ++i) {
        drainEvents();
        scheduler.AdvanceBy(SPACING);
        drainEvents();
    }
    QCOMPARE(order, (std::vector<int>{1, 2, 1, 2, 1, 2}));
}

void GateTest::headIsExclusiveWithWriterPreference()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);
    std::vector<int> order;

    // Fill the gate with ordinary traffic.
    Taker a;
    Taker b;
    auto ta = Take(a, gate, &order, 1);
    auto tb = Take(b, gate, &order, 2);
    drainEvents();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QVERIFY(a.done && b.done);

    // A HEAD arrives, then an ordinary waiter behind it.
    Taker head;
    Taker c;
    auto th = TakeHead(head, gate, &order, 100);
    auto tc = Take(c, gate, &order, 3);

    // Exclusivity: the HEAD waits while anything is in flight.
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!head.done);
    QVERIFY(!c.done);

    // Writer preference: freeing one slot grants nothing — not to the
    // ordinary waiter (a waiting HEAD blocks new ordinary permits), and
    // not to the HEAD (one permit is still out).
    a.permit.Release();
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!head.done);
    QVERIFY(!c.done);

    // The gate empties: the HEAD takes it exclusively.
    b.permit.Release();
    drainEvents();
    QVERIFY(head.done && head.permit.valid());
    QVERIFY(!c.done);

    // While the HEAD holds the gate, no ordinary permit exists.
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!c.done);

    head.permit.Release();
    drainEvents();
    QVERIFY(c.done && c.permit.valid());
    QCOMPARE(order, (std::vector<int>{1, 2, 100, 3}));
}

void GateTest::headsSerializeFifoAmongThemselves()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);
    std::vector<int> order;

    Taker h1;
    Taker h2;
    auto t1 = TakeHead(h1, gate, &order, 1);
    auto t2 = TakeHead(h2, gate, &order, 2);

    drainEvents();
    QVERIFY(h1.done && h1.permit.valid());
    QVERIFY(!h2.done);

    // Exclusive among themselves too, and the spacing floor applies to
    // HEAD grants like any other send.
    h1.permit.Release();
    drainEvents();
    QVERIFY(!h2.done);
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QVERIFY(h2.done && h2.permit.valid());
    QCOMPARE(order, (std::vector<int>{1, 2}));
}

void GateTest::stoppedWaiterYieldsNoPermitAndFreesTheQueue()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    Taker a;
    Taker b;
    auto ta = Take(a, gate);
    auto tb = Take(b, gate);
    drainEvents();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QVERIFY(a.done && b.done);

    std::stop_source stop;
    Taker c;
    Taker d;
    auto tc = Take(c, gate, nullptr, -1, stop.get_token());
    auto td = Take(d, gate);
    drainEvents();
    QVERIFY(!c.done && !d.done);

    // The queued resume contract (R4-2): request_stop() returns before the
    // stopped waiter resumes.
    stop.request_stop();
    QVERIFY(!c.done);
    drainEvents();
    QVERIFY(c.done);
    QVERIFY(!c.permit.valid());

    // The stopped waiter consumed nothing: the next release grants D.
    a.permit.Release();
    scheduler.AdvanceBy(SPACING);
    drainEvents();
    QVERIFY(d.done && d.permit.valid());
}

void GateTest::stoppedHeadReleasesTheWriterPreference()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    // One permit out: a HEAD waits for exclusivity, an ordinary waiter is
    // parked behind the writer preference.
    Taker a;
    auto ta = Take(a, gate);
    drainEvents();
    QVERIFY(a.done);

    std::stop_source stop;
    Taker head;
    Taker c;
    auto th = TakeHead(head, gate, nullptr, -1, stop.get_token());
    auto tc = Take(c, gate);
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!head.done && !c.done);

    // Stopping the HEAD lifts the preference. The pruning pass is deferred
    // through the scheduler, so it lands on the next advance.
    stop.request_stop();
    drainEvents();
    QVERIFY(head.done);
    QVERIFY(!head.permit.valid());
    scheduler.AdvanceBy(0ms);
    drainEvents();
    QVERIFY(c.done && c.permit.valid());
}

void GateTest::preStoppedAcquireNeverEnqueues()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    std::stop_source stop;
    stop.request_stop();

    // Completes synchronously with no permit, no queue entry, and nothing
    // scheduled (the D2 checkpoint shape).
    Taker a;
    auto ta = Take(a, gate, nullptr, -1, stop.get_token());
    QVERIFY(a.done);
    QVERIFY(!a.permit.valid());
    QCOMPARE(scheduler.PendingCount(), 0);

    // The gate is untouched: a later acquire grants normally.
    Taker b;
    auto tb = Take(b, gate);
    drainEvents();
    QVERIFY(b.done && b.permit.valid());
}

void GateTest::destroyedPermitReleasesItsSlot()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    // The permit span rule (IR6/R4-1) makes the pump release at
    // reply-finish; the destructor is the backstop that makes leaking a
    // slot unconstructible.
    Taker b;
    {
        Taker a1;
        Taker a2;
        auto t1 = Take(a1, gate);
        auto t2 = Take(a2, gate);
        drainEvents();
        scheduler.AdvanceBy(SPACING);
        drainEvents();
        QVERIFY(a1.done && a2.done);

        auto tb = Take(b, gate);
        scheduler.AdvanceBy(10 * SPACING);
        drainEvents();
        QVERIFY(!b.done);
        // a1 and a2 (and their permits) are destroyed here; tb's handle
        // dies too, detaching the frame (S1-1), which still resumes when
        // its grant is delivered.
    }
    drainEvents();
    QVERIFY(b.done && b.permit.valid());
}

void GateTest::releaseWithoutDispatchChargesNoSpacing()
{
    FakeScheduler scheduler;
    RateLimit::Gate gate(scheduler);

    // A dispatches normally at t=0: the floor now runs from t=0.
    Taker a;
    auto ta = Take(a, gate);
    drainEvents();
    QVERIFY(a.done && a.permit.valid());
    a.permit.Release();

    // B acquires but releases without ever dispatching (the phase-4
    // stopped-after-acquire path). While B is granted-but-unstamped, no
    // further grant is decided — then its no-send release un-defers grants
    // without charging a spacing interval.
    scheduler.AdvanceBy(SPACING);
    Taker b;
    auto tb = [](Taker &taker, RateLimit::Gate &g) -> QCoro::Task<> {
        taker.permit = co_await g.Acquire(std::stop_token{});
        taker.done = true;
    }(b, gate);
    drainEvents();
    QVERIFY(b.done && b.permit.valid());

    Taker c;
    auto tc = Take(c, gate);
    scheduler.AdvanceBy(10 * SPACING);
    drainEvents();
    QVERIFY(!c.done); // deferred behind B's outstanding, unstamped grant

    // C is granted the moment B releases: the only spacing charged is from
    // A's dispatch at t=0, long past. If B's no-send release charged an
    // interval, C would wait another SPACING from here.
    b.permit.Release();
    drainEvents();
    QVERIFY(c.done && c.permit.valid());
}

ACQ_TEST_GUILESS_MAIN(GateTest)

#include "tst_gate.moc"
