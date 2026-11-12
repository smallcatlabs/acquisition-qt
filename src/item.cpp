// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "item.h"

#include <QMutex>
#include <QString>

#include "utility"
#include <set>
#include <sstream>
#include <string>
#include <string_view>

#include "itemcategories.h"
#include "itemlocation.h"
#include "modlist.h"
#include "poe/types/displaymode.h"
#include "poe/types/item.h"
#include "util/util.h"

static QString item_unique_properties(const std::optional<std::vector<poe::ItemProperty>> &props)
{
    if (!props) {
        return "";
    }
    QString result;
    for (const auto &prop : *props) {
        result += prop.name + "~";
        for (const auto &value : prop.values) {
            result += std::get<0>(value) + "~";
        }
    }
    return result;
}

// Fix up names, remove all <<set:X>> modifiers
static QString fixup_name(const QString &name)
{
    const auto k = name.lastIndexOf(">>");
    if (k >= 0) {
        return name.sliced(k + 2);
    } else {
        return name;
    }
}

// True the first time this message is reported in this process.
//
// The callers below are probes into GGG's data: they fire per item, so a
// single wrong assumption becomes one log line per item — tens of thousands
// on a large account, in the UI panel (whose sink is at warn), burying
// everything that matters. Callers warn on the first report and drop to
// debug afterwards.
//
// Those follow-ups are recorded only when the logging level is debug or
// lower — the level gates before the sinks, so the file sink being at trace
// is not enough on its own. That is the default in QT_DEBUG builds; release
// builds default to info, where only the first report survives unless the
// level is raised (--log-level, the login dialog, or the Logging menu). That
// is the intended trade: the first report says the assumption broke, and
// anyone counting instances is already turning the level up.
//
// Per process, not per refresh: the question these probes answer is "does
// this happen at all", and re-arming every refresh would only slow the flood
// down. A restart re-arms them.
//
// Keyed on the message alone, which is a literal at every call site. Note
// that the mods probes share their message text between the implicit and
// explicit paths, so whichever reports a given combination first suppresses
// the other's warn for it; the second path still logs at debug. Key on the
// caller's prefix too if that ever matters.
//
// Item is constructed on the cache-load thread (src/cacheload.cpp) and on the
// main thread during a refresh, so the lock is not optional. It is only ever
// reached on the anomalous path.
static bool first_report_of(std::string_view message)
{
    static QMutex mutex;
    static std::set<std::string, std::less<>> seen;
    QMutexLocker locker(&mutex);
    return seen.emplace(message).second;
}

Item::Item(const poe::Item &item, const ItemLocation &base_location)
{
    m_name = fixup_name(item.name);
    m_location = base_location.getItemLocation(item);

    if (item.hybrid) {
        const auto &hybrid = item.hybrid.value();
        if (hybrid.isVaalGem.value_or(false)) {
            // Do not use the base type for vaal gems.
            m_typeLine = item.typeLine;
        } else {
            // Use base type for other hybrid items.
            m_typeLine = hybrid.baseTypeName;
        }
    } else {
        m_typeLine = item.typeLine;
    }
    m_typeLine = fixup_name(m_typeLine);
    m_baseType = fixup_name(item.baseType);
    m_identified = item.identified;

    if (item.corrupted) {
        m_corrupted = *item.corrupted;
    }
    if (item.doubleCorrupted.value_or(false)) {
        // A double-corrupted item is corrupted for every purpose that reads
        // corrupted(), whether or not GGG also sends "corrupted" with it.
        m_corrupted = true;
    }
    if (item.fractured) {
        m_fractured = *item.fractured;
    }
    if (item.split) {
        m_split = *item.split;
    }
    if (item.synthesised) {
        m_synthesized = *item.synthesised;
    }
    if (item.mutated) {
        m_mutated = *item.mutated;
    }

    LoadInfluences(item);

    m_w = item.w;
    m_h = item.h;
    if (item.frameType) {
        m_frameType = static_cast<int>(*item.frameType);
    }
    m_icon = item.icon;

    LoadModifiers(item);

    // Other code assumes icon is proper size so force quad=1 to quad=0 here as it's clunky
    // to handle elsewhere
    m_icon.replace("quad=1", "quad=0");

    // quad stashes, currency stashes, etc
    m_icon.replace("scaleIndex=", "scaleIndex=0&");

    CalculateCategories();

    if (item.id) {
        m_uid = *item.id;
    }
    if (item.note) {
        m_note = *item.note;
    }

    if (item.properties) {
        LoadProperties(*item.properties);
    }

    if (item.requirements) {
        LoadRequirements(*item.requirements);
    }

    if (item.sockets) {
        LoadSockets(*item.sockets);
    }

    CalculateHash(item);

    m_ilvl = item.ilvl;
}

