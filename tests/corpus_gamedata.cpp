// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// The game data the payload corpus needs (#246): not a test, run by hand.
//
//   corpus_gamedata <repoe-dir> <payloads-dir> <out-dir>
//
// <repoe-dir> is a directory of the RePoE files the application downloads,
// laid out as the application keeps them (the `repoe` directory inside a data
// directory). The tool builds every item of the corpus with that game data,
// then writes to <out-dir> the entries those items use, in the same layout:
// the base items the items are named after, and every stat translation
// that gives a mod name in their mod tables or whose text is the text of one
// of their mod lines once numbers and signs are left out of both and the
// game's [id|text] markup is read as its text. The second
// rule keeps the translations a line nearly matches, so that a change to the
// matching which lets a line reach a different translation shows on the
// excerpt too. Entries are copied byte for byte.
//
// It then loads <out-dir> in place of the whole game data, builds the items
// again and compares what they become, line by line. It exits with 1 if the
// excerpt gives a different result, so tst_golden, which runs on the
// excerpt, pins what the whole game data gives.
//
// It prints the corpus census (corpusdump.h) for the whole game data.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "corpusdump.h"
#include "repoe/baseitem.h"
#include "util/glaze_qt.h" // IWYU pragma: keep

namespace {

    constexpr glz::opts permissive{.error_on_unknown_keys = false};

