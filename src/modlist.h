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
// Reads the support gems' names from the base items (itemcategories.h), so
// the base items are loaded first, as RePoE::FinishUpdate does (#419).
void AddStatTranslations(const QByteArray &statTranslations);

// What becomes of a line that fits no mod name of the game data (#429).
// Filed under its own text, each number is written "#" (a sign before it
// stays in the text) and the value is the mean of the numbers, 0 if there
// are none. Item::unnamed_lines says which items' lines are filed.
enum class UnnamedLine { Dropped, FiledUnderItsText };

void AddModToTable(const QString &mod, ModTable &output, UnnamedLine unnamed = UnnamedLine::Dropped);
