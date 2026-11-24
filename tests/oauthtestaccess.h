// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>

#include <chrono>
#include <optional>
#include <type_traits>
#include <variant>

#include "util/oauthmanager.h"

// The test seam into OAuthManager, on the pattern of ApplicationTestAccess:
// OAuthManager only names this class a friend, and it is defined here.
class OAuthTestAccess
{
public:
    explicit OAuthTestAccess(OAuthManager &manager)
        : m(manager)
    {}

    // What the refresh timer does when it fires: a scenario decides the
    // moment instead of the token's expiry against the wall clock.
    void refreshNow() const { m.refreshNow(); }

    // How long until the refresh timer fires, if it is armed.
    std::optional<std::chrono::milliseconds> refreshDue() const
    {
        if (!m.m_refresh_timer.isActive()) {
            return std::nullopt;
        }
        return m.m_refresh_timer.remainingTimeAsDuration();
    }

    // OAuthManager::classify, in words: "granted", "refused", "retry", or
    // "retry after N" when the answer asks for at least N seconds.
    static QString classify(int http_status, const QByteArray &body, std::optional<int> retry_after)
    {
        const auto verdict = OAuthManager::classify(http_status,
                                                    QJsonDocument::fromJson(body).object(),
                                                    retry_after);
        switch (verdict.kind) {
        case OAuthManager::Verdict::Granted:
            return "granted";
        case OAuthManager::Verdict::Refused:
            return "refused";
        case OAuthManager::Verdict::Transient:
            break;
        }
        return verdict.retry_after.count()
                   ? QString("retry after %1").arg(verdict.retry_after.count())
                   : QString("retry");
    }

    // A redirect's query, as the listener reports it.
    void redirect(const QVariantMap &query) const { m.receiveRedirect(query); }

    // The state of each region, as "login / browser", in the names of
    // oauthmanager.h; "+retryN" after N refreshes that got no answer, and
    // "+refresh" when a refresh of the token held is on its way.
    QString state() const
    {
        const QString login = std::visit(
            [](const auto &s) -> QString {
                using S = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<S, OAuthManager::NoLogin>) {
                    return "NoLogin";
                } else if constexpr (std::is_same_v<S, OAuthManager::RefreshingStored>) {
                    return "RefreshingStored";
                } else if constexpr (std::is_same_v<S, OAuthManager::Refused>) {
                    return "Refused";
                } else {
                    return QString("Holding")
                           + (s.failures ? QString("+retry%1").arg(s.failures) : QString())
                           + (s.refresh ? "+refresh" : "");
                }
            },
            m.m_login);
        const QString browser = std::visit(
            [](const auto &s) -> QString {
                using S = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<S, OAuthManager::NoBrowser>) {
                    return "NoBrowser";
                } else if constexpr (std::is_same_v<S, OAuthManager::Waiting>) {
                    return "Waiting";
                } else {
                    return "Exchanging";
                }
            },
            m.m_browser);
        return login + " / " + browser;
    }

    // The username of the token held, or empty.
    QString heldAccount() const
    {
        if (const auto *held = std::get_if<OAuthManager::Holding>(&m.m_login)) {
            return held->token.username;
        }
        const auto *refused = std::get_if<OAuthManager::Refused>(&m.m_login);
        return refused ? refused->token.username : QString();
    }

    // The state the waiting browser login expects its redirect to carry.
    QString waitingState() const
    {
        const auto *waiting = std::get_if<OAuthManager::Waiting>(&m.m_browser);
        return waiting ? waiting->state : QString();
    }

private:
    OAuthManager &m;
};
