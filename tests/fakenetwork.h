// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QNetworkReply>
#include <QNetworkRequest>

#include <cstring>
#include <stop_token>
#include <vector>

#include <QFuture>
#include <QPromise>

#include "ratelimit/fetcherror.h"
#include "ratelimit/ratelimiter.h"

// An offline stand-in for a completed QNetworkReply. The body is served
// through readData() so QNetworkReply::readAll() works as usual.
//
// No Q_OBJECT: neither class adds signals, slots, or properties, and
// omitting it lets this header stay out of the build's moc set.
class FakeNetworkReply : public QNetworkReply
{
public:
    explicit FakeNetworkReply(const QNetworkRequest &request,
                              const QByteArray &body,
                              QNetworkReply::NetworkError error = QNetworkReply::NoError,
                              QObject *parent = nullptr,
                              const QList<QNetworkReply::RawHeaderPair> &headers = {},
                              int http_status = 0)
        : QNetworkReply(parent)
        , m_body(body)
    {
        setRequest(request);
        setUrl(request.url());
        setOpenMode(QIODevice::ReadOnly);
        setFinished(true);
        if (error != QNetworkReply::NoError) {
            setError(error, "fake network error");
        }
        for (const auto &header : headers) {
            setRawHeader(header.first, header.second);
        }
        if (http_status != 0) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, http_status);
        }
    }

    void abort() override {}
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

// A RateLimiter whose SubmitFuture() records requests instead of touching
// the network (D1). Tests complete them with resolve()/reject(), synchronously
// and in any order. Used by the facade and Shop tests; the worker suite drives
// the typed facade fake one level up (tests/fakeapiclient.h).
class FakeRateLimiter : public RateLimiter
{
public:
    explicit FakeRateLimiter(NetworkManager &network_manager)
        : RateLimiter(network_manager)
    {}

    struct PendingFuture
    {
        QString endpoint;
        QNetworkRequest request;
        std::stop_token token;
        std::shared_ptr<QPromise<RateLimit::FetchOutcome>> promise;
    };

    QFuture<RateLimit::FetchOutcome> SubmitFuture(const QString &endpoint,
                                                  QNetworkRequest network_request,
                                                  std::stop_token token = {}) override
    {
        auto promise = std::make_shared<QPromise<RateLimit::FetchOutcome>>();
        promise->start();
        QFuture<RateLimit::FetchOutcome> future = promise->future();
        m_futures.push_back({endpoint, network_request, std::move(token), std::move(promise)});
        return future;
    }

    size_t futureCount() const { return m_futures.size(); }
    const PendingFuture &pendingFuture(size_t i) const { return m_futures.at(i); }

    void resolve(size_t i, const QByteArray &body) { complete(i, RateLimit::FetchOutcome(body)); }

    void reject(size_t i,
                RateLimit::FetchError::Kind kind,
                const QString &message = {},
                int http_status = 0)
    {
        const PendingFuture &pending = m_futures.at(i);
        RateLimit::FetchError error;
        error.kind = kind;
        error.endpoint = pending.endpoint;
        error.url = pending.request.url();
        error.http_status = http_status;
        error.message = message;
        complete(i, RateLimit::FetchOutcome(std::unexpected(std::move(error))));
    }

    void complete(size_t i, RateLimit::FetchOutcome outcome)
    {
        auto &promise = m_futures.at(i).promise;
        if (!promise) {
            qFatal("FakeRateLimiter::complete: request %zu was already completed", i);
        }
        promise->addResult(std::move(outcome));
        promise->finish();
        promise.reset();
    }

private:
    std::vector<PendingFuture> m_futures;
};
