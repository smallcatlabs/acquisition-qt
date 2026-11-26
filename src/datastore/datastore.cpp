// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "datastore/datastore.h"

QString DataStore::Get(const QString &key, const QString &default_value)
{
    const auto value = Read(key);
    return (value && value->has_value()) ? **value : default_value;
}

StoredSettings::StoredSettings(DataStore &store, std::initializer_list<QString> keys)
    : m_store(store)
{
    // Every key is read even after a failure, so the caller can still show
    // what did read this session.
    for (const QString &key : keys) {
        const DataStore::ReadResult value = m_store.Read(key);
        if (!value && !m_error) {
            m_error = value.error();
        }
        m_values.insert(key, value ? value->value_or(QString()) : QString());
    }
}

bool StoredSettings::Set(const QString &key, const QString &value)
{
    Q_ASSERT(m_values.contains(key));
    return Loaded() && m_values.contains(key) && m_store.Set(key, value);
}
