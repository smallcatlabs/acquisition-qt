// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "column.h"

#include <cmath>
#include <QApplication>
#include <QIcon>
#include <QPalette>
#include <QRegularExpression>
#include <QVector>

#include "buyoutmanager.h"
#include "influence.h"
#include "itemconstants.h"
#include "modelprobes.h"
#include "util/util.h"

const double EPS = 1e-6;
const char *SORT_DOUBLE_MATCH = "^\\+?([\\d.]+)%?$";
const char *SORT_TWO_VALUES = "^(\\d+)([-/])(\\d+)$";

QColor Column::color(const Item & /* item */) const
{
    return QApplication::palette().color(QPalette::WindowText);
}

Column::MultivalueParts Column::parts(const Item *item, const QString &pretty) const
{
    // Transform values into something optimal for sorting
    // Possibilities: 12, 12.12, 10%, 10.13%, +16%, 12-14, 10/20
    static const QRegularExpression sort_double_match(SORT_DOUBLE_MATCH);
    static const QRegularExpression sort_two_values(SORT_TWO_VALUES);

    MultivalueParts result;

    QString str = sortValue(*item).toString();
    QRegularExpressionMatch match;

    if (str.contains(sort_double_match, &match)) {
        result.d1 = match.captured(1).toDouble();
    } else if (str.contains(sort_two_values, &match)) {
        if (match.captured(2).startsWith("-")) {
            result.d1 = 0.5 * (match.captured(1).toDouble() + match.captured(3).toDouble());
        } else {
            result.s1 = pretty;
            result.d2 = match.captured(1).toDouble();
        }
    } else {
        result.s1 = str;
        result.s2 = pretty;
    }

    return result;
}

Column::sort_tuple Column::multivalue(const Item *item) const
{
    MultivalueParts p = parts(item, item->PrettyName());
    return sort_tuple(p.d1, std::move(p.s1), p.d2, std::move(p.s2), *item);
}

bool Column::lt(const Item *lhs, const Item *rhs) const
{
    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.comparator_calls;
    }
    return multivalue(lhs) < multivalue(rhs);
}

ItemSortKey::Suffix Column::suffix(const Item &item, const QString &pretty)
{
    return ItemSortKey::Suffix(pretty, item.id(), item.hash_v4());
}

ItemSortKey Column::key(const Item &item) const
{
    // pretty is computed once: the head's s1/s2 and the suffix's first
    // element share its buffer (D1's measured s2/suffix sharing).
    const QString pretty = item.PrettyName();
    MultivalueParts p = parts(&item, pretty);
    return ItemSortKey{ItemSortKey::BaseHead(p.d1, std::move(p.s1), p.d2, std::move(p.s2)),
                       suffix(item, pretty)};
}

QString NameColumn::name() const
{
    return "Name";
}

QVariant NameColumn::value(const Item &item) const
{
    return item.PrettyName();
}

QColor NameColumn::color(const Item &item) const
{
    switch (item.frameType()) {
    case FrameType::FRAME_TYPE_MAGIC:
        return QColor(0x00, 0x66, 0x99);
    case FrameType::FRAME_TYPE_RARE:
        return QColor(Qt::darkYellow);
    case FrameType::FRAME_TYPE_UNIQUE:
        return QColor(234, 117, 0);
    case FrameType::FRAME_TYPE_GEM:
        return QColor(0x1b, 0xa2, 0x9b);
    case FrameType::FRAME_TYPE_CURRENCY:
        return QColor(0x77, 0x6e, 0x59);
    case FrameType::FRAME_TYPE_DIVINATION_CARD:
        return QColor(QRgb(0x01bcba));
    case FrameType::FRAME_TYPE_QUEST_ITEM:
        return QColor(QRgb(0x4ae63a));
    case FrameType::FRAME_TYPE_PROPHECY:
        return QColor(181, 75, 255);
    case FrameType::FRAME_TYPE_FOIL:
        return QColor(QRgb(0x82ad6a));
    case FrameType::FRAME_TYPE_SUPPORTER_FOIL:
        return QColor(QRgb(0x82ad6a)); // TBD: is this right?
    default:
        return QApplication::palette().color(QPalette::WindowText);
    }
}

