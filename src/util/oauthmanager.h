// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2023 Tom Holz

#pragma once

#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <chrono>
#include <optional>
#include <variant>

#include "util/oauthtoken.h"

class QOAuthHttpServerReplyHandler;

class DataStore;
class NetworkManager;

// The OAuth login with Path of Exile: the authorization code grant with PKCE
// and a loopback redirect, and the refresh of the token it gives. Qt's
// handler only listens for the redirect; the two token requests are sent
// here, through the application's NetworkManager. oauthmanager.cpp has the
// state machine.
class OAuthManager : public QObject
{
    Q_OBJECT

public:
    explicit OAuthManager(NetworkManager &manager, DataStore &datastore, QObject *parent = nullptr);
    // Starts a browser login. An attempt still waiting on the browser, or
    // whose code is being exchanged, is dropped first: its redirect, should
    // it still come, and its exchange's answer log nobody in. Returns false,
    // without opening the browser, when the callback listener cannot be
    // opened on 127.0.0.1.
    bool initLogin();
    // Ends the browser login in progress, if there is one, its code exchange
    // included: nothing it was waiting for can log anybody in any more. For
    // whoever starts a session with the login already held (#408, #414).
    void endLogin();

signals:
    // A token was taken: the answer of a code exchange, or of a refresh.
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
    friend class OAuthTestAccess;

    // The state machine (oauthmanager.cpp has its diagram) has two regions,
    // the login the session uses and the browser login, because a refresh
    // and a browser login run side by side. A request in flight is held by
    // the state that waits for its answer, and nowhere else.

    // The login the session uses.
    struct NoLogin
    {};
    struct RefreshingStored // the stored login, refreshed at start
    {
        QPointer<QNetworkReply> refresh;
        QString refresh_token;
    };
    struct Holding // a token taken in this run
    {
        OAuthToken token;
        QPointer<QNetworkReply> refresh{}; // its refresh, while one is on its way
        int failures = 0;                  // refreshes since the last that worked
    };
    struct Refused // the server refused the refresh: kept until it expires
    {
        OAuthToken token;
    };
    using Login = std::variant<NoLogin, RefreshingStored, Holding, Refused>;

    // The browser login.
    struct NoBrowser
    {};
    struct Waiting // for the redirect, with the listener open
    {
        QString state;
        QString verifier;
        QString redirect_uri;
    };
    struct Exchanging // the redirect's code; the listener stays open
    {
        QPointer<QNetworkReply> exchange;
    };
    using Browser = std::variant<NoBrowser, Waiting, Exchanging>;

    // What an answer of the token endpoint comes to.
    struct Verdict
    {
        enum Kind { Granted, Refused, Transient } kind;
        std::chrono::seconds retry_after{0}; // the least wait before trying again
        bool operator==(const Verdict &) const = default;
    };
    // From the answer's HTTP status (0 when none came), its body, and its
    // Retry-After header, if a valid one.
    static Verdict classify(int http_status,
                            const QJsonObject &body,
                            std::optional<int> retry_after);

    // What the token endpoint answered.
    struct Answer
    {
        Verdict verdict;
        QVariantMap fields;  // the reply's JSON object
        QString error;       // the error it names (RFC 6749, 5.2), or empty
        QString description; // and that error's description
        int http_status;     // 0 when no answer came
        QNetworkReply::NetworkError network_error;
    };

    // The transitions of each region: the state is replaced, and the request
    // the state before was waiting for, unless the new one waits for it too,
    // is abandoned.
    void enter(Login next);
    void enter(Browser next);

    void receiveRedirect(const QVariantMap &query);
    void receiveAnswer(QNetworkReply *reply);
    void exchangeAnswered(const Answer &answer);
    void refreshAnswered(const Answer &answer);
    void hold(const OAuthToken &token);
    void failLogin(const QString &reason);
    void failStoredLogin(const Answer &answer);
    // What the refresh timer calls: refreshes the token held, unless a
    // refresh of it is already on its way.
    void refreshNow();
    QNetworkReply *post(const QByteArray &form);

    NetworkManager &m_network_manager;
    DataStore &m_data;
    QOAuthHttpServerReplyHandler *m_listener;
    QTimer m_refresh_timer;

    Login m_login;
    Browser m_browser;
};