QString Item::PrettyName() const
{
    if (!m_name.isEmpty()) {
        return m_name + " " + m_typeLine;
    } else {
        return m_typeLine;
    }
}

void Item::LoadInfluences(const poe::Item &item)
{
    if (item.influences) {
        if (item.influences->shaper.value_or(false)) {
            m_influenceList.push_back(SHAPER);
        }
        if (item.influences->elder.value_or(false)) {
            m_influenceList.push_back(ELDER);
        }
        if (item.influences->crusader.value_or(false)) {
            m_influenceList.push_back(CRUSADER);
        }
        if (item.influences->redeemer.value_or(false)) {
            m_influenceList.push_back(REDEEMER);
        }
        if (item.influences->hunter.value_or(false)) {
            m_influenceList.push_back(HUNTER);
        }
        if (item.influences->warlord.value_or(false)) {
            m_influenceList.push_back(WARLORD);
        }
    }
    if (item.synthesised.value_or(false)) {
        m_influenceList.push_back(SYNTHESISED);
    }
    if (item.fractured.value_or(false)) {
        m_influenceList.push_back(FRACTURED);
    }
    if (item.searing.value_or(false)) {
        m_influenceList.push_back(SEARING_EXARCH);
    }
    if (item.tangled.value_or(false)) {
        m_influenceList.push_back(EATER_OF_WORLDS);
    }
}

