// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QObject>
#include <QNetworkAccessManager>

class QNetworkDiskCache;

class NetworkManager : public QNetworkAccessManager
{
    Q_OBJECT

public:
    explicit NetworkManager(QObject *parent = nullptr);

    void clearPoeSessionId();
    void setPoeSessionId(const QString &poesessid);
    void setBearerToken(const QString &token);

    static void logRequest(const QNetworkRequest &request);
    static void logReply(const QNetworkReply *reply);

    // Observation-only rate-limit logging for the OAuth token endpoint
    // (ground truth N33): the endpoint carries its own IP-scoped policy but
    // stays outside the rate-limiter hub (network-redesign D5 scope
    // rationale). Public and static so tests can drive it with a fake reply.
    static void logOAuthTokenRateLimits(QNetworkReply *reply);

protected:
    QNetworkReply *createRequest(QNetworkAccessManager::Operation op,
                                 const QNetworkRequest &originalRequest,
                                 QIODevice *outgoingData = nullptr) override;

private:
    QNetworkDiskCache* m_diskCache;
    QByteArray m_bearerToken;

    using AttributeGetter = std::function<QVariant(QNetworkRequest::Attribute)>;

    static void logAttributes(const QString &name, AttributeGetter attrs);
    static void logHeaders(const QString &name, const QHttpHeaders &headers);
};