QString CorruptedColumn::name() const
{
    return "Corr";
}

QVariant CorruptedColumn::value(const Item &item) const
{
    if (item.corrupted()) {
        return "C";
    }
    return QVariant();
}

QString CraftedColumn::name() const
{
    return "Mast";
}

QVariant CraftedColumn::value(const Item &item) const
{
    if (item.crafted()) {
        return "M";
    }
    return QVariant();
}

QString EnchantedColumn::name() const
{
    return "Ench";
}

QVariant EnchantedColumn::value(const Item &item) const
{
    if (item.enchanted()) {
        return "En";
    }
    return QVariant();
}

QString InfluncedColumn::name() const
{
    return "Inf";
}

QVariant InfluncedColumn::value(const Item & /* item */) const
{
    // The cell shows the influence icons only (icon()); sorting reads the
    // letters from sortValue().
    //TODO maybe add an option to toggle whether to send an icon or text for an influence?
    return "";
}

QVariant InfluncedColumn::sortValue(const Item &item) const
{
    QString influencesString = "";

    if (item.hasInfluence(Item::SHAPER)) {
        influencesString += "S";
    }
    if (item.hasInfluence(Item::ELDER)) {
        influencesString += "E";
    }
    if (item.hasInfluence(Item::HUNTER)) {
        influencesString += "H";
    }
    if (item.hasInfluence(Item::WARLORD)) {
        influencesString += "W";
    }
    if (item.hasInfluence(Item::CRUSADER)) {
        influencesString += "C";
    }
    if (item.hasInfluence(Item::REDEEMER)) {
        influencesString += "R";
    }
    return influencesString;
}

QVariant InfluncedColumn::icon(const Item &item) const
{
    const Item::INFLUENCE_TYPES leftInf = item.influenceLeft();
    const Item::INFLUENCE_TYPES rightInf = item.influenceRight();
    if (leftInf == Item::NONE) {
        return QVariant::fromValue(NULL);
    }
    const QIcon leftIcon(InfluenceSymbolPath(leftInf));
    if (rightInf != Item::NONE && rightInf != leftInf) {
        return combineInflunceIcons(leftIcon, QIcon(InfluenceSymbolPath(rightInf)));
    }
    return leftIcon;
}

PropertyColumn::PropertyColumn(const QString &name)
    : m_name(name)
    , m_property(name)
{}

PropertyColumn::PropertyColumn(const QString &name, const QString &property)
    : m_name(name)
    , m_property(property)
{}

QString PropertyColumn::name() const
{
    return m_name;
}

QVariant PropertyColumn::value(const Item &item) const
{
    const auto &properties = item.properties();
    const auto result = properties.find(m_property);
    if (result != properties.end()) {
        return result->second;
    }
    return QVariant();
}

QString DPSColumn::name() const
{
    return "DPS";
}

QVariant DPSColumn::value(const Item &item) const
{
    double dps = item.DPS();
    if (fabs(dps) < EPS) {
        return QVariant();
    }
    return dps;
}

QString pDPSColumn::name() const
{
    return "pDPS";
}

QVariant pDPSColumn::value(const Item &item) const
{
    double pdps = item.pDPS();
    if (fabs(pdps) < EPS) {
        return QVariant();
    }
    return pdps;
}

QString eDPSColumn::name() const
{
    return "eDPS";
}

QVariant eDPSColumn::value(const Item &item) const
{
    double edps = item.eDPS();
    if (fabs(edps) < EPS) {
        return QVariant();
    }
    return edps;
}

ElementalDamageColumn::ElementalDamageColumn(int index)
    : m_index(index)
{}

QString ElementalDamageColumn::name() const
{
    if (m_index == 0) {
        return "ED";
    }
    return "";
}

QVariant ElementalDamageColumn::value(const Item &item) const
{
    if (item.elemental_damage().size() > m_index) {
        auto &ed = item.elemental_damage().at(m_index);
        return ed.first;
    }
    return QVariant();
}

