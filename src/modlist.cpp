// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "modlist.h"

#include <QRegularExpression>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlRecord>

#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

#include "item.h"
#include "pseudomods.h"
#include "repoe/stattranslation.h"
#include "util/glaze_qt.h"
#include "util/spdlog_qt.h"
#include "util/util.h"

namespace {

    QStringListModel m_mod_list_model;
    std::set<QString> mods;
    std::unordered_map<QString, SumModGenerator *> mods_map;
    std::vector<SumModGen> mod_generators;

    const PseudoModManager pseudo_mgr;

} // namespace

QStringListModel &mod_list_model()
{
    return m_mod_list_model;
}

void InitStatTranslations()
{
    spdlog::trace("InitStatTranslations() entered");
    mods.clear();

    // Seed the mods list with pseudo mods.
    for (const auto &[pseudo_mod, real_mods] : pseudo_mgr.SUMMING_MODS) {
        mods.insert(pseudo_mod);
    }
}

void AddStatTranslations(const QByteArray &statTranslations)
{
    spdlog::trace("AddStatTranslations() entered");

    std::vector<repoe::StatTranslation> translations;

    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const std::string_view sv{statTranslations.constData(), size_t(statTranslations.size())};
    const auto ec = glz::read<permissive>(translations, sv);
    if (ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Unable to parse RePoE stat translations: {}", msg);
        return;
    }

    for (auto &translation : translations) {
        for (auto &stat : translation.English) {
            if (stat.is_markup.value_or(false)) {
                // This was added with the change to process json files inside
                // the stat_translations directory. In this case, the necropolis
                // mods from 3.24 have some kind of duplicate formatting with
                // markup that acquisition has not had to deal with before.
                //
                // It's possible this is true for other files in the stat_translations
                // folder, but acquisition has never needed to load modifiers from those
                // files before.
                continue;
            }
            QString stat_string = stat.string;
            // A stat whose format is "ignore" is not displayed and has no
            // placeholder in the string; every other stat's placeholder is
            // replaced, whichever stat comes first (#265).
            for (size_t i = 0; i < stat.format.size(); i++) {
                if (stat.format[i] != QLatin1String("ignore")) {
                    stat_string.replace(QStringLiteral("{%1}").arg(i), stat.format[i]);
                }
            }
            if (stat_string.length() > 0) {
                mods.insert(stat_string);
            }
        }
    }
}

void InitModList()
{
    spdlog::trace("InitModList() entered");

    std::set<QString> mod_strings;
    mod_generators.clear();
    mods_map.clear();
    for (auto &mod : mods) {
        if (mod_strings.count(mod) > 0) {
            spdlog::warn("InitModList(): duplicate mod: {}", mod);
        } else {
            mod_strings.insert(mod);
            std::vector<QString> list = {mod};
            SumModGen gen = std::make_shared<SumModGenerator>(mod, list);
            mods_map[mod] = gen.get();
            mod_generators.push_back(gen);
        }
    }
    QStringList mod_list;
    mod_list.reserve(mod_strings.size());
    for (auto &mod : mod_strings) {
        mod_list.append(mod);
    }
    mod_list.sort(Qt::CaseInsensitive);
    m_mod_list_model.setStringList(mod_list);
}

SumModGenerator::SumModGenerator(const QString &name, const std::vector<QString> &matches)
    : m_name(name)
    , m_matches(matches)
{}

bool SumModGenerator::Match(const char *mod, double &output)
{
    bool found = false;
    output = 0.0;
    for (auto &match : m_matches) {
        double result = 0.0;
        if (Util::MatchMod(match.toStdString().c_str(), mod, &result)) {
            output += result;
            found = true;
        }
    }
    return found;
}

void SumModGenerator::Generate(const QString &mod, ModTable &output)
{
    double result;
    if (Match(mod.toStdString().c_str(), result)) {
        // The same mod can appear in several sections (implicit and explicit,
        // say); the item's value is the sum of its lines (#262).
        output[m_name] += result;
    }
}

void AddModToTable(const QString &raw_mod, ModTable &output)
{
    QString generic_mod = raw_mod;

    if (generic_mod.startsWith("1 Added Passive Skill")) {
        // Skip modifiers that appear to be cluster jewel notables.
        // This is such a terrible hack. The entire mods stuff needs to be completely redone.
    } else {
        static const QRegularExpression rep("([0-9\\.]+)");
        generic_mod.replace(rep, "#");
    }

    const std::string raw_str = raw_mod.toStdString();

    // First, process natural modifiers.
    {
        auto it = mods_map.find(generic_mod);
        if (it != mods_map.end()) {
            SumModGenerator *gen = it->second;
            gen->Generate(raw_mod, output);
        }
    }

    // Next, process summing pseudo-mods.
    {
        auto it = pseudo_mgr.SUMMING_MODS_LOOKUP.find(generic_mod);
        if (it != pseudo_mgr.SUMMING_MODS_LOOKUP.end()) {
            const std::string generic_str = generic_mod.toStdString();

            // Get the list of summing pseudo-mods this modifier impacts.
            const std::vector<QString> &pseudo_mods = it->second;

            // Process each summing pseudo-mod.
            for (const QString &pseudo_mod : pseudo_mods) {
                double value;
                Util::MatchMod(generic_str.c_str(), raw_str.c_str(), &value);
                if (output.contains(pseudo_mod)) {
                    output[pseudo_mod] += value;
                } else {
                    output[pseudo_mod] = value;
                }
            }
        }
    }
}
