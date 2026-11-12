// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QObject>
#include <QAbstractOAuth>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "util/oauthtoken.h"

class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;

class DataStore;
class NetworkManager;

class OAuthManager : public QObject
{
    Q_OBJECT

public:
    explicit OAuthManager(NetworkManager& manager, DataStore &datastore, QObject *parent = nullptr);
    void initLogin();

private slots:
    void onRequestFailure(const QAbstractOAuth::Error error);
    void onServerError(const QString &error, const QString &errorDescription, const QUrl &uri);
    void receiveToken(const QVariantMap &tokens);
    void receiveGrant();

signals:
    void grantAccess(const OAuthToken &token);

private:
    NetworkManager &m_network_manager;
    DataStore &m_data;

    QOAuth2AuthorizationCodeFlow* m_oauth;
    QOAuthHttpServerReplyHandler* m_handler;

    OAuthToken m_token;
};
