// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#include "ui/itemtooltiptext.h"

#include <array>

#include "item.h"

/*
    PoE colors:
    Default: 0
    Augmented: 1
    Unmet: 2
    PhysicalDamage: 3
    FireDamage: 4
    ColdDamage: 5
    LightningDamage: 6
    ChaosDamage: 7
    MagicItem: 8
    RareItem: 9
    UniqueItem: 10
*/

static std::array kPoEColors
    = {"#fff", "#88f", "#d20000", "#fff", "#960000", "#366492", "gold", "#d02090"};

QString ColorPropertyValue(const ItemPropertyValue &value)
{
    size_t type = value.type;
    if (type >= kPoEColors.size()) {
        type = 0;
    }
    const QString color = kPoEColors[type];
    return "<font color='" + color + "'>" + value.str + "</font>";
}

QString FormatProperty(const ItemProperty &prop)
{
    if (prop.display_mode == 3) {
        QString format(prop.name);
        for (size_t i = 0; i < prop.values.size(); ++i) {
            const QString placeholder = QString("{%1}").arg(i);
            const QString value = ColorPropertyValue(prop.values[i]);
            format = format.replace(placeholder, value);
        }
        return format;
    }
    QString text = prop.name;
    if (prop.values.size()) {
        if (prop.name.size() > 0) {
            text += ": ";
        }
        bool first = true;
        for (auto &value : prop.values) {
            if (!first) {
                text += ", ";
            }
            first = false;
            text += ColorPropertyValue(value);
        }
    }
    return text;
}

QString GenerateProperties(const Item &item)
{
    QString text;
    bool first = true;
    for (auto &property : item.text_properties()) {
        if (!first) {
            text += "<br>";
        }
        first = false;
        text += FormatProperty(property);
    }

    return text;
}

QString GenerateRequirements(const Item &item)
{
    QString text;
    bool first = true;
    for (auto &requirement : item.text_requirements()) {
        text += first ? "Requires " : ", ";
        first = false;
        text += requirement.name + " " + ColorPropertyValue(requirement.value);
    }
    return text;
}

QString getTextMods(const Item &item, const QString &modType, const char *modColor)
{
    const auto &item_mods = item.text_mods();
    const auto it = item_mods.find(modType);
    if (it == item_mods.end()) {
        return QString();
    }
    const auto modvec = it->second;
    if (modvec.empty()) {
        return QString();
    }
    const auto mods = QStringList{modvec.begin(), modvec.end()};
    return QString("<font color='%1'>%2</font>").arg(modColor, mods.join("<br>"));
}

std::vector<QString> GenerateMods(const Item &item)
{
    // Create colored strings for each mod set.
    const auto enchantMods = getTextMods(item, "enchantMods", "#b4b4ff");
    const auto implicitMods = getTextMods(item, "implicitMods", "#88f");
    const auto fracturedMods = getTextMods(item, "fracturedMods", "#a29162");
    const auto explicitMods = getTextMods(item, "explicitMods", "#88f");
    const auto craftedMods = getTextMods(item, "craftedMods", "#b4b4ff");
    const auto mutatedMods = getTextMods(item, "mutatedMods", "#cd2285");
    const auto vestigialMods = getTextMods(item, "vestigialMods", "#88f");

    // There are no spacers between fractured, implicit, and crafted mods.
    // Mutuated mods on foulborn uniques go at the bottom of this section as well.
    QStringList main_section;
    if (!fracturedMods.isEmpty()) {
        main_section.push_back(fracturedMods);
    }
    if (!explicitMods.isEmpty()) {
        main_section.push_back(explicitMods);
    }
    if (!craftedMods.isEmpty()) {
        main_section.push_back(craftedMods);
    }
    if (!mutatedMods.isEmpty()) {
        main_section.push_back(mutatedMods);
    }

    // There are spacers between enchants, implicits, vestigial mods (shown
    // apart because they are not counted, #266) and the main section.
    std::vector<QString> sections;
    if (!enchantMods.isEmpty()) {
        sections.push_back(enchantMods);
    }
    if (!implicitMods.isEmpty()) {
        sections.push_back(implicitMods);
    }
    if (!vestigialMods.isEmpty()) {
        sections.push_back(vestigialMods);
    }
    if (!main_section.isEmpty()) {
        sections.push_back(main_section.join("<br>"));
    }
    return sections;
}

QString GenerateItemInfo(const Item &item, const QString &key, bool fancy)
{
    std::vector<QString> sections;

    QString properties_text = GenerateProperties(item);
    if (properties_text.size() > 0) {
        sections.push_back(properties_text);
    }

    QString requirements_text = GenerateRequirements(item);
    if (requirements_text.size() > 0) {
        sections.push_back(requirements_text);
    }

    std::vector<QString> mods = GenerateMods(item);
    sections.insert(sections.end(), mods.begin(), mods.end());

    QString unmet;
    if (!item.identified()) {
        unmet += "Unidentified";
    }
    if (item.corrupted()) {
        unmet += (unmet.isEmpty() ? "" : "<br>") + QString("Corrupted");
    }
    if (!unmet.isEmpty()) {
        sections.emplace_back(ColorPropertyValue(ItemPropertyValue{unmet, 2}));
    }

    QString text;
    bool first = true;
    for (auto &s : sections) {
        if (!first) {
            text += "<br>";
            if (fancy) {
                text += "<img src=':/separators/Separator" + key + ".png'><br>";
            } else {
                text += "<hr>";
            }
        }
        first = false;
        text += s;
    }
    if (!fancy) {
        text = ColorPropertyValue(ItemPropertyValue{item.PrettyName(), 0}) + "<hr>" + text;
    }
    return "<center>" + text + "</center>";
}