void Item::LoadModifiers(const poe::Item &item)
{
    std::vector<QString> enchantMods;
    std::vector<QString> implicitMods;
    std::vector<QString> vestigialMods; // from implicitMods
    std::vector<QString> fracturedMods; // from explicitMods
    std::vector<QString> explicitMods;
    std::vector<QString> craftedMods; // from explicitMods
    std::vector<QString> mutatedMods; // from explicitMods

    if (item.enchantMods) {
        enchantMods.reserve(item.enchantMods->size());
        for (const auto &mod : *item.enchantMods) {
            enchantMods.push_back(mod);
        }
    }

    if (item.implicitMods) {
        for (const auto &mod : *item.implicitMods) {
            if (!mod.flags || !mod.flags->any()) {
                implicitMods.push_back(mod.description);
                continue;
            }

            const bool fractured = mod.flags->fractured.value_or(false);
            const bool mutated = mod.flags->mutated.value_or(false);
            const bool crafted = mod.flags->crafted.value_or(false);
            const bool desecrated = mod.flags->desecrated.value_or(false);
            const bool vestigial = mod.flags->vestigial.value_or(false);

            auto warn = [&item, &mod](std::string_view msg) {
                if (first_report_of(msg)) {
                    spdlog::warn("Implicit modifier on {} is {}: {}"
                                 " (further occurrences at debug)",
                                 item.name,
                                 msg,
                                 mod.description);
                } else {
                    spdlog::debug("Implicit modifier on {} is {}: {}",
                                  item.name,
                                  msg,
                                  mod.description);
                }
            };

            if (fractured) {
                // Assumes implicit mods cannot be fractured.
                warn("fractured");
                if (mutated) {
                    warn("both fractured and mutated");
                }
                if (crafted) {
                    warn("both fractured and crafted");
                }
                if (desecrated) {
                    warn("both fractured and desecrated");
                }
                if (vestigial) {
                    warn("both fractured and vestigial");
                }
                fracturedMods.push_back(mod.description);

            } else if (mutated) {
                // Assumes implicit mods cannot be mutated.
                warn("mutated");
                if (crafted) {
                    warn("mutated and crafted");
                }
                if (desecrated) {
                    warn("mutated and desecrated");
                }
                if (vestigial) {
                    warn("mutated and vestigial");
                }
                mutatedMods.push_back(mod.description);

            } else if (crafted) {
                // Assumes implicit mods cannot be crafted.
                warn("crafted");
                if (desecrated) {
                    warn("crafted and desecrated");
                }
                if (vestigial) {
                    warn("crafted and vestigial");
                }
                craftedMods.push_back(mod.description);

            } else if (desecrated) {
                // Assumes implicit mods cannot be desecrated.
                warn("desecrated");
                if (vestigial) {
                    warn("desecrated and vestigial");
                }

            } else if (vestigial) {
                // Implicit mods are allowed to be vestigial.
                vestigialMods.push_back(mod.description);

            } else {
                // Unreachable while every flag in Flags::any() has a branch
                // above. Kept so a flag added to the struct without a branch
                // here announces itself instead of passing as unflagged.
                warn("flagged with no recognized flag");
                implicitMods.push_back(mod.description);
            }
        }
    }

    if (item.explicitMods) {
        for (const auto &mod : *item.explicitMods) {
            if (!mod.flags || !mod.flags->any()) {
                explicitMods.push_back(mod.description);
                continue;
            }

            const bool fractured = mod.flags->fractured.value_or(false);
            const bool mutated = mod.flags->mutated.value_or(false);
            const bool crafted = mod.flags->crafted.value_or(false);
            const bool desecrated = mod.flags->desecrated.value_or(false);
            const bool vestigial = mod.flags->vestigial.value_or(false);

            auto warn = [&item, &mod](std::string_view msg) {
                if (first_report_of(msg)) {
                    spdlog::warn("Explicit modifier on {} is {}: {}"
                                 " (further occurrences at debug)",
                                 item.name,
                                 msg,
                                 mod.description);
                } else {
                    spdlog::debug("Explicit modifier on {} is {}: {}",
                                  item.name,
                                  msg,
                                  mod.description);
                }
            };

            if (fractured) {
                if (mutated) {
                    warn("both fractured and mutated");
                }
                if (crafted) {
                    warn("both fractured and crafted");
                }
                if (desecrated) {
                    warn("both fractured and desecrated");
                }
                if (vestigial) {
                    warn("both fractured and vestigial");
                }
                fracturedMods.push_back(mod.description);

            } else if (mutated) {
                if (crafted) {
                    warn("mutated and crafted");
                }
                if (desecrated) {
                    warn("mutated and desecrated");
                }
                if (vestigial) {
                    warn("mutated and vestigial");
                }
                mutatedMods.push_back(mod.description);

            } else if (crafted) {
                if (desecrated) {
                    warn("crafted and desecrated");
                }
                if (vestigial) {
                    warn("crafted and vestigial");
                }
                craftedMods.push_back(mod.description);

            } else if (desecrated) {
                if (vestigial) {
                    warn("desecrated and vestigial");
                }

            } else if (vestigial) {
                // Assumes explicit mods cannot be vestigial.
                warn("vestigial");

            } else {
                // Unreachable while every flag in Flags::any() has a branch
                // above. Kept so a flag added to the struct without a branch
                // here announces itself instead of passing as unflagged.
                warn("flagged with no recognized flag");
                explicitMods.push_back(mod.description);
            }
        }
    }

    m_enchanted = (item.enchantMods && !item.enchantMods->empty());
    m_crafted = !craftedMods.empty();

    m_text_mods["enchantMods"] = enchantMods;
    m_text_mods["implicitMods"] = implicitMods;
    m_text_mods["fracturedMods"] = fracturedMods;
    m_text_mods["explicitMods"] = explicitMods;
    m_text_mods["craftedMods"] = craftedMods;
    m_text_mods["mutatedMods"] = mutatedMods;

    for (const auto &[name, mods] : m_text_mods) {
        for (const auto &mod : mods) {
            AddModToTable(mod, m_mod_table);
        }
    }
}

