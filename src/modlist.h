// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringListModel>
#include <QStringView>

#include <unordered_map>

class Item;
typedef std::unordered_map<QString, double> ModTable;

// Builds the mod lookup and the mods filter's list from the translations
// added since InitStatTranslations().
void InitModList();

QStringListModel &mod_list_model();

void InitStatTranslations();
void AddStatTranslations(const QByteArray &statTranslations);
void AddModToTable(const QString &mod, ModTable &output);
