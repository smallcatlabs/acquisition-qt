// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QString>

#include <expected>
#include <optional>
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
    // A stored value, nothing when the key holds none, or why the read
    // failed.
    using ReadResult = std::expected<std::optional<QString>, QString>;

    virtual ~DataStore() {};
    // False when the value was not written.
    virtual bool Set(const QString &key, const QString &value) = 0;
    // A failed read is not an empty store (#295): a caller that writes back
    // what it read must not write after one.
    virtual ReadResult Read(const QString &key) = 0;
    // The stored value, or `default_value` when there is none OR the read
    // failed. Only for a caller that never writes back what it read.
    QString Get(const QString &key, const QString &default_value = "");
    // False when the update was not written.
    virtual bool InsertCurrencyUpdate(const CurrencyUpdate &update) = 0;
    virtual std::vector<CurrencyUpdate> GetAllCurrency() = 0;
    void SetInt(const QString &key, int value);
    int GetInt(const QString &key, int default_value = 0);
};
