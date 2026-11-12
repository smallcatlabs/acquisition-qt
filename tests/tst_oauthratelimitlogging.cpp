// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QNetworkReply>
#include <QNetworkRequest>

#include <sstream>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "fakenetwork.h"
#include "util/networkmanager.h"

// Observation-only logging for the OAuth token endpoint (ground truth N33):
// the endpoint carries its own IP-scoped `token-request-limit` policy but
// stays outside the rate-limiter hub (network-redesign D5 scope rationale).
// These tests drive NetworkManager::logOAuthTokenRateLimits with fake
// replies shaped like the August 16, 2026 observations.
class OAuthRateLimitLoggingTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void quietWhenUnderTheLimit();
    void warnsWhenTheLimitIsMaxed();
    void errorsOn429WithRetryAfter();
    void errorsOn429WithoutRetryAfter();
    void quietOnHeaderlessReply();

private:
    QString logged() const;

    std::ostringstream m_output;
    std::shared_ptr<spdlog::logger> m_previous_logger;
};

namespace {

    FakeNetworkReply tokenReply(const QList<QNetworkReply::RawHeaderPair> &headers, int http_status)
    {
        return FakeNetworkReply(QNetworkRequest(QUrl("https://www.pathofexile.com/oauth/token")),
                                "",
                                QNetworkReply::NoError,
                                nullptr,
                                headers,
                                http_status);
    }

} // namespace

void OAuthRateLimitLoggingTest::init()
{
    m_output.str({});
    m_output.clear();
    m_previous_logger = spdlog::default_logger();

    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output);
    auto logger = std::make_shared<spdlog::logger>("oauth-ratelimit-logging-test", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%l %v");
    spdlog::set_default_logger(std::move(logger));
}

void OAuthRateLimitLoggingTest::cleanup()
{
    spdlog::set_default_logger(std::move(m_previous_logger));
}

QString OAuthRateLimitLoggingTest::logged() const
{
    spdlog::default_logger()->flush();
    return QString::fromStdString(m_output.str());
}

void OAuthRateLimitLoggingTest::quietWhenUnderTheLimit()
{
    auto reply = tokenReply({{"X-Rate-Limit-Policy", "token-request-limit"},
                             {"X-Rate-Limit-Rules", "Ip"},
                             {"X-Rate-Limit-Ip", "60:30:30"},
                             {"X-Rate-Limit-Ip-State", "1:30:0"}},
                            200);
    NetworkManager::logOAuthTokenRateLimits(&reply);

    const QString output = logged();
    QVERIFY(!output.contains("warning"));
    QVERIFY(!output.contains("error"));
}

void OAuthRateLimitLoggingTest::warnsWhenTheLimitIsMaxed()
{
    auto reply = tokenReply({{"X-Rate-Limit-Policy", "token-request-limit"},
                             {"X-Rate-Limit-Rules", "Ip"},
                             {"X-Rate-Limit-Ip", "60:30:30"},
                             {"X-Rate-Limit-Ip-State", "60:30:0"}},
                            200);
    NetworkManager::logOAuthTokenRateLimits(&reply);

    const QString output = logged();
    QVERIFY(output.contains("warning"));
    QVERIFY(output.contains("token-request-limit"));
    QVERIFY(output.contains("BORDERLINE"));
}

void OAuthRateLimitLoggingTest::errorsOn429WithRetryAfter()
{
    auto reply = tokenReply({{"X-Rate-Limit-Policy", "token-request-limit"},
                             {"X-Rate-Limit-Rules", "Ip"},
                             {"X-Rate-Limit-Ip", "60:30:30"},
                             {"X-Rate-Limit-Ip-State", "61:30:30"},
                             {"Retry-After", "30"}},
                            429);
    NetworkManager::logOAuthTokenRateLimits(&reply);

    const QString output = logged();
    QVERIFY(output.contains("error"));
    QVERIFY(output.contains("429"));
    QVERIFY(output.contains("token-request-limit"));
    QVERIFY(output.contains("Retry-After 30s"));
}

void OAuthRateLimitLoggingTest::errorsOn429WithoutRetryAfter()
{
    auto reply = tokenReply({{"X-Rate-Limit-Policy", "token-request-limit"},
                             {"X-Rate-Limit-Rules", "Ip"},
                             {"X-Rate-Limit-Ip", "60:30:30"},
                             {"X-Rate-Limit-Ip-State", "61:30:30"}},
                            429);
    NetworkManager::logOAuthTokenRateLimits(&reply);

    const QString output = logged();
    QVERIFY(output.contains("error"));
    QVERIFY(output.contains("429"));
    QVERIFY(output.contains("absent or invalid"));
}

void OAuthRateLimitLoggingTest::quietOnHeaderlessReply()
{
    // A Cloudflare-mitigated reply carries no rate-limit headers (N28).
    auto reply = tokenReply({}, 403);
    NetworkManager::logOAuthTokenRateLimits(&reply);

    const QString output = logged();
    QVERIFY(!output.contains("warning"));
    QVERIFY(!output.contains("error"));
    QVERIFY(output.contains("no parseable rate-limit policy"));
}

QTEST_GUILESS_MAIN(OAuthRateLimitLoggingTest)

#include "tst_oauthratelimitlogging.moc"
