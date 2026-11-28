// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QLabel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTreeWidget>

#include "fakenetwork.h"
#include "fakenetworkmanager.h"
#include "fakescheduler.h"
#include "ratelimit/ratelimitdialog.h"
#include "ratelimit/ratelimiter.h"
#include "ratelimit/ratelimitpolicy.h"
#include "testsuite.h"

// Pins for the M1-M2/D10 queue-update coalesce (D10 in
// docs/design/items-pipeline-m2.md):
// the batch submit emits QueueUpdate once per queued request in a single loop
// turn, so the dialog parks the latest value per policy and applies it on a
// short single-shot flush timer instead of touching the tree per emission.
// The slot is a public slot, so the scenarios drive it directly — the wiring
// from the hub's signal is production code exercised by the M1-M2 harness.

// moc-lexer note (see tst_workerupdate.cpp): declare the Q_OBJECT class
// before any helpers containing string literals with '//' in them.
class RateLimitDialogTest : public QObject
{
    Q_OBJECT

private slots:
    void burstCoalescesToOneApply();
    void flushAppliesLatestValuePerPolicy();
    void rowRebuildBetweenArrivalAndFlushStillLands();
};

namespace {

    RateLimitPolicy parsedPolicy(const QByteArray &name)
    {
        FakeNetworkReply reply(QNetworkRequest(QUrl("https://api.example.test/x")),
                               "",
                               QNetworkReply::NoError,
                               nullptr,
                               {
                                   {"X-Rate-Limit-Policy", name},
                                   {"X-Rate-Limit-Rules", "Ip"},
                                   {"X-Rate-Limit-Ip", "30:60:120"},
                                   {"X-Rate-Limit-Ip-State", "0:60:0"},
                               },
                               200);
        const auto policy = RateLimitPolicy::Parse(&reply);
        if (!policy) {
            qFatal("the synthetic policy headers failed to parse");
        }
        return *policy;
    }

    // The dialog only needs a limiter to connect to; the offline hub rig
    // from tst_ratelimiter serves. The network outlives the limiter
    // (declared first).
    struct Rig
    {
        FakeScheduler scheduler;
        FakeNetworkManager network;
        RateLimiter limiter{network, &scheduler};
        RateLimitDialog dialog{nullptr, &limiter};

        QTreeWidget *tree() { return dialog.findChild<QTreeWidget *>(); }

        QString queueCell(const QString &policy_name)
        {
            QTreeWidget *t = tree();
            for (int i = 0; i < t->topLevelItemCount(); ++i) {
                if (t->topLevelItem(i)->text(0) == policy_name) {
                    return t->topLevelItem(i)->text(1);
                }
            }
            return QString("<no row>");
        }
    };

} // namespace

void RateLimitDialogTest::burstCoalescesToOneApply()
{
    Rig rig;
    rig.dialog.OnPolicyUpdate(parsedPolicy("policy-a"));
    QCOMPARE(rig.queueCell("policy-a"), QString(""));

    int data_changes = 0;
    connect(rig.tree()->model(),
            &QAbstractItemModel::dataChanged,
            this,
            [&data_changes](const QModelIndex &, const QModelIndex &, const QList<int> &) {
                ++data_changes;
            });

    // The burst shape: one emission per queued request, one loop turn.
    for (int i = 1; i <= 2000; ++i) {
        rig.dialog.OnQueueUpdate("policy-a", i);
    }

    // Nothing lands synchronously — the burst's loop turn never touches the
    // tree — and the whole burst is one apply at the flush.
    QCOMPARE(data_changes, 0);
    QCOMPARE(rig.queueCell("policy-a"), QString(""));
    QTRY_COMPARE(rig.queueCell("policy-a"), QString("2000"));
    QCOMPARE(data_changes, 1);
}

void RateLimitDialogTest::flushAppliesLatestValuePerPolicy()
{
    Rig rig;
    rig.dialog.OnPolicyUpdate(parsedPolicy("policy-a"));
    rig.dialog.OnPolicyUpdate(parsedPolicy("policy-b"));

    // Interleaved arrivals inside one window: each policy shows its latest
    // value, and a latest value of zero clears the cell.
    rig.dialog.OnQueueUpdate("policy-a", 3);
    rig.dialog.OnQueueUpdate("policy-b", 8);
    rig.dialog.OnQueueUpdate("policy-a", 5);
    rig.dialog.OnQueueUpdate("policy-b", 0);

    QTRY_COMPARE(rig.queueCell("policy-a"), QString("5"));
    QCOMPARE(rig.queueCell("policy-b"), QString(""));
}

void RateLimitDialogTest::rowRebuildBetweenArrivalAndFlushStillLands()
{
    Rig rig;
    rig.dialog.OnPolicyUpdate(parsedPolicy("policy-a"));

    // A policy update replaces the row object wholesale between arrival and
    // flush; the pending value must land on the replacement row (rows are
    // looked up by name at flush time, never captured at arrival).
    rig.dialog.OnQueueUpdate("policy-a", 7);
    rig.dialog.OnPolicyUpdate(parsedPolicy("policy-a"));

    QTRY_COMPARE(rig.queueCell("policy-a"), QString("7"));
}

ACQ_TEST_MAIN(RateLimitDialogTest)

#include "tst_ratelimitdialog.moc"