    void writeFile(const QString &path, const QByteArray &data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || (file.write(data) != data.size())) {
            qFatal("cannot write %s", qPrintable(path));
        }
    }

    // The names a base type is looked up by (Item::CalculateCategories): the
    // name after any <<set:...>> marker, and for a transfigured gem also the
    // name before " of ".
    void addBaseNames(const poe::Item &item, std::set<QString> &names)
    {
        QString name = item.baseType;
        const auto marker = name.lastIndexOf(">>");
        if (marker >= 0) {
            name = name.sliced(marker + 2);
        }
        names.insert(name);
        const auto of = name.indexOf(" of ");
        if (of >= 0) {
            names.insert(name.first(of));
        }
        if (item.socketedItems) {
            for (const poe::Item &socketed : *item.socketedItems) {
                addBaseNames(socketed, names);
            }
        }
    }

    void addBaseNames(const std::optional<std::vector<poe::Item>> &items, std::set<QString> &names)
    {
        if (items) {
            for (const poe::Item &item : *items) {
                addBaseNames(item, names);
            }
        }
    }

    // A mod line or a mod name with its numbers, placeholders and their
    // signs left out, and the game's [id|text] markup read as its text: what
    // the two have in common whatever the matching rule.
    QString skeleton(QString text)
    {
        static const QRegularExpression markup(R"(\[[^\]|]*\|([^\]]*)\])");
        static const QRegularExpression value(R"([+-]?(?:#|[0-9]+(?:\.[0-9]+)?))");
        return text.replace(markup, R"(\1)").remove(value).simplified();
    }

    struct Corpus
    {
        QStringList dump;
        corpus::Census census;
        std::set<QString> mod_names;
        std::set<QString> mod_line_skeletons;
        std::set<QString> base_names;
    };

    // Build every item of the corpus with the game data now loaded.
    Corpus readCorpus(const QDir &payloads)
    {
        Corpus result;
        for (const QString &name : corpus::payloadFiles(payloads)) {
            const QByteArray json = corpus::readFile(payloads.filePath(name));
            const Items items = corpus::itemsOf(name, json);
            result.dump << "##### " + name << corpus::dumpPayload(items);
            result.census.add(items);
            for (const auto &item : items) {
                for (const auto &[mod, value] : item->mod_table()) {
                    result.mod_names.insert(mod);
                }
                for (const auto &[section, lines] : item->text_mods()) {
                    for (const QString &line : lines) {
                        result.mod_line_skeletons.insert(skeleton(line));
                    }
                }
            }
            if (name.startsWith("character-")) {
                const auto character = json::readCharacter(json);
                addBaseNames(character->equipment, result.base_names);
                addBaseNames(character->inventory, result.base_names);
                addBaseNames(character->rucksack, result.base_names);
                addBaseNames(character->jewels, result.base_names);
            } else {
                addBaseNames(json::readStash(json)->items, result.base_names);
            }
        }
        return result;
    }

    // The mod names one stat translation entry gives, by the application's
    // own reading of it.
    std::set<QString> modNamesOf(const std::string &entry)
    {
        InitStatTranslations();
        AddStatTranslations(QByteArray("[") + QByteArray::fromStdString(entry) + "]");
        InitModList();
        const QStringList names = mod_list_model().stringList();
        return std::set<QString>(names.begin(), names.end());
    }

    // Cut the entries of one stat translation file that give a mod name
    // the corpus uses or nearly uses. Returns how many were kept.
    size_t cutTranslations(const QString &from,
                           const QString &to,
                           const Corpus &corpus,
                           const std::set<QString> &pseudo)
    {
        const std::string text = corpus::readFile(from).toStdString();
        std::vector<glz::raw_json> entries;
        if (const auto error = glz::read<permissive>(entries, text)) {
            qFatal("cannot parse %s: %s", qPrintable(from), glz::format_error(error, text).c_str());
        }
        QByteArray out = "[";
        size_t kept = 0;
        for (const glz::raw_json &entry : entries) {
            bool keep = false;
            for (const QString &name : modNamesOf(entry.str)) {
                if (pseudo.contains(name)) {
                    continue; // every entry "gives" the pseudo-mods
                }
                if (corpus.mod_names.contains(name)
                    || corpus.mod_line_skeletons.contains(skeleton(name))) {
                    keep = true;
                    break;
                }
            }
            if (keep) {
                out += (kept == 0 ? "\n" : ",\n") + QByteArray::fromStdString(entry.str);
                ++kept;
            }
        }
        out += "\n]\n";
        writeFile(to, out);
        return kept;
    }

    // Cut the base items with a wanted name: every entry of that name, so a
    // name the game data gives several item classes keeps them all and the
    // excerpt resolves it as the whole does (#390). Returns how many were kept.
    size_t cutBaseItems(const QString &from, const QString &to, const std::set<QString> &wanted)
    {
        const std::string text = corpus::readFile(from).toStdString();
        std::map<std::string, glz::raw_json> entries;
        if (const auto error = glz::read<permissive>(entries, text)) {
            qFatal("cannot parse %s: %s", qPrintable(from), glz::format_error(error, text).c_str());
        }
        QByteArray out = "{";
        size_t kept = 0;
        for (const auto &[key, entry] : entries) {
            repoe::BaseItem base;
            if (glz::read<permissive>(base, entry.str) || !wanted.contains(base.name)) {
                continue;
            }
            std::string quoted;
            (void) glz::write_json(key, quoted);
            out += (kept == 0 ? "\n" : ",\n") + QByteArray::fromStdString(quoted) + ":"
                   + QByteArray::fromStdString(entry.str);
            ++kept;
        }
        out += "\n}\n";
        writeFile(to, out);
        return kept;
    }

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "usage: corpus_gamedata <repoe-dir> <payloads-dir> <out-dir>\n");
        return 2;
    }
    const QDir repoe(QString::fromLocal8Bit(argv[1]));
    const QDir payloads(QString::fromLocal8Bit(argv[2]));
    const QDir out(QString::fromLocal8Bit(argv[3]));

    corpus::loadGameData(repoe);
    const Corpus whole = readCorpus(payloads);

    std::set<QString> pseudo;
    for (const auto &[name, sources] : PseudoModManager::SUMMING_MODS) {
        pseudo.insert(name);
    }

    const size_t base_items = cutBaseItems(repoe.filePath("base_items.min.json"),
                                           out.filePath("base_items.min.json"),
                                           whole.base_names);
    const size_t translations = cutTranslations(repoe.filePath("stat_translations.min.json"),
                                                out.filePath("stat_translations.min.json"),
                                                whole,
                                                pseudo);
    const size_t necropolis = cutTranslations(repoe.filePath(
                                                  "stat_translations/necropolis.min.json"),
                                              out.filePath("stat_translations/necropolis.min.json"),
                                              whole,
                                              pseudo);
    writeFile(out.filePath("item_classes.min.json"),
              corpus::readFile(repoe.filePath("item_classes.min.json")));
    writeFile(out.filePath("version.txt"), corpus::readFile(repoe.filePath("version.txt")));

    corpus::loadGameData(out);
    const Corpus excerpt = readCorpus(payloads);

    std::printf("game data version: %s\n",
                corpus::readFile(repoe.filePath("version.txt")).trimmed().constData());
    std::printf("base items kept: %zu\n", base_items);
    std::printf("stat translations kept: %zu\n", translations);
    std::printf("necropolis stat translations kept: %zu\n", necropolis);
    std::printf("\ncensus with the whole game data:\n");
    for (const QString &line : whole.census.lines()) {
        std::printf("%s\n", qPrintable(line));
    }

    if (whole.dump == excerpt.dump) {
        std::printf("\nthe excerpt gives the same %lld lines as the whole game data\n",
                    static_cast<long long>(whole.dump.size()));
        return 0;
    }
    std::printf("\nTHE EXCERPT GIVES A DIFFERENT RESULT\n");
    int shown = 0;
    for (qsizetype i = 0; i < std::max(whole.dump.size(), excerpt.dump.size()); ++i) {
        const QString a = whole.dump.value(i);
        const QString b = excerpt.dump.value(i);
        if (a != b) {
            std::printf("line %lld\n  whole:   %s\n  excerpt: %s\n",
                        static_cast<long long>(i + 1),
                        qPrintable(a),
                        qPrintable(b));
            if (++shown == 20) {
                break;
            }
        }
    }
    return 1;
}
