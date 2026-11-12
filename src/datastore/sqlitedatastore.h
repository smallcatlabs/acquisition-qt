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
    void Set(const QString &key, const QString &value);
    QString Get(const QString &key, const QString &default_value = "");
    bool InsertCurrencyUpdate(const CurrencyUpdate &update);
    std::vector<CurrencyUpdate> GetAllCurrency();
    static QString MakeFilename(const QString &name, const QString &league);

private:
    void CreateTable(const QString &username, const QString &fields);

    QString m_filename;

    QString getThreadLocalConnectionName() const;
    QSqlDatabase getThreadLocalDatabase();
    mutable QMutex m_mutex;
    mutable QSet<QString> m_connection_names;
};
