// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <optional>
#include <vector>

#include "poe/types/stashtab.h"

class QSqlDatabase;
class QString;

class StashRepo : public QObject
{
    Q_OBJECT
public:
    explicit StashRepo(QSqlDatabase &db);

    std::optional<poe::StashTab> getStash(const QString &id,
                                          const QString &realm,
                                          const QString &league);
    std::vector<poe::StashTab> getStashList(const QString &realm,
                                            const QString &league,
                                            const std::optional<QString> type = {});

    std::vector<poe::StashTab> getStashChildren(const QString &id,
                                                const QString &realm,
                                                const QString &league);

    bool resetRepo();
    bool ensureSchema();

public slots:
    // `bytes` is the exact wire JSON of the reply's stash sub-object (F62):
    // it is stored as-is in json_data, never a re-serialization of `stash`,
    // so the cache keeps every field the poe:: types do not model. The typed
    // `stash` fills the queryable columns.
    bool saveStash(const poe::StashTab &stash,
                   const QByteArray &bytes,
                   const QString &realm,
                   const QString &league);
    bool saveStashList(const std::vector<poe::StashTab> &stashes,
                       const QString &realm,
                       const QString &league);
    bool reconcileStashList(const std::vector<poe::StashTab> &stashes,
                            const QString &realm,
                            const QString &league);
    bool reconcileStashChildren(const QString &parent_id,
                                const QStringList &child_ids,
                                const QString &realm,
                                const QString &league);

private:
    QSqlDatabase &m_db;
};
