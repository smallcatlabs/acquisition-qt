// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QTemporaryDir>

#include "fakenetwork.h"
#include "ratelimit/networkcapture.h"
#include "ratelimit/ratelimitedrequest.h"
#include "testsuite.h"

namespace {

    QList<QJsonObject> readRecords(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QList<QJsonObject> records;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                return {}; // any unparseable line fails the caller's count check
            }
            records.append(doc.object());
        }
        return records;
    }

} // namespace

class NetworkCaptureTest : public QObject
{
    Q_OBJECT

private slots:
    void appendWritesParseableJsonLines();
    void rotationKeepsOneBackup();
    void replyRecordCarriesHeadersAndTiming();
    void headRecordSurvivesDegradedReply();
};

void NetworkCaptureTest::appendWritesParseableJsonLines()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("capture.jsonl");

    NetworkCapture capture(path);
    capture.Append(QJsonObject{{"kind", "test"}, {"n", 1}});
    capture.Append(QJsonObject{{"kind", "test"}, {"n", 2}});

    const auto records = readRecords(path);
    QCOMPARE(records.size(), 2);
    QCOMPARE(records[0]["v"].toInt(), NetworkCapture::SCHEMA_VERSION);
    QCOMPARE(records[0]["n"].toInt(), 1);
    QCOMPARE(records[1]["n"].toInt(), 2);
}

void NetworkCaptureTest::rotationKeepsOneBackup()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("capture.jsonl");

    NetworkCapture capture(path, 128);
    const QString filler(64, 'x');
    for (int i = 0; i < 10; ++i) {
        capture.Append(QJsonObject{{"n", i}, {"filler", filler}});
    }

    QVERIFY(QFile::exists(path));
    QVERIFY(QFile::exists(path + ".1"));
    QVERIFY(QFileInfo(path).size() <= 128);

    // Every line in both files must still be parseable, and the newest
    // record must be in the current file.
    const auto current = readRecords(path);
    const auto backup = readRecords(path + ".1");
    QVERIFY(!current.isEmpty());
    QVERIFY(!backup.isEmpty());
    QCOMPARE(current.last()["n"].toInt(), 9);
}

void NetworkCaptureTest::replyRecordCarriesHeadersAndTiming()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("capture.jsonl");
    NetworkCapture capture(path);

    const QUrl url("https://api.pathofexile.com/stash/Standard");
    RateLimitedRequest request("List Stashes", QNetworkRequest(url), std::stop_token{});
    request.send_time = QDateTime::fromString("2026-07-18T12:00:01.500", Qt::ISODateWithMs);
    request.scheduled_time = QDateTime::fromString("2026-07-18T12:00:01.400", Qt::ISODateWithMs);
    const QDateTime received = QDateTime::fromString("2026-07-18T12:00:02.250", Qt::ISODateWithMs);

    const QList<QNetworkReply::RawHeaderPair> headers = {
        {"X-Rate-Limit-Policy", "stash-list-request-limit"},
        {"X-Rate-Limit-Rules", "Ip"},
        {"X-Rate-Limit-Ip", "30:60:60,100:1800:600"},
        {"X-Rate-Limit-Ip-State", "1:60:0,1:1800:0"},
        {"Date", "Sat, 18 Jul 2026 12:00:02 GMT"},
        {"Content-Type", "application/json"}, // must NOT be captured
    };
    FakeNetworkReply reply(QNetworkRequest(url), "{}", QNetworkReply::NoError, nullptr, headers, 200);

    capture.RecordReply("stash-list-request-limit", request, &reply, received);

    const auto records = readRecords(path);
    QCOMPARE(records.size(), 1);
    const QJsonObject record = records[0];
    QCOMPARE(record["kind"].toString(), "reply");
    QCOMPARE(record["policy"].toString(), "stash-list-request-limit");
    QCOMPARE(record["endpoint"].toString(), "List Stashes");
    QCOMPARE(record["url"].toString(), url.toString());
    QVERIFY(record["request_id"].toInteger() > 0);
    QCOMPARE(record["status"].toInt(), 200);
    QVERIFY(!record.contains("error"));
    // Capture timestamps are UTC (f53d8cb1): converting the local fixture
    // times the same way keeps the comparison timezone-independent, and
    // the trailing 'Z' pins the unambiguous-offset contract itself.
    QVERIFY(record["sent"].toString().endsWith("Z"));
    QCOMPARE(record["sent"].toString(), request.send_time.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(record["scheduled"].toString(),
             request.scheduled_time.toUTC().toString(Qt::ISODateWithMs));
    QCOMPARE(record["received"].toString(), received.toUTC().toString(Qt::ISODateWithMs));

    // Header names are lowercased in the capture; values are verbatim.
    const QJsonObject captured = record["headers"].toObject();
    QCOMPARE(captured["x-rate-limit-ip"].toString(), "30:60:60,100:1800:600");
    QCOMPARE(captured["x-rate-limit-ip-state"].toString(), "1:60:0,1:1800:0");
    QCOMPARE(captured["date"].toString(), "Sat, 18 Jul 2026 12:00:02 GMT");
    QVERIFY(!captured.contains("content-type"));
}

void NetworkCaptureTest::headRecordSurvivesDegradedReply()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("capture.jsonl");
    NetworkCapture capture(path);

    // The N20 shape: policy header present, everything else missing.
    const QUrl url("https://api.pathofexile.com/character");
    const QList<QNetworkReply::RawHeaderPair> headers = {
        {"X-Rate-Limit-Policy", "character-request-limit"},
    };
    FakeNetworkReply reply(QNetworkRequest(url), "", QNetworkReply::NoError, nullptr, headers, 200);

    capture.RecordHeadResponse("Get Character", &reply);

    const auto records = readRecords(path);
    QCOMPARE(records.size(), 1);
    const QJsonObject record = records[0];
    QCOMPARE(record["kind"].toString(), "head");
    QCOMPARE(record["endpoint"].toString(), "Get Character");
    QCOMPARE(record["status"].toInt(), 200);
    const QJsonObject captured = record["headers"].toObject();
    QCOMPARE(captured.size(), 1);
    QCOMPARE(captured["x-rate-limit-policy"].toString(), "character-request-limit");
}

ACQ_TEST_GUILESS_MAIN(NetworkCaptureTest)

#include "tst_networkcapture.moc"
