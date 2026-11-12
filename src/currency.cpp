// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "currency.h"

#include <utility>

namespace {

    // Currencies added after the first 19 have no rank from the maintainer
    // yet: they share this one, below every ranked currency, and sort among
    // themselves in table order (see Currency::SortKey). Replace it with real
    // ranks when they are known.
    constexpr int kUnrankedRank = 0;

    // Empty but not null: a tag is bound into NOT NULL columns, and "no
    // currency" has always been stored as "".
    const QString kEmpty("");

} // namespace

CurrencyTable::CurrencyTable(std::vector<CurrencyInfo> rows)
    : m_rows(std::move(rows))
{}

const CurrencyTable &CurrencyTable::Default()
{
    // The first 19 rows are the currencies the application has always had,
    // in their old order, with their stored tags; the history is positional,
    // so new currencies are appended. The words include the game's own
    // (trade-ground-truth T9). Ranks are the old ones. Wisdom values are the
    // currency dialog's (Scroll of Wisdom equivalents).
    // clang-format off
    static const CurrencyTable table({
        {"alt", "Orb of Alteration", {"alt", "alts", "alteration", "alterations"}, 2, 0},
        {"fuse", "Orb of Fusing", {"fusing", "fuse", "fuses", "fusings"}, 7, 0},
        {"alch", "Orb of Alchemy", {"alch", "alchs", "alchemy"}, 8, 0},
        {"chaos", "Chaos Orb", {"chaos"}, 11, 0},
        {"gcp", "Gemcutter's Prism", {"gcp", "gcps", "gemcutter", "gemcutters"}, 15, 0},
        {"exa", "Exalted Orb", {"exalted", "exa"}, 17, 0},
        {"chrom", "Chromatic Orb", {"chrome", "chrom", "chromes", "chromatic", "chromatics"}, 1, 0},
        {"jew", "Jeweller's Orb", {"jewellers", "jew", "jewel", "jewels", "jeweler", "jewelers"}, 3, 0},
        {"chance", "Orb of Chance", {"chance"}, 4, 0},
        {"chisel", "Cartographer's Chisel", {"chisel", "chisels", "cartographer", "cartographers"}, 5, 0},
        {"scour", "Orb of Scouring", {"scour", "scours", "scouring"}, 10, 0},
        {"blessed", "Blessed Orb", {"blessed"}, 9, 0},
        {"regret", "Orb of Regret", {"regret", "regrets"}, 12, 0},
        {"regal", "Regal Orb", {"regal", "regals"}, 13, 0},
        {"divine", "Divine Orb", {"divine", "divines"}, 16, 0},
        {"vaal", "Vaal Orb", {"vaal"}, 14, 0},
        {"coin", "Perandus Coin", {"coin", "coins", "perandus", "p", "shekel"}, 6, 0},
        {"mirror", "Mirror of Kalandra", {"mirror", "mir"}, 18, 0},
        {"silver", "Silver Coin", {"silver"}, 19, 0},
        // Added from T9: the tag is the game's word.
        {"annul", "Orb of Annulment", {"annul"}, kUnrankedRank, 0},
        {"aug", "Orb of Augmentation", {"aug"}, kUnrankedRank, 0},
        {"transmute", "Orb of Transmutation", {"transmute"}, kUnrankedRank, 4},
        {"wisdom", "Scroll of Wisdom", {"wisdom"}, kUnrankedRank, 1},
        {"portal", "Portal Scroll", {"portal"}, kUnrankedRank, 1},
        {"scrap", "Armourer's Scrap", {"scrap"}, kUnrankedRank, 2},
        {"whetstone", "Blacksmith's Whetstone", {"whetstone"}, kUnrankedRank, 4},
        {"bauble", "Glassblower's Bauble", {"bauble"}, kUnrankedRank, 0},
        {"offer", "Offering to the Goddess", {"offer"}, kUnrankedRank, 0},
        {"offer-dedication", "Dedication to the Goddess", {"offer-dedication"}, kUnrankedRank, 0},
        {"offer-gift", "Gift to the Goddess", {"offer-gift"}, kUnrankedRank, 0},
        {"offer-tribute", "Tribute to the Goddess", {"offer-tribute"}, kUnrankedRank, 0},
        {"lesser-ember", "Lesser Eldritch Ember", {"lesser-ember"}, kUnrankedRank, 0},
        {"greater-ember", "Greater Eldritch Ember", {"greater-ember"}, kUnrankedRank, 0},
        {"grand-ember", "Grand Eldritch Ember", {"grand-ember"}, kUnrankedRank, 0},
        {"excep-ember", "Exceptional Eldritch Ember", {"excep-ember"}, kUnrankedRank, 0},
        {"lesser-echor", "Lesser Eldritch Ichor", {"lesser-echor"}, kUnrankedRank, 0},
        {"greater-echor", "Greater Eldritch Ichor", {"greater-echor"}, kUnrankedRank, 0},
        {"grand-echor", "Grand Eldritch Ichor", {"grand-echor"}, kUnrankedRank, 0},
        {"excep-echor", "Exceptional Eldritch Ichor", {"excep-echor"}, kUnrankedRank, 0},
        {"facetors", "Facetor's Lens", {"facetors"}, kUnrankedRank, 0},
        {"engineers", "Engineer's Orb", {"engineers"}, kUnrankedRank, 0},
        {"infused-engineers-orb", "Infused Engineer's Orb", {"infused-engineers-orb"}, kUnrankedRank, 0},
    });
    // clang-format on
    return table;
}

std::optional<size_t> CurrencyTable::findTag(QStringView tag) const
{
    for (size_t row = 0; row < m_rows.size(); ++row) {
        if (m_rows[row].tag == tag) {
            return row;
        }
    }
    return std::nullopt;
}

std::optional<size_t> CurrencyTable::findWord(QStringView word) const
{
    for (size_t row = 0; row < m_rows.size(); ++row) {
        for (const QString &known : m_rows[row].words) {
            if (word.compare(known, Qt::CaseInsensitive) == 0) {
                return row;
            }
        }
    }
    return std::nullopt;
}

Currency Currency::AtRow(size_t row)
{
    return Currency(static_cast<CurrencyType>(row + 1));
}

Currency Currency::FromTag(const QString &tag)
{
    const auto row = CurrencyTable::Default().findTag(tag);
    return row ? AtRow(*row) : Currency();
}

Currency Currency::FromWord(QStringView word)
{
    const auto row = CurrencyTable::Default().findWord(word);
    return row ? AtRow(*row) : Currency();
}

const CurrencyInfo *Currency::Info() const
{
    const auto &table = CurrencyTable::Default();
    const int row = static_cast<int>(type) - 1;
    if ((row < 0) || (static_cast<size_t>(row) >= table.size())) {
        return nullptr;
    }
    return &table.at(static_cast<size_t>(row));
}

const QString &Currency::AsString() const
{
    const CurrencyInfo *info = Info();
    return info ? info->name : kEmpty;
}

const QString &Currency::AsTag() const
{
    const CurrencyInfo *info = Info();
    return info ? info->tag : kEmpty;
}

int Currency::AsRank() const
{
    const CurrencyInfo *info = Info();
    return info ? info->rank : 0;
}

int Currency::SortKey() const
{
    const CurrencyInfo *info = Info();
    if (!info) {
        return -1;
    }
    // Ranks differ between ranked currencies, so the row only orders the
    // unranked ones among themselves (kUnrankedRank), in table order.
    return (info->rank * 1000) + (static_cast<int>(type) - 1);
}
