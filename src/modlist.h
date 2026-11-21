// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringListModel>
#include <QStringView>

#include <memory>
#include <unordered_map>
#include <vector>

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

// Whether the game data, or the pseudo-mods, give this mod name.
bool IsModName(const QString &name);

// Adds the items' own-text names (Item::own_text_mods) to the list of names
// the Mods filter offers, each once and in its place in the order, as
// inserted rows: a reset would clear the text of every mod row (#429).
// Names stay in the list until the game data is loaded again. Main thread
// only: the list is a model the search form's widgets read.
void AddOwnTextModNames(const std::vector<std::shared_ptr<Item>> &items);
