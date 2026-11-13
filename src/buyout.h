// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

#include <map>

#include "currency.h"
#include "util/spdlog_qt.h"

struct Buyout
{
    Q_GADGET
public:
    enum BuyoutType {
        BUYOUT_TYPE_IGNORE,
        BUYOUT_TYPE_BUYOUT,
        BUYOUT_TYPE_FIXED,
        BUYOUT_TYPE_CURRENT_OFFER,
        BUYOUT_TYPE_NO_PRICE,
        BUYOUT_TYPE_INHERIT,
    };
    Q_ENUM(BuyoutType)

    enum BuyoutSource {
        BUYOUT_SOURCE_NONE,
        BUYOUT_SOURCE_MANUAL,
        BUYOUT_SOURCE_GAME,
        BUYOUT_SOURCE_AUTO,
    };
    Q_ENUM(BuyoutSource)

    typedef std::map<BuyoutType, QString> BuyoutTypeMap;
    typedef std::map<BuyoutSource, QString> BuyoutSourceMap;

    Buyout() = default;
    Buyout(double m_value, BuyoutType m_type, Currency m_currency, QDateTime m_last_update)
        : value(m_value)
        , type(m_type)
        , currency(m_currency)
        , last_update(m_last_update)
    {}

    double value{0.0};
    BuyoutType type{BuyoutType::BUYOUT_TYPE_INHERIT};
    BuyoutSource source{BUYOUT_SOURCE_MANUAL};
    Currency currency{CurrencyType::CURRENCY_NONE};
    QDateTime last_update;
    bool inherited{false};

    bool operator==(const Buyout &o) const;
    bool operator!=(const Buyout &o) const;
    bool IsNull() const
    {
        return (value == 0.0) && (type == BuyoutType::BUYOUT_TYPE_INHERIT)
               && (source == BUYOUT_SOURCE_MANUAL) && (currency == CurrencyType::CURRENCY_NONE)
               && last_update.isNull() && (inherited == false);
    };
    bool IsValid() const;
    bool IsActive() const;
    bool IsInherited() const { return inherited || type == BuyoutType::BUYOUT_TYPE_INHERIT; }
    bool IsPostable() const;
    bool IsPriced() const;
    bool IsGameSet() const;
    bool RequiresRefresh() const;

    static BuyoutType TagAsBuyoutType(QString tag);
    static BuyoutType IndexAsBuyoutType(int index);
    static BuyoutSource TagAsBuyoutSource(QString tag);

    QString AsText() const;
    const QString &BuyoutTypeAsTag() const;
    const QString &BuyoutTypeAsPrefix() const;
    const QString &BuyoutSourceAsTag() const;
    const QString &CurrencyAsTag() const;

private:
    static const QString m_buyout_type_error;
    static const BuyoutTypeMap m_buyout_type_as_tag;
    static const BuyoutTypeMap m_buyout_type_as_prefix;
    static const BuyoutSourceMap m_buyout_source_as_tag;
};

// The price a game note or a tab's name sets: "~b/o 5 chaos", "~price 1.5
// divine", anything before or after it tolerated. An active buyout with the
// game's source, and no date (the game does not say when it was set); an
// inactive, null buyout when the text names no price this build can read.
Buyout ParseGamePrice(const QString &text);

using BuyoutType = Buyout::BuyoutType;
template<>
struct fmt::formatter<BuyoutType, char> : QtEnumFormatter<BuyoutType>
{};

using BuyoutSource = Buyout::BuyoutSource;
template<>
struct fmt::formatter<BuyoutSource, char> : QtEnumFormatter<BuyoutSource>
{};
