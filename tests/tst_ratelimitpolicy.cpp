// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QNetworkReply>
#include <QNetworkRequest>

#include "fakenetwork.h"
#include "ratelimit/ratelimit.h"
#include "ratelimit/ratelimitpolicy.h"
#include "testsuite.h"

// The total-parse suite (network-redesign spec, D8 and "Testing plan"
// item 3): RateLimitPolicy::Parse either yields a policy whose every
// triplet is three in-range integers, or a description of the first
// grammar failure. The malformed vectors below include the previous
// parser's UB inputs (missing per-rule headers parsed to a [""] list and
// read out of bounds) — they are ordinary test vectors now.

// moc-lexer note (see tst_workerupdate.cpp): declare the Q_OBJECT class
// before any helpers containing string literals with '//' in them.
class RateLimitPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesSingleRulePolicy();
    void parsesMultiRuleMultiItemPolicy();
    void statusReflectsWorstItem_data();
    void statusReflectsWorstItem();
    void malformedHeadersFailToParse_data();
    void malformedHeadersFailToParse();
};

namespace {

    FakeNetworkReply headerReply(const QList<QNetworkReply::RawHeaderPair> &headers)
    {
        return FakeNetworkReply(QNetworkRequest(QUrl("https://api.example.test/x")),
                                "",
                                QNetworkReply::NoError,
                                nullptr,
                                headers,
                                200);
    }

} // namespace

void RateLimitPolicyTest::parsesSingleRulePolicy()
{
    auto reply = headerReply({
        {"X-Rate-Limit-Policy", "test-policy"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:120"},
        {"X-Rate-Limit-Ip-State", "0:60:0"},
    });
    const auto policy = RateLimitPolicy::Parse(&reply);
    QVERIFY(policy.has_value());
    QCOMPARE(policy->name(), QString("test-policy"));
    QCOMPARE(policy->status(), RateLimit::Status::OK);
    QCOMPARE(policy->maximum_hits(), 30);
    QCOMPARE(policy->rules().size(), size_t(1));
    QCOMPARE(policy->rules()[0].name(), QString("Ip"));
    QCOMPARE(policy->rules()[0].items().size(), size_t(1));
    const auto &item = policy->rules()[0].items()[0];
    QCOMPARE(item.limit().hits(), 30);
    QCOMPARE(item.limit().period(), 60);
    QCOMPARE(item.limit().restriction(), 120);
    QCOMPARE(item.state().hits(), 0);
}

void RateLimitPolicyTest::parsesMultiRuleMultiItemPolicy()
{
    // The real API serves tiered rules like this (see the captured example
    // in tst_networkcapture.cpp).
    auto reply = headerReply({
        {"X-Rate-Limit-Policy", "backend-item-request-limit"},
        {"X-Rate-Limit-Rules", "Account,Ip"},
        {"X-Rate-Limit-Account", "6:4:10"},
        {"X-Rate-Limit-Account-State", "1:4:0"},
        {"X-Rate-Limit-Ip", "9:4:10,180:60:300"},
        {"X-Rate-Limit-Ip-State", "2:4:0,18:60:0"},
    });
    const auto policy = RateLimitPolicy::Parse(&reply);
    QVERIFY(policy.has_value());
    QCOMPARE(policy->rules().size(), size_t(2));
    QCOMPARE(policy->rules()[1].items().size(), size_t(2));
    QCOMPARE(policy->maximum_hits(), 180);
    QCOMPARE(policy->status(), RateLimit::Status::OK);
}

void RateLimitPolicyTest::statusReflectsWorstItem_data()
{
    QTest::addColumn<QByteArray>("state");
    QTest::addColumn<RateLimit::Status>("expected");

    QTest::newRow("ok") << QByteArray("29:60:0") << RateLimit::Status::OK;
    QTest::newRow("borderline") << QByteArray("30:60:0") << RateLimit::Status::BORDERLINE;
    QTest::newRow("violation") << QByteArray("31:60:60") << RateLimit::Status::VIOLATION;
}

void RateLimitPolicyTest::statusReflectsWorstItem()
{
    QFETCH(QByteArray, state);
    QFETCH(RateLimit::Status, expected);

    auto reply = headerReply({
        {"X-Rate-Limit-Policy", "test-policy"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:120"},
        {"X-Rate-Limit-Ip-State", state},
    });
    const auto policy = RateLimitPolicy::Parse(&reply);
    QVERIFY(policy.has_value());
    QCOMPARE(policy->status(), expected);
}

void RateLimitPolicyTest::malformedHeadersFailToParse_data()
{
    QTest::addColumn<QList<QNetworkReply::RawHeaderPair>>("headers");

    const QByteArray kLimit = "30:60:120";
    const QByteArray kState = "0:60:0";

    QTest::newRow("no-headers-at-all") << QList<QNetworkReply::RawHeaderPair>{};
    QTest::newRow("missing-policy-name") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("empty-policy-name") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", ""},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("missing-rules-list") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("empty-rules-list") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", ""},
    };
    QTest::newRow("empty-rule-name") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip,"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", kState},
    };
    // The previous parser's out-of-bounds input: a named rule with no
    // per-rule headers split to [""] and indexed past the end.
    QTest::newRow("missing-limit-header") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("missing-state-header") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", kLimit},
    };
    QTest::newRow("unequal-list-lengths") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:120,60:300:600"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("short-triplet") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("long-triplet") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:120:5"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("non-numeric-field") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:sixty:120"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    // A zero-hit limit is meaningless as a lookback and was a live
    // divide-by-zero in the now-deleted EstimateDuration (R7).
    QTest::newRow("zero-hit-limit") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "0:60:120"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("zero-period") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:0:120"},
        {"X-Rate-Limit-Ip-State", "0:0:0"},
    };
    QTest::newRow("negative-state-hits") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", "-1:60:0"},
    };
    QTest::newRow("negative-restriction") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:-1"},
        {"X-Rate-Limit-Ip-State", kState},
    };
    QTest::newRow("mismatched-periods") << QList<QNetworkReply::RawHeaderPair>{
        {"X-Rate-Limit-Policy", "p"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", kLimit},
        {"X-Rate-Limit-Ip-State", "0:30:0"},
    };
}

void RateLimitPolicyTest::malformedHeadersFailToParse()
{
    QFETCH(QList<QNetworkReply::RawHeaderPair>, headers);

    auto reply = headerReply(headers);
    const auto policy = RateLimitPolicy::Parse(&reply);
    QVERIFY(!policy.has_value());
    QVERIFY(!policy.error().isEmpty());
}

ACQ_TEST_GUILESS_MAIN(RateLimitPolicyTest)

#include "tst_ratelimitpolicy.moc"
