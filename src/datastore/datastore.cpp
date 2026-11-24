// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "datastore/datastore.h"

QString DataStore::Get(const QString &key, const QString &default_value)
{
    const auto value = Read(key);
    return (value && value->has_value()) ? **value : default_value;
}

void DataStore::SetInt(const QString &key, int value)
{
    Set(key, QString::number(value));
}

int DataStore::GetInt(const QString &key, int default_value)
{
    return Get(key, QString::number(default_value)).toInt();
}
