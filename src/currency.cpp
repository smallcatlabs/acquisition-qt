// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "currency.h"

#include <utility>

namespace {

    // The ranks in the table are the order of the currencies' values in the
    // challenge league of November 27, 2026 (the maintainer's economy
    // snapshot, #38), equal values ordered by their Standard values. They
    // are an approximation: values move, and differ between leagues.

    // Offered by the game's price dialog but with no market value in that
    // snapshot (the Goddess offerings are not currencies): they share this
    // rank, below every ranked currency, a later row above an earlier one
    // (Currency::SortKey).
    constexpr int kUnrankedRank = 1;

    // No longer in the game (Cartographer's Chisel, Perandus Coin, Silver
    // Coin, the Engineer's Orbs) or no longer something an item can be
    // priced in (Facetor's Lens). They keep their rows, so that a stored
    // price in them still reads, and sort below every other currency, a
    // later row above an earlier one.
    constexpr int kRemovedRank = 0;

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
    // (trade-ground-truth T9). A Jeweller's price is written with the game's
    // word and stored under its old tag (#426); the other eighteen are
    // written with their tags. The ranks are explained above (#38). Wisdom
    // values are the currency dialog's (Scroll of Wisdom equivalents). Only
    // the first 19 are choosable for a price: nothing says the forum reads
    // the others' words (T32: the trade site does not read `facetors` at
    // all).
    // clang-format off
    static const CurrencyTable table({
        {"alt", "Orb of Alteration", {"alt", "alts", "alteration", "alterations"}, 11, 0, true},
        {"fuse", "Orb of Fusing", {"fusing", "fuse", "fuses", "fusings"}, 9, 0, true},
        {"alch", "Orb of Alchemy", {"alch", "alchs", "alchemy"}, 7, 0, true},
        {"chaos", "Chaos Orb", {"chaos"}, 22, 0, true},
        {"gcp", "Gemcutter's Prism", {"gcp", "gcps", "gemcutter", "gemcutters"}, 26, 0, true},
        {"exa", "Exalted Orb", {"exalted", "exa"}, 23, 0, true},
        {"chrom", "Chromatic Orb", {"chrome", "chrom", "chromes", "chromatic", "chromatics"}, 25, 0, true},
        {"jew", "Jeweller's Orb", {"jewellers", "jew", "jewel", "jewels", "jeweler", "jewelers"}, 2, 0, true, "jewellers"},
        {"chance", "Orb of Chance", {"chance"}, 18, 0, true},
        {"chisel", "Cartographer's Chisel", {"chisel", "chisels", "cartographer", "cartographers"}, kRemovedRank, 0, true},
        {"scour", "Orb of Scouring", {"scour", "scours", "scouring"}, 19, 0, true},
        {"blessed", "Blessed Orb", {"blessed"}, 6, 0, true},
        {"regret", "Orb of Regret", {"regret", "regrets"}, 21, 0, true},
        {"regal", "Regal Orb", {"regal", "regals"}, 13, 0, true},
        {"divine", "Divine Orb", {"divine", "divines"}, 32, 0, true},
        {"vaal", "Vaal Orb", {"vaal"}, 17, 0, true},
        {"coin", "Perandus Coin", {"coin", "coins", "perandus", "p", "shekel"}, kRemovedRank, 0, true},
        {"mirror", "Mirror of Kalandra", {"mirror", "mir"}, 33, 0, true},
        {"silver", "Silver Coin", {"silver"}, kRemovedRank, 0, true},
        // Added from T9, to read notes: the tag is the game's word.
        {"annul", "Orb of Annulment", {"annul"}, 28, 0, false},
        {"aug", "Orb of Augmentation", {"aug"}, 14, 0, false},
        {"transmute", "Orb of Transmutation", {"transmute"}, 4, 4, false},
        {"wisdom", "Scroll of Wisdom", {"wisdom"}, 3, 1, false},
        {"portal", "Portal Scroll", {"portal"}, 5, 1, false},
        {"scrap", "Armourer's Scrap", {"scrap"}, 10, 2, false},
        {"whetstone", "Blacksmith's Whetstone", {"whetstone"}, 8, 4, false},
        {"bauble", "Glassblower's Bauble", {"bauble"}, 20, 0, false},
        {"offer", "Offering to the Goddess", {"offer"}, kUnrankedRank, 0, false},
        {"offer-dedication", "Dedication to the Goddess", {"offer-dedication"}, kUnrankedRank, 0, false},
        {"offer-gift", "Gift to the Goddess", {"offer-gift"}, kUnrankedRank, 0, false},
        {"offer-tribute", "Tribute to the Goddess", {"offer-tribute"}, kUnrankedRank, 0, false},
        {"lesser-ember", "Lesser Eldritch Ember", {"lesser-ember"}, 15, 0, false},
        {"greater-ember", "Greater Eldritch Ember", {"greater-ember"}, 24, 0, false},
        {"grand-ember", "Grand Eldritch Ember", {"grand-ember"}, 29, 0, false},
        {"excep-ember", "Exceptional Eldritch Ember", {"excep-ember"}, 30, 0, false},
        {"lesser-echor", "Lesser Eldritch Ichor", {"lesser-echor"}, 12, 0, false},
        {"greater-echor", "Greater Eldritch Ichor", {"greater-echor"}, 16, 0, false},
        {"grand-echor", "Grand Eldritch Ichor", {"grand-echor"}, 27, 0, false},
        {"excep-echor", "Exceptional Eldritch Ichor", {"excep-echor"}, 31, 0, false},
        {"facetors", "Facetor's Lens", {"facetors"}, kRemovedRank, 0, false},
        {"engineers", "Engineer's Orb", {"engineers"}, kRemovedRank, 0, false},
        {"infused-engineers-orb", "Infused Engineer's Orb", {"infused-engineers-orb"}, kRemovedRank, 0, false},
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
    const auto &table = CurrencyTable::Default();
    auto row = table.findWord(word);
    if (!row) {
        const qsizetype hyphen = word.indexOf(u'-');
        if (hyphen > 0) {
            row = table.findWord(word.first(hyphen));
        }
    }
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

const QString &Currency::AsWord() const
{
    const CurrencyInfo *info = Info();
    if (!info) {
        return kEmpty;
    }
    return info->written.isEmpty() ? info->tag : info->written;
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
    // ones that share kUnrankedRank or kRemovedRank: a later row is higher.
    return (info->rank * 1000) + (static_cast<int>(type) - 1);
}
