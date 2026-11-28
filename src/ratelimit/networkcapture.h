// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QFile>
#include <QJsonObject>
#include <QString>

class QDateTime;
class QNetworkReply;

struct RateLimitedRequest;

// Records every rate-limited exchange (HEAD probes included) as one JSON
// object per line, with the verbatim rate-limit headers and timing data.
// This is the research instrument specified in
// docs/design/network-ground-truth.md: it observes traffic and never
// influences it. Off by default; enabled by the 'network_capture_enabled'
// setting.
class NetworkCapture
{
public:
    // Records are dropped once the file would exceed the cap; one rotated
    // backup ('<file>.1') is kept, so worst-case disk use is twice the cap.
    static constexpr qint64 DEFAULT_MAX_BYTES = 20LL * 1024 * 1024;

    // Stamped into every record as 'v' so old captures stay identifiable
    // when the format evolves.
    static constexpr int SCHEMA_VERSION = 1;

    explicit NetworkCapture(const QString &file_path, qint64 max_bytes = DEFAULT_MAX_BYTES);

    // Record the HEAD probe used to set up an endpoint. Called before any
    // validation so degraded probe replies (missing rate-limit headers) are
    // captured too — those are exactly the cases worth studying (N20).
    void RecordHeadResponse(const QString &endpoint, const QNetworkReply *reply);

    // Record a completed exchange: the scheduling decision, send and receive
    // times, HTTP status, and the raw rate-limit headers.
    void RecordReply(const QString &policy_name,
                     const RateLimitedRequest &request,
                     const QNetworkReply *reply,
                     const QDateTime &received);

    // Record a pump entering the terminal failed state (the exception
    // policy: logged loudly and captured): the policy, what was thrown, and
    // how many queued requests were failed with it.
    void RecordPumpFailure(const QString &policy_name, const QString &error, qsizetype dropped);

    // Append one record as a line of JSON, stamping the schema version and
    // rotating the file at the size cap.
    void Append(QJsonObject record);

private:
    // Add status, error (if any), and the verbatim X-Rate-Limit, Retry-After,
    // and Date headers to a record.
    static void AddReplyFields(QJsonObject &record, const QNetworkReply *reply);

    void Rotate();

    QFile m_file;
    const qint64 m_max_bytes;
};
