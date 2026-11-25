// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QHash>
#include <QString>

#include <expected>
#include <initializer_list>
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
    virtual ~DataStore() {};
    // False when the value was not written. For a key whose new value never
    // derives from what was read; StoredSettings writes the others.
    virtual bool Set(const QString &key, const QString &value) = 0;
    // The stored value, or `default_value` when there is none OR the read
    // failed. Only for a caller that never writes back what it read.
    QString Get(const QString &key, const QString &default_value = "");
    // False when the update was not written.
    virtual bool InsertCurrencyUpdate(const CurrencyUpdate &update) = 0;
    virtual std::vector<CurrencyUpdate> GetAllCurrency() = 0;

protected:
    // A stored value, nothing when the key holds none, or why the read
    // failed.
    using ReadResult = std::expected<std::optional<QString>, QString>;
    // A failed read is not an empty store (#295). Not public, so a caller
    // that writes back what it read reads through StoredSettings, which
    // writes nothing after one.
    virtual ReadResult Read(const QString &key) = 0;

private:
    friend class StoredSettings;
};

// The keys a caller reads and writes back as they were read (#458), such as
// the shop's threads and template. They are read together at construction,
// and when any read fails none of them is written this session, so what
// could not be read is never saved over. A key whose new value does not
// derive from what was read (oauth_token, shop_hash, version) is written
// with DataStore::Set instead: a sign-out must clear the token even when the
// token could not be read.
class StoredSettings
{
public:
    StoredSettings(DataStore &store, std::initializer_list<QString> keys);
    // True when every key was read.
    bool Loaded() const { return !m_error; }
    // Why the first read that failed did, empty when every read succeeded.
    QString error() const { return m_error.value_or(QString()); }
    // The stored value, empty when the key holds none or was not read.
    QString value(const QString &key) const { return m_values.value(key); }
    // Writes nothing and returns false when not loaded or the key is not one
    // of these; otherwise the store's result.
    bool Set(const QString &key, const QString &value);

private:
    DataStore &m_store;
    QHash<QString, QString> m_values;
    std::optional<QString> m_error;
};
