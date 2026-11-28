// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ratelimit/networkcapture.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QNetworkReply>

#include "ratelimit/ratelimitedrequest.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

NetworkCapture::NetworkCapture(const QString &file_path, qint64 max_bytes)
    : m_file(file_path)
    , m_max_bytes(max_bytes)
{
    if (!m_file.open(QIODevice::Append)) {
        spdlog::error("Network capture: cannot open '{}': {}; capture is disabled.",
                      file_path,
                      m_file.errorString());
    }
}

void NetworkCapture::RecordHeadResponse(const QString &endpoint, const QNetworkReply *reply)
{
    QJsonObject record;
    record["kind"] = "head";
    record["endpoint"] = endpoint;
    record["url"] = reply->url().toString();
    // UTC so the timestamp carries its offset ("Z"): local-time QDateTime
    // serializes with no offset at all, which downstream tooling rejects
    // as an ambiguous clock.
    record["received"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    AddReplyFields(record, reply);
    Append(std::move(record));
}

void NetworkCapture::RecordReply(const QString &policy_name,
                                 const RateLimitedRequest &request,
                                 const QNetworkReply *reply,
                                 const QDateTime &received)
{
    QJsonObject record;
    record["kind"] = "reply";
    record["policy"] = policy_name;
    record["endpoint"] = request.endpoint;
    record["request_id"] = static_cast<qint64>(request.id);
    record["url"] = request.network_request.url().toString();
    record["scheduled"] = request.scheduled_time.toUTC().toString(Qt::ISODateWithMs);
    record["sent"] = request.send_time.toUTC().toString(Qt::ISODateWithMs);
    record["received"] = received.toUTC().toString(Qt::ISODateWithMs);
    AddReplyFields(record, reply);
    Append(std::move(record));
}

void NetworkCapture::RecordPumpFailure(const QString &policy_name,
                                       const QString &error,
                                       qsizetype dropped)
{
    QJsonObject record;
    record["kind"] = "pump_failed";
    record["policy"] = policy_name;
    record["error"] = error;
    record["dropped"] = static_cast<qint64>(dropped);
    record["time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    Append(std::move(record));
}

void NetworkCapture::Append(QJsonObject record)
{
    if (!m_file.isOpen()) {
        return;
    }
    record["v"] = SCHEMA_VERSION;
    QByteArray line = QJsonDocument(record).toJson(QJsonDocument::Compact);
    line.append('\n');
    if (m_file.size() + line.size() > m_max_bytes) {
        Rotate();
        if (!m_file.isOpen()) {
            return;
        }
    }
    m_file.write(line);
    // Flush per record: capture traffic is at most a few lines per second,
    // and an unflushed tail would lose exactly the records leading up to a
    // crash or violation — the ones the capture exists for.
    m_file.flush();
}

void NetworkCapture::AddReplyFields(QJsonObject &record, const QNetworkReply *reply)
{
    record["status"] = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        record["error"] = static_cast<int>(reply->error());
        record["error_string"] = reply->errorString();
    }
    // Header names are stored lowercased: Qt already normalizes their case,
    // so lowercasing keeps the capture format stable across Qt versions.
    // Values are verbatim.
    QJsonObject headers;
    const auto &pairs = reply->rawHeaderPairs();
    for (const auto &pair : pairs) {
        const QString name = QString::fromUtf8(pair.first).toLower();
        if (name.startsWith("x-rate-limit") || (name == "retry-after") || (name == "date")) {
            headers[name] = QString::fromUtf8(pair.second);
        }
    }
    record["headers"] = headers;
}

void NetworkCapture::Rotate()
{
    const QString path = m_file.fileName();
    const QString backup = path + ".1";
    m_file.close();
    QFile::remove(backup);
    if (!QFile::rename(path, backup)) {
        spdlog::error("Network capture: could not rotate '{}' to '{}'", path, backup);
    }
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        spdlog::error("Network capture: cannot reopen '{}': {}; capture is disabled.",
                      path,
                      m_file.errorString());
    }
}