void Item::LoadProperties(const std::vector<poe::ItemProperty> &properties)
{
    bool seen_plain_quality = false;
    for (const auto &prop : properties) {
        const QString name = prop.name;
        const auto &values = prop.values;

        if (name == "Elemental Damage") {
            m_elemental_damage.reserve(values.size());
            for (const auto &value : values) {
                // We use std::get to explicitly convert from std::tuple to std::pair.
                m_elemental_damage.emplace_back(std::get<0>(value), std::get<1>(value));
            }
        } else if (values.size() > 0) {
            const auto &firstValue = values[0];
            QString strval = std::get<0>(firstValue);
            if (name == "Quality") {
                // Quality is stored like "+23%" but we want to use "23".
                if (strval.startsWith("+")) {
                    strval.removeFirst();
                }
                if (strval.endsWith("%")) {
                    strval.chop(1);
                }
            } else if (name == "Level") {
                // Some items like gems at max level have the text "(Max)" after the
                // level number. This needs to be removed so the search field can be
                // matched.
                if (strval.endsWith("(Max)")) {
                    // Remove "(Max)" and the space before it.
                    strval.chop(6);
                }
            } else if (name == "Stack Size") {
                // "123/5000": the count is the part before the slash.
                const auto n = strval.indexOf("/");
                m_count = (n >= 0 ? strval.first(n) : strval).toInt();
            }
            m_properties[name] = strval;

            // The filters need numbers, but values arrive in display form:
            // "8.00%", "+20%". Keep the display string above and store the
            // number separately when the value is a single number.
            QString numeric = strval.trimmed();
            if (numeric.startsWith("+")) {
                numeric.removeFirst();
            }
            if (numeric.endsWith("%")) {
                numeric.chop(1);
            }
            bool ok = false;
            const double number = numeric.toDouble(&ok);
            if (ok) {
                m_numeric_properties[name] = number;
                if (name.startsWith("Quality (")) {
                    // Catalyst quality: "Quality (Attribute Modifiers)". It
                    // counts as Quality unless the item has plain Quality.
                    if (!seen_plain_quality) {
                        m_numeric_properties["Quality"] = number;
                    }
                } else if (name == "Quality") {
                    seen_plain_quality = true;
                }
            }
        }

        ItemProperty property;
        property.name = name;
        property.display_mode = static_cast<int>(
            prop.displayMode.value_or(poe::DisplayMode::InsertedValues));
        for (const auto &[str, type] : values) {
            property.values.emplace_back(str, type);
        }
        m_text_properties.push_back(property);
    }
}

void Item::LoadRequirements(const std::vector<poe::ItemProperty> &requirements)
{
    for (const auto &req : requirements) {
        const auto &values = req.values;
        if (values.size() < 1) {
            continue;
        }
        const auto &name = req.name;
        const auto &[str, type] = values[0];
        // The game spells stat requirements both ways ("Int" and
        // "Intelligence"); the numeric map uses the short form. The display
        // list keeps the name as sent.
        QString key = name;
        if (name == "Strength") {
            key = QStringLiteral("Str");
        } else if (name == "Dexterity") {
            key = QStringLiteral("Dex");
        } else if (name == "Intelligence") {
            key = QStringLiteral("Int");
        }
        m_requirements[key] = str.toInt();
        m_text_requirements.push_back({name, ItemPropertyValue{str, type}});
    }
}

