// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "buyout.h"

#include <QRegularExpression>

const QString Buyout::m_buyout_type_error;

const Buyout::BuyoutTypeMap Buyout::m_buyout_type_as_tag = {{BUYOUT_TYPE_IGNORE, "[ignore]"},
                                                            {BUYOUT_TYPE_BUYOUT, "b/o"},
                                                            {BUYOUT_TYPE_FIXED, "price"},
                                                            {BUYOUT_TYPE_NO_PRICE, "no price"},
                                                            {BUYOUT_TYPE_CURRENT_OFFER, "c/o"},
                                                            {BUYOUT_TYPE_INHERIT, ""}};

const Buyout::BuyoutTypeMap Buyout::m_buyout_type_as_prefix = {{BUYOUT_TYPE_IGNORE, ""},
                                                               {BUYOUT_TYPE_BUYOUT, " ~b/o "},
                                                               {BUYOUT_TYPE_FIXED, " ~price "},
                                                               {BUYOUT_TYPE_CURRENT_OFFER, " ~c/o "},
                                                               {BUYOUT_TYPE_NO_PRICE, ""},
                                                               {BUYOUT_TYPE_INHERIT, ""}};

const Buyout::BuyoutSourceMap Buyout::m_buyout_source_as_tag = {{BUYOUT_SOURCE_NONE, ""},
                                                                {BUYOUT_SOURCE_MANUAL, "manual"},
                                                                {BUYOUT_SOURCE_GAME, "game"},
                                                                {BUYOUT_SOURCE_AUTO, "auto"}};

bool Buyout::IsValid() const
{
    switch (type) {
    case BUYOUT_TYPE_IGNORE:
    case BUYOUT_TYPE_INHERIT:
    case BUYOUT_TYPE_NO_PRICE:
        return true;
    case BUYOUT_TYPE_BUYOUT:
    case BUYOUT_TYPE_FIXED:
    case BUYOUT_TYPE_CURRENT_OFFER:
        return (currency != Currency::CURRENCY_NONE) && (source != BUYOUT_SOURCE_NONE);
    default:
        spdlog::error("Invalid buyout type: {}", static_cast<int>(type));
        return false;
    }
}

bool Buyout::IsActive() const
{
    return IsValid() && type != BUYOUT_TYPE_INHERIT;
}

bool Buyout::IsPostable() const
{
    return ((source != BUYOUT_SOURCE_GAME) && (IsPriced() || (type == BUYOUT_TYPE_NO_PRICE)));
}

bool Buyout::IsPriced() const
{
    return (type == BUYOUT_TYPE_BUYOUT || type == BUYOUT_TYPE_FIXED
            || type == BUYOUT_TYPE_CURRENT_OFFER);
}

bool Buyout::IsGameSet() const
{
    return (source == BUYOUT_SOURCE_GAME);
}

bool Buyout::RequiresRefresh() const
{
    return !(type == BUYOUT_TYPE_IGNORE || type == BUYOUT_TYPE_INHERIT);
}

BuyoutSource Buyout::TagAsBuyoutSource(QString tag)
{
    auto &m = m_buyout_source_as_tag;
    auto const &it = std::find_if(m.begin(), m.end(), [&](BuyoutSourceMap::value_type const &x) {
        return x.second == tag;
    });
    return (it != m.end()) ? it->first : BUYOUT_SOURCE_NONE;
}

BuyoutType Buyout::TagAsBuyoutType(QString tag)
{
    auto &m = m_buyout_type_as_tag;
    auto const &it = std::find_if(m.begin(), m.end(), [&](BuyoutTypeMap::value_type const &x) {
        return x.second == tag;
    });
    return (it != m.end()) ? it->first : BUYOUT_TYPE_INHERIT;
}

BuyoutType Buyout::IndexAsBuyoutType(int index)
{
    if (index >= 0) {
        size_t index_t = (size_t) index;
        if (index_t < m_buyout_type_as_tag.size()) {
            return static_cast<BuyoutType>(index_t);
        }
    }

    spdlog::warn("Buyout type index out of bounds: {}. This should never happen - please report.",
                 index);
    return BUYOUT_TYPE_INHERIT;
}

QString Buyout::AsText() const
{
    if (IsPriced()) {
        // 'g' with wide precision: QString::number(double) defaults to six
        // significant digits, turning large prices into scientific notation.
        return BuyoutTypeAsTag() + " " + QString::number(value, 'g', 15) + " " + CurrencyAsTag();
    } else {
        return BuyoutTypeAsTag();
    }
}

const QString &Buyout::BuyoutTypeAsTag() const
{
    auto const &it = m_buyout_type_as_tag.find(type);
    if (it != m_buyout_type_as_tag.end()) {
        return it->second;
    } else {
        spdlog::warn(
            "No mapping from buyout type: {} to tag. This should never happen - please report.",
            type);
        return m_buyout_type_error;
    }
}

const QString &Buyout::BuyoutTypeAsPrefix() const
{
    auto const &it = m_buyout_type_as_prefix.find(type);
    if (it != m_buyout_type_as_prefix.end()) {
        return it->second;
    } else {
        spdlog::warn(
            "No mapping from buyout type: {} to prefix. This should never happen - please report.",
            type);
        return m_buyout_type_error;
    }
}

const QString &Buyout::BuyoutSourceAsTag() const
{
    auto const &it = m_buyout_source_as_tag.find(source);
    if (it != m_buyout_source_as_tag.end()) {
        return it->second;
    } else {
        spdlog::warn(
            "No mapping from buyout source: {} to tag. This should never happen - please report.",
            source);
        return m_buyout_type_error;
    }
}

const QString &Buyout::CurrencyAsTag() const
{
    return currency.AsTag();
}

bool Buyout::operator==(const Buyout &o) const
{
    static const double eps = 1e-6;
    return std::fabs(o.value - value) < eps && o.type == type && o.currency == currency
           && o.inherited == inherited && o.source == source;
}

bool Buyout::operator!=(const Buyout &o) const
{
    return !(o == *this);
}

Buyout ParseGamePrice(const QString &text)
{
    static const QRegularExpression exp("(~\\S+)\\s+(\\d+\\.?\\d*)\\s+(\\w+)");
    static const std::map<QString, Buyout::BuyoutType> types = {
        {"~gb/o", Buyout::BUYOUT_TYPE_BUYOUT},
        {"~b/o", Buyout::BUYOUT_TYPE_BUYOUT},
        {"~c/o", Buyout::BUYOUT_TYPE_CURRENT_OFFER},
        {"~price", Buyout::BUYOUT_TYPE_FIXED},
    };
    if (text.isEmpty()) {
        return Buyout();
    }
    const QRegularExpressionMatch m = exp.match(text);
    if (!m.hasMatch()) {
        return Buyout();
    }
    const auto type = types.find(m.captured(1));
    Buyout parsed;
    parsed.type = (type != types.end()) ? type->second : Buyout::BUYOUT_TYPE_INHERIT;
    parsed.value = m.captured(2).toDouble();
    parsed.currency = Currency::FromString(m.captured(3));
    parsed.source = Buyout::BUYOUT_SOURCE_GAME;
    return parsed.IsActive() ? parsed : Buyout();
}
