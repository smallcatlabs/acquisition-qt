// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>
#include <vector>

#include "util/spdlog_qt.h"

// One currency, in one place.
struct CurrencyInfo
{
    // The persistence key in both buyout tables, in the currency dialog's
    // saved ratios and in the legacy importer, and the word posted to the
    // forum. Never changed once a currency is in the table.
    QString tag;
    // The display name, which is also the item name the currency dialog
    // counts.
    QString name;
    // The words a price note may name it by, matched in any case: the
    // game's own word (trade-ground-truth T9) and other spellings.
    QStringList words;
    // The Price column sorts by currency on this first (higher is worth
    // more).
    int rank;
    // How many Scrolls of Wisdom one is worth in the currency dialog's
    // wisdom total; 0 when it is not counted there.
    int wisdom;
    // Whether a price can be set in it: it is offered in the price editor,
    // posted to the forum, and listed in the currency dialog. The others
    // are only words read from notes the game writes (game prices are never
    // posted).
    bool choosable;
};

// The currencies of one game. A table is a value: one per realm could be
// chosen; there is one today. Its order is part of the stored data: the
// currency history is positional, so currencies are only ever appended.
class CurrencyTable
{
public:
    explicit CurrencyTable(std::vector<CurrencyInfo> rows);

    // The Path of Exile 1 table.
    static const CurrencyTable &Default();

    size_t size() const { return m_rows.size(); }
    const CurrencyInfo &at(size_t row) const { return m_rows.at(row); }
    const std::vector<CurrencyInfo> &rows() const { return m_rows; }

    std::optional<size_t> findTag(QStringView tag) const;
    // A word from a price note, in any case.
    std::optional<size_t> findWord(QStringView word) const;

private:
    std::vector<CurrencyInfo> m_rows;
};

// A currency, or none. The value is the currency's position in the default
// table plus one; 0 is no currency. The named values are the currencies the
// application has always had (the first 19 rows); later rows have no name.
struct Currency
{
    Q_GADGET;

public:
    enum CurrencyType : int {
        CURRENCY_NONE,
        CURRENCY_ORB_OF_ALTERATION,
        CURRENCY_ORB_OF_FUSING,
        CURRENCY_ORB_OF_ALCHEMY,
        CURRENCY_CHAOS_ORB,
        CURRENCY_GCP,
        CURRENCY_EXALTED_ORB,
        CURRENCY_CHROMATIC_ORB,
        CURRENCY_JEWELLERS_ORB,
        CURRENCY_ORB_OF_CHANCE,
        CURRENCY_CARTOGRAPHERS_CHISEL,
        CURRENCY_ORB_OF_SCOURING,
        CURRENCY_BLESSED_ORB,
        CURRENCY_ORB_OF_REGRET,
        CURRENCY_REGAL_ORB,
        CURRENCY_DIVINE_ORB,
        CURRENCY_VAAL_ORB,
        CURRENCY_PERANDUS_COIN,
        CURRENCY_MIRROR_OF_KALANDRA,
        CURRENCY_SILVER_COIN,
    };
    Q_ENUM(CurrencyType)

    Currency() = default;
    Currency(CurrencyType in_type)
        : type(in_type) {};

    CurrencyType type{CurrencyType::CURRENCY_NONE};

    // The currency in a row of the default table.
    static Currency AtRow(size_t row);
    // A stored tag; an unknown tag is no currency.
    static Currency FromTag(const QString &tag);
    // A word from a price note, in any case; an unknown word is no currency.
    // A hyphenated word that is not known is read as its part before the
    // first hyphen ("chaos-each" is chaos), as it was before hyphens were
    // read at all.
    static Currency FromWord(QStringView word);

    bool IsNone() const { return type == CURRENCY_NONE; }
    // The table row; null for no currency.
    const CurrencyInfo *Info() const;

    bool operator==(const Currency &rhs) const { return type == rhs.type; }
    bool operator!=(const Currency &rhs) const { return type != rhs.type; }
    // Table order (the shop groups prices by it).
    bool operator<(const Currency &rhs) const { return type < rhs.type; }

    // Empty for no currency.
    const QString &AsString() const;
    const QString &AsTag() const;
    // The rank from the table; 0 for no currency.
    int AsRank() const;
    // What the Price column sorts on first: by rank, the unranked currencies
    // in table order below every ranked one, and no currency below all.
    int SortKey() const;
};

using CurrencyType = Currency::CurrencyType;
template<>
struct fmt::formatter<CurrencyType, char> : QtEnumFormatter<CurrencyType>
{};
