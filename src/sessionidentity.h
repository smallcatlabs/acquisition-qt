// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

#include <optional>
#include <utility>

// Who a session runs as: decided once, at login, and passed down by value
// (#299). Nothing below the login decision reads these from the settings.
//
// Only Make() builds one, and it refuses an empty field, so an identity
// that exists is complete.
class SessionIdentity
{
public:
    static std::optional<SessionIdentity> Make(QString account, QString realm, QString league)
    {
        if (account.isEmpty() || realm.isEmpty() || league.isEmpty()) {
            return std::nullopt;
        }
        return SessionIdentity(std::move(account), std::move(realm), std::move(league));
    }

    const QString &account() const { return m_account; }
    const QString &realm() const { return m_realm; }
    const QString &league() const { return m_league; }

private:
    SessionIdentity(QString account, QString realm, QString league)
        : m_account(std::move(account))
        , m_realm(std::move(realm))
        , m_league(std::move(league))
    {}

    QString m_account;
    QString m_realm;
    QString m_league;
};