QColor ElementalDamageColumn::color(const Item &item) const
{
    if (item.elemental_damage().size() > m_index) {
        auto &ed = item.elemental_damage().at(m_index);
        switch (ed.second) {
        case ElementalDamageType::ED_FIRE:
            return QColor(0xc5, 0x13, 0x13);
        case ElementalDamageType::ED_COLD:
            return QColor(0x36, 0x64, 0x92);
        case ElementalDamageType::ED_LIGHTNING:
            return QColor(0xb9, 0x9c, 0x00);
        }
    }
    return QColor();
}

QString ChaosDamageColumn::name() const
{
    return "CD";
}

QVariant ChaosDamageColumn::value(const Item &item) const
{
    const auto &properties = item.properties();
    const auto result = properties.find(QStringLiteral("Chaos Damage"));
    if (result != properties.end()) {
        return result->second;
    }
    return QVariant();
}

QColor ChaosDamageColumn::color(const Item &item) const
{
    Q_UNUSED(item);
    return QColor(0xd0, 0x20, 0x90);
}

QString cDPSColumn::name() const
{
    return "cDPS";
}

QVariant cDPSColumn::value(const Item &item) const
{
    double cdps = item.cDPS();
    if (fabs(cdps) < EPS) {
        return QVariant();
    }
    return cdps;
}

PriceColumn::PriceColumn(const BuyoutManager &bo_manager)
    : m_bo_manager(bo_manager)
{}

QString PriceColumn::name() const
{
    return "Price";
}

QVariant PriceColumn::value(const Item &item) const
{
    return m_bo_manager.Effective(item).buyout.AsText();
}

QColor PriceColumn::color(const Item &item) const
{
    return m_bo_manager.Effective(item).buyout.IsInherited()
               ? QColor(0xaa, 0xaa, 0xaa)
               : QApplication::palette().color(QPalette::WindowText);
}

ItemSortKey::PriceHead PriceColumn::head(const Item &item) const
{
    const Buyout bo = m_bo_manager.Effective(item).buyout;
    return ItemSortKey::PriceHead(bo.currency.SortKey(), bo.value);
}

std::tuple<int, double, const Item &> PriceColumn::multivalue(const Item *item) const
{
    const ItemSortKey::PriceHead h = head(*item);
    // The item reference (rather than a copy or pointer) makes the tuple
    // comparison tie-break on the item object itself (Item::operator<).
    return std::tuple<int, double, const Item &>(std::get<0>(h), std::get<1>(h), *item);
}

bool PriceColumn::lt(const Item *lhs, const Item *rhs) const
{
    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.comparator_calls;
    }
    return multivalue(lhs) < multivalue(rhs);
}

ItemSortKey PriceColumn::key(const Item &item) const
{
    return ItemSortKey{head(item), suffix(item, item.PrettyName())};
}

DateColumn::DateColumn(const BuyoutManager &bo_manager)
    : m_bo_manager(bo_manager)
{}

QString DateColumn::name() const
{
    return "Last Update";
}

QVariant DateColumn::value(const Item &item) const
{
    const Buyout bo = m_bo_manager.Effective(item).buyout;
    // A price the game set has no date (#293): nothing is shown for it.
    return (bo.IsActive() && bo.last_update.isValid()) ? Util::TimeAgoInWords(bo.last_update)
                                                       : QVariant();
}

ItemSortKey::DateHead DateColumn::head(const Item &item) const
{
    return ItemSortKey::DateHead(m_bo_manager.Effective(item).buyout.last_update);
}

bool DateColumn::lt(const Item *lhs, const Item *rhs) const
{
    if (auto &probes = ModelProbes::instance(); probes.enabled) {
        ++probes.comparator_calls;
    }
    const ItemSortKey::DateHead lhs_head = head(*lhs);
    const ItemSortKey::DateHead rhs_head = head(*rhs);
    return (std::tie(lhs_head, *lhs) < std::tie(rhs_head, *rhs));
}

ItemSortKey DateColumn::key(const Item &item) const
{
    return ItemSortKey{head(item), suffix(item, item.PrettyName())};
}

QString ItemlevelColumn::name() const
{
    return "ilvl";
}

QVariant ItemlevelColumn::value(const Item &item) const
{
    if (item.ilvl() > 0) {
        return item.ilvl();
    }
    return QVariant();
}
