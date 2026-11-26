// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// OAuthManager's state machine as a table: each row is a sequence of events
// and the state of both regions after each one (oauthmanager.cpp has the
// diagram). The manager is driven alone, over a network whose replies the
// row completes by hand, in any order. tst_applogin runs the same cases
// through the whole application.

#include <QTest>

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "datastore/sqlitedatastore.h"
#include "fakenetworkmanager.h"
#include "oauthtestaccess.h"
#include "testsuite.h"
#include "util/oauthmanager.h"

// Takes the addresses the manager asks the browser to open.
class OAuthTestBrowser : public QObject
{
    Q_OBJECT
public slots:
    void open(const QUrl &) {}
};

class OAuthManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void transitions_data();
    void transitions();
    void classify_data();
    void classify();

private:
    OAuthTestBrowser m_browser;
};

void OAuthManagerTest::init()
{
    QDesktopServices::setUrlHandler("https", &m_browser, "open");
}

void OAuthManagerTest::cleanup()
{
    QDesktopServices::unsetUrlHandler("https");
}

namespace {

    QByteArray token(const QString &account, int expires_in)
    {
        return QJsonDocument(
                   QJsonObject{{"access_token", "access-" + account},
                               {"expires_in", expires_in},
                               {"token_type", "bearer"},
                               {"scope", "account:leagues account:stashes account:characters"},
                               {"username", account},
                               {"refresh_token", "refresh-" + account}})
            .toJson(QJsonDocument::Compact);
    }

} // namespace

