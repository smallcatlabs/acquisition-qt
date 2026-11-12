// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QDir>
#include <QObject>
#include <QSqlDatabase>
#include <QString>

#include <memory>
#include <optional>

class BuyoutRepo;
class CharacterRepo;
class StashRepo;

class UserStore : public QObject
{
    Q_OBJECT
public:
    explicit UserStore(const QDir &dir, const QString &username);
    ~UserStore();

    StashRepo &stashes() { return *m_stashes; }
    CharacterRepo &characters() { return *m_characters; }
    BuyoutRepo &buyouts() { return *m_buyouts; }

    // True when the database opened, its user_version was readable and not
    // newer than this build's schema, and any migration committed (#250,
    // #257). Pushed to the buyout repo, which is what BuyoutManager holds.
    bool isUsable() const { return m_usable; }

private:
    bool open(const QDir &dir, const QString &username);
    std::optional<int> userVersion();
    bool migrate();

    QSqlDatabase m_db;
    bool m_usable{false};
    std::unique_ptr<StashRepo> m_stashes;
    std::unique_ptr<CharacterRepo> m_characters;
    std::unique_ptr<BuyoutRepo> m_buyouts;
};
