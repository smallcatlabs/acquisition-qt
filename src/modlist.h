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

// This generates regular expressions for mods and does other setup, should be called when the app starts, perhaps in main()
// Maybe this is not needed and constexpr could do the trick, but VS doesn't support it right now.
void InitModList();

QStringListModel &mod_list_model();

class ModGenerator
{
public:
    virtual ~ModGenerator() {};
    virtual void Generate(const QString &json, ModTable &output) = 0;
};

class SumModGenerator : public ModGenerator
{
public:
    SumModGenerator(const QString &name, const std::vector<QString> &sum);
    virtual ~SumModGenerator() {};
    virtual void Generate(const QString &json, ModTable &output);

private:
    bool Match(const char *mod, double &output);

    QString m_name;
    std::vector<QString> m_matches;
};

typedef std::shared_ptr<SumModGenerator> SumModGen;

void InitStatTranslations();
void AddStatTranslations(const QByteArray &statTranslations);
void AddModToTable(const QString &mod, ModTable &output);
