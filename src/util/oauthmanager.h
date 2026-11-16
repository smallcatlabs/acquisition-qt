// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QAbstractOAuth>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include <optional>

#include "util/oauthtoken.h"

class QNetworkReply;
class QOAuth2AuthorizationCodeFlow;
class QOAuthHttpServerReplyHandler;

class DataStore;
class NetworkManager;

class OAuthManager : public QObject
{
    Q_OBJECT

public:
    explicit OAuthManager(NetworkManager &manager, DataStore &datastore, QObject *parent = nullptr);
    // Starts a browser login. An attempt still waiting on the browser is
    // dropped first: its listener is closed and its state is replaced, so its
    // redirect, should it still come, logs nobody in. Returns false, without
    // opening the browser, when the callback listener cannot be opened on
    // 127.0.0.1.
    bool initLogin();

private slots:
    void onRequestFailure(const QAbstractOAuth::Error error);
    void onServerError(const QString &error, const QString &errorDescription, const QUrl &uri);
    void receiveCallback(const QVariantMap &data);
    void receiveToken(const QVariantMap &tokens);
    void receiveGrant();

signals:
    void grantAccess(const OAuthToken &token);
    // The browser login initLogin() started has ended without access.
    // `reason` is a line for the user. It holds at most the error code and
    // the description the server sent, never a code or a token.
    void loginFailed(const QString &reason);

private:
    // The error with which the token endpoint answered (RFC 6749, 5.2), read
    // from its reply. Both are empty when the reply names none.
    struct TokenReply
    {
        QString error;
        QString description;
    };

    void noteTokenReply(QNetworkReply *reply);
    // Ends the browser login in progress, if there is one: nothing listens
    // for its redirect any more.
    void endLogin();
    void failLogin(const QString &reason);

    NetworkManager &m_network_manager;
    DataStore &m_data;

    QOAuth2AuthorizationCodeFlow *m_oauth;
    QOAuthHttpServerReplyHandler *m_handler;

    OAuthToken m_token;

    // A browser login has started and has neither been granted nor failed.
    bool m_login_in_progress{false};
    // The last redirect received carried another state than its login's.
    bool m_state_mismatch{false};
    // The token endpoint's reply that Qt is handling, while it handles it.
    std::optional<TokenReply> m_token_reply;
};
