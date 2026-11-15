// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

// What the application makes of the real replies in tests/fixtures/payloads
// (#246), written out as text: each item as the application builds it, its
// tooltip, its Path of Building text and the forum shop that lists it.
// tst_golden compares this with the files in tests/fixtures/payloads-golden;
// corpus_gamedata uses it to show that the game data excerpt in
// tests/fixtures/payloads-repoe gives the same result as the whole game data.
//
// Everything here is ordered and formatted the same on every platform:
// unordered tables are sorted and numbers are written with ten significant
// digits.

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "buyout.h"
#include "currency.h"
#include "item.h"
#include "itemcategories.h"
#include "itemlocation.h"
#include "itemsfromwire.h"
#include "modlist.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "pseudomods.h"
#include "shoprender.h"
#include "ui/itemtooltiptext.h"
#include "util/json_readers.h"

namespace corpus {

    // The realm and league the forum shop is rendered for. The payload files
    // do not say which league they came from, and nothing else here reads it.
    inline constexpr const char *kRealm = "pc";
    inline constexpr const char *kLeague = "Corpus";

    inline QStringList payloadFiles(const QDir &dir)
    {
        return dir.entryList({"stash-*.json", "character-*.json"}, QDir::Files, QDir::Name);
    }

    inline QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("cannot read %s", qPrintable(path));
        }
        return file.readAll();
    }

    // Load the game data in `dir`, which is laid out like the application's
    // own repoe directory, the way RePoE::FinishUpdate does.
    inline void loadGameData(const QDir &dir)
    {
        InitItemClasses(readFile(dir.filePath("item_classes.min.json")));
        InitItemBaseTypes(readFile(dir.filePath("base_items.min.json")));
        InitStatTranslations();
        AddStatTranslations(readFile(dir.filePath("stat_translations.min.json")));
        AddStatTranslations(readFile(dir.filePath("stat_translations/necropolis.min.json")));
        InitModList();
    }

    // The items of one payload file, by the walk the cache load and a refresh
    // share. A stash's items are placed in that stash, also where the
    // application would show them under a parent tab.
    inline Items itemsOf(const QString &file_name, const QByteArray &json)
    {
        Items items;
        if (file_name.startsWith("character-")) {
            const auto character = json::readCharacter(json);
            if (!character) {
                qFatal("cannot parse %s", qPrintable(file_name));
            }
            AppendWireItems(*character, ItemLocation(*character), items);
        } else {
            const auto stash = json::readStash(json);
            if (!stash) {
                qFatal("cannot parse %s", qPrintable(file_name));
            }
            AppendWireItems(*stash, ItemLocation(*stash), items);
        }
        return items;
    }

    inline QString number(double value)
    {
        return QString::number(value, 'g', 10);
    }

    // A text that may hold line breaks, on one line. The game ends a line
    // inside a mod with "\r\n".
    inline QString oneLine(QString text)
    {
        return text.replace('\r', "\\r").replace('\n', "\\n");
    }

    inline QString influenceName(Item::INFLUENCE_TYPES type)
    {
        switch (type) {
        case Item::NONE:
            return "none";
        case Item::SHAPER:
            return "shaper";
        case Item::ELDER:
            return "elder";
        case Item::CRUSADER:
            return "crusader";
        case Item::REDEEMER:
            return "redeemer";
        case Item::HUNTER:
            return "hunter";
        case Item::WARLORD:
            return "warlord";
        case Item::SYNTHESISED:
            return "synthesised";
        case Item::FRACTURED:
            return "fractured";
        case Item::SEARING_EXARCH:
            return "searing exarch";
        case Item::EATER_OF_WORLDS:
            return "eater of worlds";
        }
        return "?";
    }

    inline QChar socketLetter(SocketKind kind)
    {
        switch (kind) {
        case SocketKind::Red:
            return 'R';
        case SocketKind::Green:
            return 'G';
        case SocketKind::Blue:
            return 'B';
        case SocketKind::White:
            return 'W';
        case SocketKind::Abyss:
            return 'A';
        case SocketKind::Resonator:
            return 'D';
        case SocketKind::Unknown:
            return '?';
        }
        return '?';
    }

    // The forum code that links this item in a shop.
    inline QString forumCode(const Item &item)
    {
        return item.location().GetForumCode(kRealm, kLeague, 0);
    }

    // One item, as the application built it.
    inline QStringList dumpItem(const Item &item)
    {
        QStringList out;
        out << "=== " + oneLine(item.PrettyName());
        out << "id: " + item.id();
        out << "location: " + item.location().GetHeader()
                   + (item.location().socketed() ? " (socketed)" : "");
        out << "forum code: " + forumCode(item);
        out << QString("frame type %1, category '%2', item level %3, %4x%5, count %6")
                   .arg(item.frameType())
                   .arg(item.category())
                   .arg(item.ilvl())
                   .arg(item.w())
                   .arg(item.h())
                   .arg(item.count());

        QStringList flags;
        const auto flag = [&flags](bool set, const char *name) {
            if (set) {
                flags << name;
            }
        };
        flag(item.identified(), "identified");
        flag(item.corrupted(), "corrupted");
        flag(item.crafted(), "crafted");
        flag(item.enchanted(), "enchanted");
        flag(item.fractured(), "fractured");
        flag(item.split(), "split");
        flag(item.synthesized(), "synthesized");
        flag(item.mutated(), "mutated");
        flag(item.Wearable(), "path of building item");
        out << "flags: " + flags.join(", ");

        if (item.hasInfluence()) {
            out << "influence: " + influenceName(item.influenceLeft()) + ", "
                       + influenceName(item.influenceRight());
        }
        if (!item.note().isEmpty()) {
            out << "note: " + oneLine(item.note())
                       + (item.HasNotePrice() ? " => " + item.note_price().AsText()
                                              : " => no price");
        }

        if (!item.text_sockets().empty()) {
            QString sockets;
            for (size_t i = 0; i < item.text_sockets().size(); ++i) {
                const ItemSocket &socket = item.text_sockets()[i];
                if (i > 0) {
                    sockets += (socket.group == item.text_sockets()[i - 1].group) ? "-" : " ";
                }
                sockets += socketLetter(socket.kind);
            }
            const ItemSocketGroup &all = item.sockets();
            out << QString("sockets: %1 (%2 sockets, %3 linked; r%4 g%5 b%6 w%7)")
                       .arg(sockets)
                       .arg(item.sockets_cnt())
                       .arg(item.links_cnt())
                       .arg(all.r)
                       .arg(all.g)
                       .arg(all.b)
                       .arg(all.w);
            QStringList groups;
            for (const ItemSocketGroup &group : item.socket_groups()) {
                groups << QString("r%1 g%2 b%3 w%4")
                              .arg(group.r)
                              .arg(group.g)
                              .arg(group.b)
                              .arg(group.w);
            }
            out << "socket groups: " + groups.join("; ");
        }

        if (!item.properties().empty()) {
            out << "properties:";
            for (const auto &[name, value] : item.properties()) {
                const auto numeric = item.numeric_properties().find(name);
                out << "  " + oneLine(name) + " = " + oneLine(value)
                           + (numeric != item.numeric_properties().end()
                                  ? " (number " + number(numeric->second) + ")"
                                  : QString());
            }
        }
        // A number stored under a name that is not a property of its own: a
        // map's tier, a catalyst's quality counted as Quality.
        for (const auto &[name, value] : item.numeric_properties()) {
            if (!item.properties().contains(name)) {
                out << "number without a property: " + oneLine(name) + " = " + number(value);
            }
        }
        if (!item.elemental_damage().empty()) {
            QStringList parts;
            for (const auto &[damage, type] : item.elemental_damage()) {
                parts << QString("%1 (type %2)").arg(damage).arg(type);
            }
            out << "elemental damage: " + parts.join(", ");
        }
        if (item.DPS() != 0.0) {
            out << QString("dps: %1 (physical %2, elemental %3, chaos %4)")
                       .arg(number(item.DPS()),
                            number(item.pDPS()),
                            number(item.eDPS()),
                            number(item.cDPS()));
        }
        if (!item.requirements().empty()) {
            QStringList parts;
            for (const auto &[name, value] : item.requirements()) {
                parts << QString("%1 %2").arg(name).arg(value);
            }
            out << "requirements: " + parts.join(", ");
        }

        if (!item.mod_table().empty()) {
            out << "mod table:";
            const std::map<QString, double> sorted(item.mod_table().begin(), item.mod_table().end());
            for (const auto &[name, value] : sorted) {
                out << "  " + oneLine(name) + " = " + number(value);
            }
        }

        if (item.Wearable()) {
            out << "path of building:";
            for (const QString &line : item.POBformat().split('\n')) {
                out << "  " + oneLine(line);
            }
        }

        // The tooltip's text, one line for each line break it asks for.
        QString tooltip = oneLine(GenerateItemInfo(item, QString(), false));
        tooltip.replace("<br>", "<br>\n");
        tooltip.replace("<hr>", "<hr>\n");
        out << "tooltip:";
        for (const QString &line : tooltip.split('\n')) {
            out << "  " + line;
        }

        out << "hash: " + item.hash_v4();
        return out;
    }

    // The forum shop that lists every item of a payload at one price, with
    // a line break before each item's link.
    inline QStringList dumpShop(const Items &items)
    {
        ShopRenderInput input;
        input.shop_template = kShopTemplateItems;
        input.realm = kRealm;
        input.league = kLeague;
        const Buyout price(1.0,
                           Buyout::BUYOUT_TYPE_BUYOUT,
                           Currency::CURRENCY_CHAOS_ORB,
                           QDateTime::fromSecsSinceEpoch(1, QTimeZone::UTC));
        for (const auto &item : items) {
            input.items.push_back({item->PrettyName(), item->location(), price});
            input.tab_index[item->location().id()] = 0;
        }
        const RenderedShop shop = RenderShop(input);

        QStringList out;
        out << QString("=== forum shop with every item at ~b/o 1 chaos: %1 page(s), %2 unlinked")
                   .arg(shop.pages.size())
                   .arg(shop.unlinked.size());
        for (QString page : shop.pages) {
            page.replace("[linkItem", "\n  [linkItem");
            out << page.split('\n');
        }
        return out;
    }

    // What one payload file becomes.
    inline QStringList dumpPayload(const Items &items)
    {
        QStringList out;
        out << QString("%1 item(s)").arg(items.size());
        for (const auto &item : items) {
            out << QString() << dumpItem(*item);
        }
        out << QString() << dumpShop(items);
        return out;
    }

    // What the corpus as a whole shows about the game data and the reading
    // of items: a change in the game's files, or in the code that reads
    // them, moves one of these numbers.
    class Census
    {
    public:
        void add(const Items &items)
        {
            for (const auto &item : items) {
                ++m_items;
                if (item->category().isEmpty()) {
                    m_uncategorised.insert(item->typeLine());
                    ++m_uncategorised_items;
                }
                for (const auto &[section, lines] : item->text_mods()) {
                    if (section == QLatin1String("vestigialMods")) {
                        // Shown and not counted (Item::LoadModifiers).
                        continue;
                    }
                    for (const QString &line : lines) {
                        ++m_mod_lines;
                        ModTable table;
                        AddModToTable(line, table);
                        if (table.empty()) {
                            ++m_unmatched_lines;
                            m_unmatched.insert(oneLine(line));
                        }
                    }
                }
                for (const auto &[name, value] : item->properties()) {
                    ++m_properties;
                    if (item->numeric_properties().contains(name)) {
                        m_numeric_names.insert(name);
                    } else {
                        ++m_text_properties;
                        m_text_names.insert(name);
                    }
                }
            }
        }

        QStringList lines() const
        {
            QStringList out;
            out << QString("items, with those socketed in others: %1").arg(m_items);
            out << QString("items without a category: %1").arg(m_uncategorised_items);
            for (const QString &name : m_uncategorised) {
                out << "  " + name;
            }
            out << QString("mod lines read into mod tables: %1").arg(m_mod_lines);
            out << QString("mod lines that reach no mod name: %1, %2 of them different")
                       .arg(m_unmatched_lines)
                       .arg(m_unmatched.size());
            for (const QString &line : m_unmatched) {
                out << "  " + line;
            }
            out << QString("properties with a value: %1").arg(m_properties);
            out << QString("properties whose value is not one number: %1").arg(m_text_properties);
            for (const QString &name : m_text_names) {
                out << "  " + oneLine(name)
                           + (m_numeric_names.contains(name) ? " (a number on other items)"
                                                             : QString());
            }
            return out;
        }

    private:
        int m_items{0};
        int m_uncategorised_items{0};
        int m_mod_lines{0};
        int m_unmatched_lines{0};
        int m_properties{0};
        int m_text_properties{0};
        std::set<QString> m_uncategorised;
        std::set<QString> m_unmatched;
        std::set<QString> m_numeric_names;
        std::set<QString> m_text_names;
    };

} // namespace corpus
