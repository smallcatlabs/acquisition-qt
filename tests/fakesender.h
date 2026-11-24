// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QElapsedTimer>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <algorithm>
#include <cstring>
#include <vector>

#include "ratelimit/ratelimitmanager.h"

// A QNetworkReply that starts out in flight, unlike FakeNetworkReply
// (fakenetwork.h) which is born finished. RateLimitManager connects to
// finished() after its SendFcn returns, so the harness must hand back an
// unfinished reply and complete it later with finish().
//
// No Q_OBJECT: no new signals, slots, or properties (same reasoning as
// fakenetwork.h — keeps this header out of the build's moc set).
class InFlightReply : public QNetworkReply
{
public:
    explicit InFlightReply(const QNetworkRequest &request, QObject *parent = nullptr)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOpenMode(QIODevice::ReadOnly);
    }

    // Complete the reply: install the synthetic headers, status, and error, an
    // optional response body (served through readData so readAll() works, for
    // full-chain tests whose consumer parses the payload — the manager's own
    // suite never reads a body and passes none), then emit finished() so
    // RateLimitManager::ReceiveReply runs synchronously on the caller's stack.
    void finish(const QList<QNetworkReply::RawHeaderPair> &headers,
                int http_status = 0,
                QNetworkReply::NetworkError error = QNetworkReply::NoError,
                const QByteArray &body = {})
    {
        if (isFinished()) {
            qFatal("InFlightReply::finish: reply for %s was already finished",
                   qPrintable(url().toString()));
        }
        for (const auto &header : headers) {
            setRawHeader(header.first, header.second);
        }
        if (http_status != 0) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, http_status);
        }
        if (error != QNetworkReply::NoError) {
            setError(error, "fake network error");
        }
        m_body = body;
        setFinished(true);
        emit finished();
    }

    // As Qt's: a reply still running finishes at once, inside the call,
    // with OperationCanceledError; a finished one is left alone.
    void abort() override
    {
        if (!isFinished()) {
            finish({}, 0, QNetworkReply::OperationCanceledError);
        }
    }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override
    {
        return (m_body.size() - m_offset) + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 n = std::min<qint64>(maxSize, m_body.size() - m_offset);
        if (n <= 0) {
            return -1;
        }
        std::memcpy(data, m_body.constData() + m_offset, size_t(n));
        m_offset += n;
        return n;
    }

private:
    QByteArray m_body;
    qint64 m_offset{0};
};

// The fake sender the F57 finding and the network-redesign testing plan
// (item 1) demand: records every request the manager sends, with a
// monotonic timestamp, and returns an in-flight reply for the test to
// complete. Must outlive any RateLimitManager holding fcn(): declare the
// sender first so it is destroyed last.
class FakeSender : public QObject
{
public:
    struct Sent
    {
        QNetworkRequest request;
        QPointer<InFlightReply> reply; // nulls once the manager deleteLater()s it
        qint64 elapsed_ms;             // since the sender was constructed
    };

    FakeSender() { m_clock.start(); }

    RateLimitManager::SendFcn fcn()
    {
        return [this](QNetworkRequest &request) -> QNetworkReply * {
            auto *reply = new InFlightReply(request, this);
            m_sent.push_back({request, reply, m_clock.elapsed()});
            return reply;
        };
    }

    int count() const { return static_cast<int>(m_sent.size()); }
    const Sent &sent(size_t i) const { return m_sent.at(i); }

private:
    QElapsedTimer m_clock;
    std::vector<Sent> m_sent;
};
