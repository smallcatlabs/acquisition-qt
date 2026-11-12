// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>
#include <QObject>

#include <optional>
#include <vector>

#include "poe/types/character.h"

class QSqlDatabase;
class QString;

class CharacterRepo : public QObject
{
    Q_OBJECT
public:
    explicit CharacterRepo(QSqlDatabase &db);

    std::optional<poe::Character> getCharacter(const QString &name, const QString &realm);
    std::vector<poe::Character> getCharacterList(const QString &realm,
                                                 const std::optional<QString> league = {});

    bool resetRepo();
    bool ensureSchema();

public slots:
    // `bytes` is the exact wire JSON of the reply's character sub-object
    // (F62): stored as-is in json_data, never a re-serialization of
    // `character` — see StashRepo::saveStash.
    bool saveCharacter(const poe::Character &character, const QByteArray &bytes);
    bool saveCharacterList(const std::vector<poe::Character> &characters);
    bool reconcileCharacterList(const std::vector<poe::Character> &characters, const QString &realm);

private:
    QSqlDatabase &m_db;
};
