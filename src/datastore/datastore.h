// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QString>

#include <vector>

// For now we just serialize/deserialize 'value' inside CurrencyManager
// Later we might need more logic if GGG adds more currency types and we want to be backwards compatible
struct CurrencyUpdate
{
    long long timestamp{0};
    QString value;
};

class DataStore
{
public:
    virtual ~DataStore() {};
    virtual void Set(const QString &key, const QString &value) = 0;
    virtual QString Get(const QString &key, const QString &default_value = "") = 0;
    // False when the update was not written.
    virtual bool InsertCurrencyUpdate(const CurrencyUpdate &update) = 0;
    virtual std::vector<CurrencyUpdate> GetAllCurrency() = 0;
    void SetInt(const QString &key, int value);
    int GetInt(const QString &key, int default_value = 0);
};
