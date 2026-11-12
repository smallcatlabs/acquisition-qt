// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Mod-match census (#265, #296): not a test, run by hand. It loads RePoE
// stat translation files given on the command line through the
// application's own code, renders every translation the way the game
// prints it, feeds each line to AddModToTable, and reports which mod names
// the mods filter offers that no line can reach, by cause.
//
//   modmatch_census <out.tsv> <stat_translations.min.json> [more files...]
//
// The TSV lists, for every rendered line, the mod table entries it
// produced, so two builds can be compared line by line.

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include "modlist.h"
#include "pseudomods.h"
#include "repoe/stattranslation.h"
#include "util/glaze_qt.h" // IWYU pragma: keep

namespace {

    // The name the mods filter offers for an entry: each displayed stat's
    // placeholder replaced by its format.
    QString nameOf(const repoe::EnglishTranslation &entry)
    {
        QString name = entry.string;
        for (size_t i = 0; i < entry.format.size(); ++i) {
            if (entry.format[i] != QLatin1String("ignore")) {
                name.replace(QStringLiteral("{%1}").arg(i), entry.format[i]);
            }
        }
        return name;
    }

    // The line the game prints for an entry when every displayed stat has
    // the value `value`: "#" prints the number, "+#" prints it with its sign.
    QString lineOf(const repoe::EnglishTranslation &entry, int value)
    {
        QString line = entry.string;
        for (size_t i = 0; i < entry.format.size(); ++i) {
            const QString &format = entry.format[i];
            QString number;
            if (format == QLatin1String("#")) {
                number = QString::number(value);
            } else if (format == QLatin1String("+#")) {
                number = (value < 0 ? "" : "+") + QString::number(value);
            } else {
                continue;
            }
            line.replace(QStringLiteral("{%1}").arg(i), number);
        }
        return line;
    }

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: modmatch_census <out.tsv> <stat_translations.json>...\n");
        return 2;
    }

    std::vector<repoe::EnglishTranslation> entries;
    InitStatTranslations();
    for (int i = 2; i < argc; ++i) {
        QFile file(QString::fromLocal8Bit(argv[i]));
        if (!file.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        const QByteArray data = file.readAll();
        AddStatTranslations(data);

        std::vector<repoe::StatTranslation> translations;
        constexpr glz::opts permissive{.error_on_unknown_keys = false};
        if (glz::read<permissive>(translations, std::string_view(data.constData(), data.size()))) {
            std::fprintf(stderr, "cannot parse %s\n", argv[i]);
            return 2;
        }
        for (const auto &translation : translations) {
            for (const auto &entry : translation.English) {
                if (!entry.is_markup.value_or(false)) {
                    entries.push_back(entry);
                }
            }
        }
    }
    InitModList();

    std::set<QString> pseudo;
    for (const auto &[name, sources] : PseudoModManager::SUMMING_MODS) {
        pseudo.insert(name);
    }
    std::set<QString> offered;
    for (const QString &name : mod_list_model().stringList()) {
        if (!pseudo.contains(name)) {
            offered.insert(name);
        }
    }

    QFile out(QString::fromLocal8Bit(argv[1]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 2;
    }
    QTextStream tsv(&out);

    std::set<QString> reached;
    int signedEntries = 0;
    int negativeReached = 0;
    for (const auto &entry : entries) {
        const QString name = nameOf(entry);
        if (name.isEmpty()) {
            continue;
        }
        const bool isSigned = entry.format.size() > 0
                              && std::find(entry.format.begin(), entry.format.end(), "+#")
                                     != entry.format.end();
        for (const int value : {7, -7}) {
            if (value < 0 && !isSigned) {
                continue;
            }
            const QString line = lineOf(entry, value);
            ModTable table;
            AddModToTable(line, table);
            const std::map<QString, double> sorted(table.begin(), table.end());
            tsv << QString(line).replace('\n', "\\n");
            for (const auto &[key, number] : sorted) {
                tsv << '\t' << QString(key).replace('\n', "\\n") << '=' << number;
            }
            tsv << '\n';

            const auto found = table.find(name);
            const bool hit = (found != table.end());
            if (value > 0 && hit) {
                reached.insert(name);
            }
            if (value < 0) {
                ++signedEntries;
                if (hit && found->second == double(value)) {
                    ++negativeReached;
                }
            }
        }
    }

    static const QRegularExpression literalDigit("[0-9]");
    static const QRegularExpression placeholder(R"(\{[0-9]+\})");
    int unreachable = 0;
    int literalDigits = 0;
    int literalPlaceholders = 0;
    int other = 0;
    QStringList others;
    for (const QString &name : offered) {
        if (reached.contains(name)) {
            continue;
        }
        ++unreachable;
        if (name.contains(placeholder)) {
            ++literalPlaceholders;
        } else if (name.contains(literalDigit)) {
            ++literalDigits;
        } else {
            ++other;
            others.append(name);
        }
    }

    std::printf("translation entries rendered: %zu\n", entries.size());
    std::printf("mod names offered (without pseudo-mods): %zu\n", offered.size());
    std::printf("names no rendered line reaches: %d\n", unreachable);
    std::printf("  with a literal {n}: %d\n", literalPlaceholders);
    std::printf("  with literal digits: %d\n", literalDigits);
    std::printf("  other: %d\n", other);
    for (const QString &name : others) {
        std::printf("    %s\n", qPrintable(QString(name).replace('\n', "\\n")));
    }
    std::printf("negative rolls of +# entries: %d, reaching their name with a negative value: %d\n",
                signedEntries,
                negativeReached);
    return 0;
}
