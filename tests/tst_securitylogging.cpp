// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopedPointer>
#include <QStandardPaths>
#include <QTcpServer>

#include <sstream>

#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "util/json_readers.h"
#include "util/networkmanager.h"
#include "util/oauthtoken.h"

namespace {

    class TestNetworkManager : public NetworkManager
    {
    public:
        QNetworkReply *get(const QNetworkRequest &request)
        {
            return createRequest(QNetworkAccessManager::GetOperation, request);
        }
    };

} // namespace

class SecurityLoggingTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void authorizationHeaderIsMaskedCaseInsensitively();
    void unrelatedHeaderIsLoggedUnchanged();
    void attachedBearerTokenIsMasked();
    void malformedOAuthTokenDoesNotEchoInput();
    void apiRequestWithoutBearerLogsNoError();

private:
    QString logged() const;

    std::ostringstream m_output;
    std::shared_ptr<spdlog::logger> m_previous_logger;
};

void SecurityLoggingTest::init()
{
    m_output.str({});
    m_output.clear();
    m_previous_logger = spdlog::default_logger();

    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(m_output);
    auto logger = std::make_shared<spdlog::logger>("security-logging-test", sink);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%v");
    spdlog::set_default_logger(std::move(logger));
}

void SecurityLoggingTest::cleanup()
{
    spdlog::set_default_logger(std::move(m_previous_logger));
}

QString SecurityLoggingTest::logged() const
{
    spdlog::default_logger()->flush();
    return QString::fromStdString(m_output.str());
}

void SecurityLoggingTest::authorizationHeaderIsMaskedCaseInsensitively()
{
    constexpr auto secret = "Bearer authorization-secret";
    QNetworkRequest request{QUrl{"https://api.pathofexile.com/stash/Standard"}};
    request.setRawHeader("aUtHoRiZaTiOn", secret);

    NetworkManager::logRequest(request);

    const QString output = logged();
    QVERIFY(!output.contains(secret));
    QVERIFY(output.contains("masked for security"));
}

void SecurityLoggingTest::unrelatedHeaderIsLoggedUnchanged()
{
    constexpr auto value = "ordinary-header-value";
    QNetworkRequest request{QUrl{"https://api.pathofexile.com/stash/Standard"}};
    request.setRawHeader("X-Acquisition-Test", value);

    NetworkManager::logRequest(request);

    const QString output = logged();
    QVERIFY(output.contains(value));
    QVERIFY(!output.contains("masked for security"));
}

void SecurityLoggingTest::attachedBearerTokenIsMasked()
{
    constexpr auto secret = "attached-bearer-secret-never-log";
    QStandardPaths::setTestModeEnabled(true);
    TestNetworkManager manager;
    manager.setBearerToken(secret);

    QScopedPointer<QNetworkReply> reply{
        manager.get(QNetworkRequest{QUrl{"https://api.pathofexile.com/stash/Standard"}})};
    reply->abort();

    const QString output = logged();
    QVERIFY(!output.contains(secret));
    QVERIFY(output.contains("masked for security"));
}

void SecurityLoggingTest::malformedOAuthTokenDoesNotEchoInput()
{
    constexpr auto secret = "oauth-token-secret-never-log";
    const QByteArray malformed = QByteArrayLiteral("{\"access_token\":\"") + secret
                                 + QByteArrayLiteral("\" \"token_type\":\"Bearer\"}");

    const auto token = json::readOAuthToken(malformed);

    QVERIFY(!token);
    const QString output = logged();
    QVERIFY(!output.contains(secret));
    QVERIFY(output.contains("at byte"));
}

void SecurityLoggingTest::apiRequestWithoutBearerLogsNoError()
{
    // The login dialog asks api.pathofexile.com for the league list before
    // the startup token refresh has finished, and that endpoint needs no
    // bearer. A missing bearer is not an error at request time; a call that
    // needs one fails with a 401 that the caller reports.
    QStandardPaths::setTestModeEnabled(true);
    // Route the request to a loopback proxy so no outside host is contacted.
    QTcpServer proxy;
    QVERIFY(proxy.listen(QHostAddress::LocalHost));
    TestNetworkManager manager;
    manager.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", proxy.serverPort()));

    QScopedPointer<QNetworkReply> reply{manager.get(
        QNetworkRequest{QUrl{"https://api.pathofexile.com/leagues?type=main&compact=1"}})};
    reply->abort();

    const QString output = logged();
    QVERIFY2(!output.contains("bearer token is empty"), qPrintable(output));
    QVERIFY(!reply->request().hasRawHeader("Authorization"));
}

QTEST_GUILESS_MAIN(SecurityLoggingTest)

#include "tst_securitylogging.moc"
