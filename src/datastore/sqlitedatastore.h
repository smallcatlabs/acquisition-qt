// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QMutex>
#include <QSet>
#include <QSqlDatabase>

#include "datastore/datastore.h"

class Application;

class SqliteDataStore : public DataStore
{
public:
    SqliteDataStore(const QString &m_filename);
    ~SqliteDataStore();
    bool Set(const QString &key, const QString &value) override;
    ReadResult Read(const QString &key) override;
    bool InsertCurrencyUpdate(const CurrencyUpdate &update) override;
    std::vector<CurrencyUpdate> GetAllCurrency() override;
    static QString MakeFilename(const QString &name, const QString &league);

private:
    void CreateTable(const QString &username, const QString &fields);

    QString m_filename;

    QString getThreadLocalConnectionName() const;
    QSqlDatabase getThreadLocalDatabase();
    mutable QMutex m_mutex;
    mutable QSet<QString> m_connection_names;
};