// Steps:
//   authenticate      initLogin()
//   redirect          the redirect of the waiting login, with a code
//   redirect wrong    a redirect with another state
//   redirect again    the last redirect sent again
//   end               endLogin(), as Login does
//   timer             the refresh timer fires
//   grant N A [S]     request N (from 0) is answered with a token for A,
//                     which expires in S seconds (36000)
//   refuse N          request N is answered 400 invalid_grant
//   fail N            request N gets no answer (connection refused)
//   answer N S [R]    request N is answered with HTTP status S, a body that
//                     names an error, and Retry-After R if given
// After each step, and before the first, the state is "login / browser",
// with the account held, if any, after another " / ". At the end, `running`
// lists the requests still on their way: a request whose state was left is
// not among them, it was aborted. `due` is when the refresh timer fires, in
// seconds, or -1 when it is not armed.
void OAuthManagerTest::transitions_data()
{
    QTest::addColumn<bool>("stored");
    QTest::addColumn<QStringList>("steps");
    QTest::addColumn<QStringList>("states");
    QTest::addColumn<QStringList>("signals_");
    QTest::addColumn<int>("requests");
    QTest::addColumn<QList<int>>("running");
    QTest::addColumn<int>("due");

    // clang-format off
    QTest::newRow("stored login refreshed") << true
        << QStringList{"grant 0 A"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A"}
        << QStringList{"granted A"} << 1 << QList<int>{} << 35700;
    QTest::newRow("stored login refused") << true
        << QStringList{"refuse 0"}
        << QStringList{"RefreshingStored / NoBrowser", "NoLogin / NoBrowser"}
        << QStringList{"storedLoginFailed"} << 1 << QList<int>{} << -1;
    QTest::newRow("stored login unreachable") << true
        << QStringList{"fail 0"}
        << QStringList{"RefreshingStored / NoBrowser", "NoLogin / NoBrowser"}
        << QStringList{"storedLoginFailed"} << 1 << QList<int>{} << -1;
    QTest::newRow("browser login") << false
        << QStringList{"authenticate", "redirect", "grant 0 A"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / Exchanging",
                       "Holding / NoBrowser / A"}
        << QStringList{"granted A"} << 1 << QList<int>{} << 35700;
    QTest::newRow("redirect with another state") << false
        << QStringList{"authenticate", "redirect wrong"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / NoBrowser"}
        << QStringList{"loginFailed"} << 0 << QList<int>{} << -1;
    QTest::newRow("refused exchange keeps the login held") << true
        << QStringList{"grant 0 A", "authenticate", "redirect", "refuse 1"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding / Waiting / A", "Holding / Exchanging / A",
                       "Holding / NoBrowser / A"}
        << QStringList{"granted A", "loginFailed"} << 2 << QList<int>{} << 35700;
    QTest::newRow("#410 stored login refreshed while the browser is out") << true
        << QStringList{"authenticate", "grant 0 A", "redirect", "grant 1 B"}
        << QStringList{"RefreshingStored / NoBrowser", "RefreshingStored / Waiting",
                       "Holding / Waiting / A", "Holding / Exchanging / A",
                       "Holding / NoBrowser / B"}
        << QStringList{"granted A", "granted B"} << 2 << QList<int>{} << 35700;
    QTest::newRow("#410 refused refresh while the browser is out") << false
        << QStringList{"authenticate", "redirect", "grant 0 A", "authenticate", "timer",
                       "refuse 1"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / Exchanging",
                       "Holding / NoBrowser / A", "Holding / Waiting / A",
                       "Holding+refresh / Waiting / A", "Refused / Waiting / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << -1;
    QTest::newRow("#414 Login during the exchange") << true
        << QStringList{"grant 0 A", "authenticate", "redirect", "end"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding / Waiting / A", "Holding / Exchanging / A",
                       "Holding / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << 35700;
    QTest::newRow("Authenticate again during the exchange") << false
        << QStringList{"authenticate", "redirect", "authenticate", "redirect", "grant 1 A"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / Exchanging",
                       "NoLogin / Waiting", "NoLogin / Exchanging", "Holding / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << 35700;
    QTest::newRow("stale refresh at start") << true
        << QStringList{"authenticate", "redirect", "grant 1 B"}
        << QStringList{"RefreshingStored / NoBrowser", "RefreshingStored / Waiting",
                       "RefreshingStored / Exchanging", "Holding / NoBrowser / B"}
        << QStringList{"granted B"} << 2 << QList<int>{} << 35700;
    QTest::newRow("redirect during a refresh") << true
        << QStringList{"grant 0 A", "authenticate", "timer", "redirect", "grant 2 B"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding / Waiting / A", "Holding+refresh / Waiting / A",
                       "Holding+refresh / Exchanging / A", "Holding / NoBrowser / B"}
        << QStringList{"granted A", "granted B"} << 3 << QList<int>{} << 35700;
    QTest::newRow("refresh answered during the exchange") << true
        << QStringList{"grant 0 A", "authenticate", "redirect", "timer", "grant 2 A",
                       "grant 1 B"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding / Waiting / A", "Holding / Exchanging / A",
                       "Holding+refresh / Exchanging / A", "Holding / Exchanging / A",
                       "Holding / NoBrowser / B"}
        << QStringList{"granted A", "granted A", "granted B"} << 3 << QList<int>{} << 35700;
    QTest::newRow("redirect sent again during the exchange") << false
        << QStringList{"authenticate", "redirect", "redirect again"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / Exchanging",
                       "NoLogin / Exchanging"}
        << QStringList{} << 1 << QList<int>{0} << -1;
    QTest::newRow("one refresh at a time") << true
        << QStringList{"grant 0 A", "timer", "timer"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+refresh / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{1} << 35700;
    QTest::newRow("no refresh without a login held") << false
        << QStringList{"timer", "authenticate", "timer"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / NoBrowser", "NoLogin / Waiting",
                       "NoLogin / Waiting"}
        << QStringList{} << 0 << QList<int>{} << -1;
    QTest::newRow("a refresh that fails is tried again") << true
        << QStringList{"grant 0 A", "timer", "fail 1"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+retry1 / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << 15;
    QTest::newRow("tried again later each time, up to a cap") << true
        << QStringList{"grant 0 A", "timer", "fail 1", "timer", "fail 2", "timer", "fail 3",
                       "timer", "fail 4", "timer", "fail 5"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+retry1 / NoBrowser / A",
                       "Holding+retry1+refresh / NoBrowser / A",
                       "Holding+retry2 / NoBrowser / A",
                       "Holding+retry2+refresh / NoBrowser / A",
                       "Holding+retry3 / NoBrowser / A",
                       "Holding+retry3+refresh / NoBrowser / A",
                       "Holding+retry4 / NoBrowser / A",
                       "Holding+retry4+refresh / NoBrowser / A",
                       "Holding+retry5 / NoBrowser / A"}
        << QStringList{"granted A"} << 6 << QList<int>{} << 120;
    QTest::newRow("a retry that succeeds holds the new token") << true
        << QStringList{"grant 0 A", "timer", "fail 1", "timer", "grant 2 A"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+retry1 / NoBrowser / A",
                       "Holding+retry1+refresh / NoBrowser / A", "Holding / NoBrowser / A"}
        << QStringList{"granted A", "granted A"} << 3 << QList<int>{} << 35700;
    QTest::newRow("a refused refresh is not tried again") << true
        << QStringList{"grant 0 A", "timer", "refuse 1", "timer"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Refused / NoBrowser / A",
                       "Refused / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << -1;
    QTest::newRow("a browser login after a refusal holds its token") << true
        << QStringList{"grant 0 A", "timer", "refuse 1", "authenticate", "redirect", "grant 2 B"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Refused / NoBrowser / A",
                       "Refused / Waiting / A", "Refused / Exchanging / A",
                       "Holding / NoBrowser / B"}
        << QStringList{"granted A", "granted B"} << 3 << QList<int>{} << 35700;
    QTest::newRow("a refresh answered 500 is tried again") << true
        << QStringList{"grant 0 A", "timer", "answer 1 500"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+retry1 / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << 15;
    QTest::newRow("a refresh answered 429 waits as long as it is asked") << true
        << QStringList{"grant 0 A", "timer", "answer 1 429 90"}
        << QStringList{"RefreshingStored / NoBrowser", "Holding / NoBrowser / A",
                       "Holding+refresh / NoBrowser / A", "Holding+retry1 / NoBrowser / A"}
        << QStringList{"granted A"} << 2 << QList<int>{} << 90;
    QTest::newRow("a short-lived token is refreshed at half its life") << false
        << QStringList{"authenticate", "redirect", "grant 0 A 300"}
        << QStringList{"NoLogin / NoBrowser", "NoLogin / Waiting", "NoLogin / Exchanging",
                       "Holding / NoBrowser / A"}
        << QStringList{"granted A"} << 1 << QList<int>{} << 150;
    // clang-format on
}

void OAuthManagerTest::transitions()
{
    QFETCH(bool, stored);
    QFETCH(QStringList, steps);
    QFETCH(QStringList, states);
    QFETCH(QStringList, signals_);
    QFETCH(int, requests);
    QFETCH(QList<int>, running);
    QFETCH(int, due);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FakeNetworkManager network;
    SqliteDataStore datastore(dir.filePath("global.db"));
    if (stored) {
        datastore.Set("oauth_token", R"({"username":"Stored","refresh_token":"refresh-Stored"})");
    }
    OAuthManager manager(network, datastore);
    const OAuthTestAccess access(manager);

    QStringList emitted;
    connect(&manager, &OAuthManager::grantAccess, this, [&](const OAuthToken &token) {
        emitted << "granted " + token.username;
    });
    connect(&manager, &OAuthManager::loginFailed, this, [&] { emitted << "loginFailed"; });
    connect(&manager, &OAuthManager::storedLoginFailed, this, [&] {
        emitted << "storedLoginFailed";
    });

    const auto state = [&] {
        const QString account = access.heldAccount();
        return access.state() + (account.isEmpty() ? QString() : " / " + account);
    };

    QCOMPARE(state(), states.value(0));
    QString last_state;
    for (qsizetype i = 0; i < steps.size(); ++i) {
        const QStringList step = steps[i].split(' ');
        const QString &verb = step[0];
        if (verb == "authenticate") {
            QVERIFY(manager.initLogin());
        } else if (verb == "redirect") {
            const QString waiting = access.waitingState();
            if (step.value(1) == "again") {
                access.redirect({{"code", "code"}, {"state", last_state}});
            } else if (step.value(1) == "wrong") {
                access.redirect({{"code", "code"}, {"state", waiting + "-wrong"}});
            } else {
                last_state = waiting;
                access.redirect({{"code", "code"}, {"state", waiting}});
            }
        } else if (verb == "end") {
            manager.endLogin();
        } else if (verb == "timer") {
            access.refreshNow();
        } else {
            const int n = step.value(1).toInt();
            QVERIFY(n < network.count());
            InFlightReply *reply = network.sent(size_t(n)).reply;
            QVERIFY(reply);
            if (verb == "grant") {
                reply->finish({{"Content-Type", "application/json"}},
                              200,
                              QNetworkReply::NoError,
                              token(step.value(2), step.value(3, "36000").toInt()));
            } else if (verb == "refuse") {
                reply->finish({{"Content-Type", "application/json"}},
                              400,
                              QNetworkReply::ProtocolInvalidOperationError,
                              R"({"error":"invalid_grant"})");
            } else if (verb == "answer") {
                QList<QNetworkReply::RawHeaderPair> headers{{"Content-Type", "application/json"}};
                if (step.size() > 3) {
                    headers.append({"Retry-After", step[3].toLatin1()});
                }
                const int status = step.value(2).toInt();
                reply->finish(headers,
                              status,
                              (status == 429) ? QNetworkReply::UnknownContentError
                                              : QNetworkReply::InternalServerError,
                              status == 429 ? R"({"error":"rate_limited"})"
                                            : R"({"error":"server_error"})");
            } else {
                QCOMPARE(verb, QString("fail"));
                reply->finish({}, 0, QNetworkReply::ConnectionRefusedError);
            }
        }
        QVERIFY2(state() == states.value(i + 1),
                 qPrintable(QString("after '%1': %2, expected %3")
                                .arg(steps[i], state(), states.value(i + 1))));
    }
    QCOMPARE(emitted, signals_);
    QCOMPARE(network.count(), requests);
    QList<int> still_running;
    for (int n = 0; n < network.count(); ++n) {
        if (network.sent(size_t(n)).reply && network.sent(size_t(n)).reply->isRunning()) {
            still_running << n;
        }
    }
    QCOMPARE(still_running, running);
    const auto remaining = access.refreshDue();
    const int seconds = remaining ? int(remaining->count() / 1000) : -1;
    // A coarse timer may report a second either way.
    QVERIFY2(std::abs(seconds - due) <= 1,
             qPrintable(QString("refresh due in %1 s, expected %2").arg(seconds).arg(due)));
}

// What an answer of the token endpoint comes to (OAuthManager::classify):
// only a 4xx other than 429 that names an error refuses the grant; any other
// failure is passing, and is tried again no sooner than Retry-After asks.
void OAuthManagerTest::classify_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<int>("retry_after"); // -1 for none
    QTest::addColumn<QString>("verdict");

    const QByteArray granted = token("A", 36000);
    // clang-format off
    QTest::newRow("200 with a token") << 200 << granted << -1 << "granted";
    QTest::newRow("200 malformed") << 200 << QByteArray("{\"access_token\":") << -1 << "retry";
    QTest::newRow("200 without a token") << 200 << QByteArray("{}") << -1 << "retry";
    QTest::newRow("400 invalid_grant") << 400 << QByteArray(R"({"error":"invalid_grant"})") << -1 << "refused";
    QTest::newRow("400 another error") << 400 << QByteArray(R"({"error":"invalid_request"})") << -1 << "refused";
    QTest::newRow("400 an error that is not a string") << 400 << QByteArray(R"({"error":{"code":1}})") << -1 << "retry";
    QTest::newRow("401 invalid_client") << 401 << QByteArray(R"({"error":"invalid_client"})") << -1 << "refused";
    QTest::newRow("429 with Retry-After") << 429 << QByteArray(R"({"error":"rate_limited"})") << 90 << "retry after 90";
    QTest::newRow("429 without Retry-After") << 429 << QByteArray() << -1 << "retry";
    QTest::newRow("500 naming an error") << 500 << QByteArray(R"({"error":"server_error"})") << -1 << "retry";
    QTest::newRow("503 temporarily_unavailable") << 503 << QByteArray(R"({"error":"temporarily_unavailable"})") << 30 << "retry after 30";
    QTest::newRow("503 from the CDN, in HTML") << 503 << QByteArray("<html>Service Unavailable</html>") << -1 << "retry";
    QTest::newRow("no answer") << 0 << QByteArray() << -1 << "retry";
    // clang-format on
}

void OAuthManagerTest::classify()
{
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(int, retry_after);
    QFETCH(QString, verdict);
    QCOMPARE(OAuthTestAccess::classify(status,
                                       body,
                                       retry_after < 0 ? std::nullopt
                                                       : std::optional<int>(retry_after)),
             verdict);
}

ACQ_TEST_MAIN(OAuthManagerTest)
#include "tst_oauthmanager.moc"