void Item::LoadSockets(const std::vector<poe::ItemSocket> &sockets)
{
    ItemSocketGroup current_group = {0, 0, 0, 0};
    m_sockets_cnt = static_cast<int>(sockets.size());
    int counter = 0;
    int prev_group = -1;
    for (const auto &socket : sockets) {
        QString code;
        if (socket.attr) {
            code = *socket.attr;
        } else if (socket.sColour) {
            code = *socket.sColour;
        }

        if (code.isEmpty()) {
            continue;
        }
        // The tooltip and the POB export still read the first letter.
        const char attr = code[0].toLatin1();

        const int group = socket.group;
        ItemSocket current_socket = {static_cast<unsigned char>(group), attr};
        m_text_sockets.push_back(current_socket);
        if (prev_group != current_socket.group) {
            counter = 0;
            m_socket_groups.push_back(current_group);
            current_group = {0, 0, 0, 0};
        }
        prev_group = current_socket.group;
        ++counter;
        m_links_cnt = std::max(m_links_cnt, counter);
        // Decided on the whole code: "DV" (Delve resonator) is not "D", and
        // "DV" and "A" (abyss) sockets have no colour.
        if (code == "S") {
            m_sockets.r++;
            current_group.r++;
        } else if (code == "D") {
            m_sockets.g++;
            current_group.g++;
        } else if (code == "I") {
            m_sockets.b++;
            current_group.b++;
        } else if (code == "G") {
            m_sockets.w++;
            current_group.w++;
        }
    }
    m_socket_groups.push_back(current_group);
}

void Item::CalculateCategories()
{
    m_category = GetItemCategory(m_baseType);
    if (m_category.isEmpty() == false) {
        return;
    }
    // If we didn't find a category on the first try, check to see if
    // this might be a transfigured skill gem by looking for the base
    // name and seeing if that's something we can categorize.
    const auto indx = m_baseType.indexOf(" of ");
    if (indx >= 0) {
        const auto altBaseType = m_baseType.first(indx);
        m_category = GetItemCategory(altBaseType);
        if (m_category.isEmpty() == false) {
            return;
        }
    }
}

double Item::DPS() const
{
    return pDPS() + eDPS() + cDPS();
}

double Item::pDPS() const
{
    const auto phys = m_properties.find(QStringLiteral("Physical Damage"));
    if (phys == m_properties.end()) {
        return 0;
    }
    const auto aps = m_properties.find(QStringLiteral("Attacks per Second"));
    if (aps == m_properties.end()) {
        return 0;
    }
    const double attacks = aps->second.toDouble();
    const QString hit = phys->second;
    return attacks * Util::AverageDamage(hit);
}

double Item::eDPS() const
{
    if (m_elemental_damage.empty()) {
        return 0;
    }
    const auto aps = m_properties.find(QStringLiteral("Attacks per Second"));
    if (aps == m_properties.end()) {
        return 0;
    }
    double damage = 0;
    for (auto &x : m_elemental_damage) {
        damage += Util::AverageDamage(x.first);
    }
    const double attacks = aps->second.toDouble();
    return attacks * damage;
}

double Item::cDPS() const
{
    const auto chaos = m_properties.find(QStringLiteral("Chaos Damage"));
    if (chaos == m_properties.end()) {
        return 0;
    }

    const auto aps = m_properties.find(QStringLiteral("Attacks per Second"));
    if (aps == m_properties.end()) {
        return 0;
    }
    const double attacks = aps->second.toDouble();
    const QString hit = chaos->second;

    return attacks * Util::AverageDamage(hit);
}

void Item::CalculateHash(const poe::Item &json)
{
    QString unique_new = m_name + "~" + m_typeLine + "~";

    QString unique_common;

    if (json.explicitMods) {
        for (const auto &mod : *json.explicitMods) {
            unique_common += mod.description + "~";
        }
    }
    if (json.implicitMods) {
        for (const auto &mod : *json.implicitMods) {
            unique_common += mod.description + "~";
        }
    }

    unique_common += item_unique_properties(json.properties) + "~";
    unique_common += item_unique_properties(json.additionalProperties) + "~";

    if (json.sockets) {
        for (const auto &socket : *json.sockets) {
            if (!socket.attr) {
                continue;
            }
            const int group = socket.group;
            const QString attr = *socket.attr;
            unique_common += QString::number(group) + "~" + attr + "~";
        }
    }

    unique_common += "~" + m_location.GetLegacyHash();

    unique_new += unique_common;

    m_hash = Util::Md5(unique_new);
}

