// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "item.h"

#include <QMutex>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QString>

#include <array>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

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
        m_note_price = ParseGamePrice(m_note);
    }

    if (item.properties) {
        LoadProperties(*item.properties);
    }

    if (!m_numeric_properties.contains(QStringLiteral("Map Tier"))) {
        // Maps carry their tier in the base type, "Map (Tier 16)", and no
        // "Map Tier" property (#229). Any other shape gives no tier.
        static const QRegularExpression map_tier(R"(^Map \(Tier ([1-9][0-9]?)\)$)");
        const QRegularExpressionMatch match = map_tier.match(m_baseType);
        if (match.hasMatch()) {
            m_numeric_properties[QStringLiteral("Map Tier")] = match.captured(1).toInt();
        }
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

namespace {

    // The sections an item's mod lines are shown and searched in.
    enum class ModSection { Enchant, Implicit, Fractured, Explicit, Crafted, Mutated, Vestigial };

    constexpr std::array kModSections = {ModSection::Enchant,
                                         ModSection::Implicit,
                                         ModSection::Fractured,
                                         ModSection::Explicit,
                                         ModSection::Crafted,
                                         ModSection::Mutated,
                                         ModSection::Vestigial};

    QString section_key(ModSection section)
    {
        switch (section) {
        case ModSection::Enchant:
            return QStringLiteral("enchantMods");
        case ModSection::Implicit:
            return QStringLiteral("implicitMods");
        case ModSection::Fractured:
            return QStringLiteral("fracturedMods");
        case ModSection::Explicit:
            return QStringLiteral("explicitMods");
        case ModSection::Crafted:
            return QStringLiteral("craftedMods");
        case ModSection::Mutated:
            return QStringLiteral("mutatedMods");
        case ModSection::Vestigial:
            return QStringLiteral("vestigialMods");
        }
        return {};
    }

    // The section a mod line goes to. Fractured, mutated, crafted and
    // vestigial mods have sections of their own; every other mod, flagged or
    // not, stays in the section of the array it arrived in, so no line the
    // game sends is dropped (#266). rareonly says where a mod can roll, not
    // what it is.
    ModSection route_mod(ModSection arrived_in, const poe::ItemMod::Flags &flags)
    {
        if (flags.fractured.value_or(false)) {
            return ModSection::Fractured;
        }
        if (flags.mutated.value_or(false)) {
            return ModSection::Mutated;
        }
        if (flags.crafted.value_or(false)) {
            return ModSection::Crafted;
        }
        if (flags.vestigial.value_or(false)) {
            return ModSection::Vestigial;
        }
        return arrived_in;
    }

    // Reports flags the code assumes the game does not send: more than one
    // kind on a line, an implicit that is fractured, mutated, crafted or
    // desecrated, and an explicit that is vestigial. The line is kept either
    // way.
    void probe_flags(const poe::Item &item,
                     const poe::ItemMod &mod,
                     const poe::ItemMod::Flags &flags,
                     ModSection arrived_in)
    {
        const std::array<std::pair<const char *, bool>, 5> set{{
            {"fractured", flags.fractured.value_or(false)},
            {"mutated", flags.mutated.value_or(false)},
            {"crafted", flags.crafted.value_or(false)},
            {"desecrated", flags.desecrated.value_or(false)},
            {"vestigial", flags.vestigial.value_or(false)},
        }};
        std::string names;
        int count = 0;
        for (const auto &[name, on] : set) {
            if (on) {
                names += (count++ > 0) ? "+" : "";
                names += name;
            }
        }
        const bool implicit = (arrived_in == ModSection::Implicit);
        const bool unexpected = (count > 1)
                                || (implicit && (count > 0) && !flags.vestigial.value_or(false))
                                || (!implicit && flags.vestigial.value_or(false));
        if (!unexpected) {
            return;
        }
        const std::string message = (implicit ? "implicit " : "explicit ") + names;
        if (first_report_of(message)) {
            spdlog::warn("Modifier on {} is {}: {} (further occurrences at debug)",
                         item.name,
                         message,
                         mod.description);
        } else {
            spdlog::debug("Modifier on {} is {}: {}", item.name, message, mod.description);
        }
    }

} // namespace

void Item::LoadModifiers(const poe::Item &item)
{
    std::array<ItemMods, kModSections.size()> sections;
    const auto add = [&sections](ModSection section, const QString &line) {
        sections[static_cast<size_t>(section)].push_back(line);
    };

    if (item.enchantMods) {
        for (const auto &mod : *item.enchantMods) {
            add(ModSection::Enchant, mod);
        }
    }

    const auto addArray = [&item, &add](const std::optional<std::vector<poe::ItemMod>> &mods,
                                        ModSection arrived_in) {
        if (!mods) {
            return;
        }
        for (const auto &mod : *mods) {
            const poe::ItemMod::Flags flags = mod.flags.value_or(poe::ItemMod::Flags{});
            probe_flags(item, mod, flags, arrived_in);
            add(route_mod(arrived_in, flags), mod.description);
        }
    };
    addArray(item.implicitMods, ModSection::Implicit);
    addArray(item.explicitMods, ModSection::Explicit);

    m_enchanted = (item.enchantMods && !item.enchantMods->empty());
    m_crafted = !sections[static_cast<size_t>(ModSection::Crafted)].empty();

    for (const ModSection section : kModSections) {
        m_text_mods[section_key(section)] = std::move(sections[static_cast<size_t>(section)]);
    }

    for (const auto &[name, mods] : m_text_mods) {
        // Vestigial lines are shown but not counted. Provisional: what the
        // game means by the flag is not known, and this keeps every number
        // as it was before such lines were kept (#266).
        if (name == section_key(ModSection::Vestigial)) {
            continue;
        }
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

static std::optional<SocketKind> socket_kind(const poe::ItemSocket &socket)
{
    // The attribute and the colour name the same thing in two alphabets; PoE2
    // sockets carry neither.
    if (socket.attr) {
        const QString &attr = *socket.attr;
        if (attr == "S") {
            return SocketKind::Red;
        } else if (attr == "D") {
            return SocketKind::Green;
        } else if (attr == "I") {
            return SocketKind::Blue;
        } else if (attr == "G") {
            return SocketKind::White;
        } else if (attr == "A") {
            return SocketKind::Abyss;
        } else if (attr == "DV") {
            return SocketKind::Resonator;
        }
        return SocketKind::Unknown;
    }
    if (socket.sColour) {
        const QString &colour = *socket.sColour;
        if (colour == "R") {
            return SocketKind::Red;
        } else if (colour == "G") {
            return SocketKind::Green;
        } else if (colour == "B") {
            return SocketKind::Blue;
        } else if (colour == "W") {
            return SocketKind::White;
        } else if (colour == "A") {
            return SocketKind::Abyss;
        } else if (colour == "DV") {
            return SocketKind::Resonator;
        }
        return SocketKind::Unknown;
    }
    return std::nullopt;
}

void Item::LoadSockets(const std::vector<poe::ItemSocket> &sockets)
{
    ItemSocketGroup current_group = {0, 0, 0, 0};
    m_sockets_cnt = static_cast<int>(sockets.size());
    int counter = 0;
    int prev_group = -1;
    for (const auto &socket : sockets) {
        const auto kind = socket_kind(socket);
        if (!kind) {
            continue;
        }

        const ItemSocket current_socket = {static_cast<unsigned char>(socket.group), *kind};
        m_text_sockets.push_back(current_socket);
        if (prev_group != current_socket.group) {
            counter = 0;
            m_socket_groups.push_back(current_group);
            current_group = {0, 0, 0, 0};
        }
        prev_group = current_socket.group;
        ++counter;
        m_links_cnt = std::max(m_links_cnt, counter);
        // Abyss and resonator sockets count as sockets and links, with no
        // colour.
        switch (*kind) {
        case SocketKind::Red:
            m_sockets.r++;
            current_group.r++;
            break;
        case SocketKind::Green:
            m_sockets.g++;
            current_group.g++;
            break;
        case SocketKind::Blue:
            m_sockets.b++;
            current_group.b++;
            break;
        case SocketKind::White:
            m_sockets.w++;
            current_group.w++;
            break;
        case SocketKind::Abyss:
        case SocketKind::Resonator:
        case SocketKind::Unknown:
            break;
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
    return IsPathOfBuildingBaseType(m_baseType);
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
        ItemSocket prev = {255, SocketKind::Unknown};
        size_t i = 0;
        for (const auto &socket : sockets) {
            bool link = socket.group == prev.group;
            if (i > 0) {
                pob << (link ? "-" : " ");
            }
            switch (socket.kind) {
            case SocketKind::Red:
                pob << "R";
                break;
            case SocketKind::Green:
                pob << "G";
                break;
            case SocketKind::Blue:
                pob << "B";
                break;
            case SocketKind::White:
                pob << "W";
                break;
            case SocketKind::Abyss:
                pob << "A";
                break;
            case SocketKind::Resonator:
                pob << "DV";
                break;
            case SocketKind::Unknown:
                pob << "?";
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
