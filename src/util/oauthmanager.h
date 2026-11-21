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
    // Ends the browser login in progress, if there is one: nothing listens
    // for its redirect any more, so it can log nobody in. For whoever starts
    // a session with the login already held (#408).
    void endLogin();

private slots:
    void onRequestFailure(const QAbstractOAuth::Error error);
    void onServerError(const QString &error, const QString &errorDescription, const QUrl &uri);
    void receiveCallback(const QVariantMap &data);
    // Takes the tokens of a reply noteTokenReply let through: a refresh's,
    // or a code exchange's, which ends the browser login.
    void receiveToken(const QVariantMap &tokens);

signals:
    void grantAccess(const OAuthToken &token);
    // The browser login initLogin() started has ended without access.
    // `reason` is a line for the user. It holds at most the error code and
    // the description the server sent, never a code or a token.
    void loginFailed(const QString &reason);
    // The refresh of the stored login, at start, has failed: the server
    // refused the token, which is then gone from the store, or could not be
    // reached, and the token is kept. `reason` says which, to the user. A
    // browser login the user has started meanwhile is still waiting.
    void storedLoginFailed(const QString &reason);

private:
    // What a reply of the token endpoint said, beyond what Qt reports.
    struct TokenReply
    {
        bool refresh;        // it answers a refresh, not a code exchange
        QString error;       // the error it names (RFC 6749, 5.2), or empty
        QString description; // and that error's description
        int http_status;     // 0 when no answer came
        int network_error;   // QNetworkReply::NetworkError
    };

    // Notes a reply of the token endpoint before Qt handles it, and says
    // whether Qt may: only while the login it was sent for still waits.
    // Called with nullptr once Qt has handled it.
    bool noteTokenReply(QNetworkReply *reply);
    void failStoredLogin(const TokenReply &reply);
    void failLogin(const QString &reason);
    bool loginInProgress() const { return !m_attempt.isEmpty(); }
    // Which login waits for an answer of this stage now, as a fingerprint;
    // empty when none does. Each request carries the one it was sent for.
    QByteArray waitingFor(QAbstractOAuth::Stage stage) const;

    NetworkManager &m_network_manager;
    DataStore &m_data;

    QOAuth2AuthorizationCodeFlow *m_oauth;
    QOAuthHttpServerReplyHandler *m_handler;

    OAuthToken m_token;

    // The state of the browser login that has started and has neither been
    // granted nor failed; empty when there is none.
    QString m_attempt;
    // The last redirect received carried another state than its login's.
    bool m_state_mismatch{false};
    // The token endpoint's reply that Qt is handling, while it handles it.
    std::optional<TokenReply> m_token_reply;
};