bool Item::operator<(const Item &rhs) const
{
    const QString name = PrettyName();
    const QString rhs_name = rhs.PrettyName();
    return std::tie(name, m_uid, m_hash) < std::tie(rhs_name, rhs.m_uid, rhs.m_hash);
}

bool Item::Wearable() const
{
    return (m_category == "flasks" || m_category == "amulet" || m_category == "ring"
            || m_category == "belt" || m_category.contains("armour")
            || m_category.contains("weapons") || m_category.contains("jewels"));
}

QString Item::POBformat() const
{
    std::stringstream pob;
    // clang-format off
    switch (m_frameType) {
    case  0: pob << "Rarity: NORMAL"; break;
    case  1: pob << "Rarity: MAGIC"; break;
    case  2: pob << "Rarity: RARE"; break;
    case  3: pob << "Rarity: UNIQUE"; break;
    case  4: // gem
    case  5: // currency
    case  6: // divination card
    case  7: // quest
    case  8: spdlog::error("Cannot build POB format: unsupported frameType: {}", m_frameType); break; // prophecy (legacy)
    case  9: pob << "Rarity: UNIQUE"; break; // foil
    case 10: pob << "Rarity: UNIQUE"; break; // supporter foil
    case 11: spdlog::error("Cannot build POB format: unsupported frameType: {}", m_frameType); break; // necropolis
    default: spdlog::error("Cannot build POB format: unrecognized frameType: {}", m_frameType); break;
    }
    // clang-format on
    pob << "\n" << name().toStdString();
    pob << "\n" << typeLine().toStdString();
    pob << "\nUnique ID: " << m_uid.toStdString();
    pob << "\nItem Level: " << m_ilvl;

    const auto qual = m_properties.find(QStringLiteral("Quality"));
    if (qual != m_properties.end()) {
        QString quality = qual->second;
        if (quality.startsWith("+")) {
            quality.slice(1);
        }
        if (quality.endsWith("%")) {
            quality.chop(1);
        }
        pob << "\nQuality: " << quality.toInt();
    }

    const auto &sockets = text_sockets();
    if (sockets.size() > 0) {
        pob << "\nSockets: ";
        ItemSocket prev = {255, '-'};
        size_t i = 0;
        for (const auto &socket : sockets) {
            bool link = socket.group == prev.group;
            if (i > 0) {
                pob << (link ? "-" : " ");
            }
            switch (socket.attr) {
            case 'S':
                pob << "R";
                break;
            case 'D':
                pob << "G";
                break;
            case 'I':
                pob << "B";
                break;
            case 'G':
                pob << "W";
                break;
            default:
                pob << socket.attr;
                break;
            }
            prev = socket;
            ++i;
        }
    }

    const auto lvl = m_requirements.find(QStringLiteral("Level"));
    if (lvl != m_requirements.end()) {
        pob << "\nLevelReq: " << lvl->second;
    }

    const auto &implicitMods = m_text_mods.at("implicitMods");
    const auto &enchantMods = m_text_mods.at("enchantMods");
    pob << "\nImplicits: " << (implicitMods.size() + enchantMods.size());
    for (const auto &mod : enchantMods) {
        pob << "\n{crafted}" << mod.toStdString();
    }
    for (const auto &mod : implicitMods) {
        pob << "\n" << mod.toStdString();
    }

    using mod_pair = std::pair<const char *, const char *>;
    const std::array<mod_pair, 4> mod_sets{{
        {"fracturedMods", "{fractured}"},
        {"explicitMods", ""},
        {"craftedMods", "{crafted}"},
        {"mutatedMods", "{mutated}"},
    }};
    for (const auto &[name, tag] : mod_sets) {
        const auto mods = m_text_mods.at(name);
        for (const auto &mod : mods) {
            pob << "\n" << tag << mod.toStdString();
        }
    }

    if (m_corrupted) {
        pob << "\nCorrupted";
    }

    return QString::fromStdString(pob.str());
}
