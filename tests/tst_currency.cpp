// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QComboBox>
#include <QtTest/QtTest>

#include "currency.h"
#include "util/util.h"

// The currency table (#301, #273, #38).
class CurrencyTest : public QObject
{
    Q_OBJECT

private slots:
    void existingCurrenciesKeepTagsNamesAndOrder();
    void existingRanksAreKept();
    void divineRanksAboveExalted();
    void comboCarriesTheTag();
    void gameWordsAreRecognised_data();
    void gameWordsAreRecognised();
    void wordsMatchInAnyCase();
};

namespace {

    struct Existing
    {
        Currency::CurrencyType type;
        const char *tag;
        const char *name;
        int rank;
    };

    // The 19 currencies the application has always had, in their order
    // (positional in the currency history), with the tags stored in users'
    // databases and posted to the forum, their names, and their ranks
    // before #38.
    const std::vector<Existing> &existing()
    {
        static const std::vector<Existing> rows = {
            {Currency::CURRENCY_ORB_OF_ALTERATION, "alt", "Orb of Alteration", 2},
            {Currency::CURRENCY_ORB_OF_FUSING, "fuse", "Orb of Fusing", 7},
            {Currency::CURRENCY_ORB_OF_ALCHEMY, "alch", "Orb of Alchemy", 8},
            {Currency::CURRENCY_CHAOS_ORB, "chaos", "Chaos Orb", 11},
            {Currency::CURRENCY_GCP, "gcp", "Gemcutter's Prism", 15},
            {Currency::CURRENCY_EXALTED_ORB, "exa", "Exalted Orb", 17},
            {Currency::CURRENCY_CHROMATIC_ORB, "chrom", "Chromatic Orb", 1},
            {Currency::CURRENCY_JEWELLERS_ORB, "jew", "Jeweller's Orb", 3},
            {Currency::CURRENCY_ORB_OF_CHANCE, "chance", "Orb of Chance", 4},
            {Currency::CURRENCY_CARTOGRAPHERS_CHISEL, "chisel", "Cartographer's Chisel", 5},
            {Currency::CURRENCY_ORB_OF_SCOURING, "scour", "Orb of Scouring", 10},
            {Currency::CURRENCY_BLESSED_ORB, "blessed", "Blessed Orb", 9},
            {Currency::CURRENCY_ORB_OF_REGRET, "regret", "Orb of Regret", 12},
            {Currency::CURRENCY_REGAL_ORB, "regal", "Regal Orb", 13},
            {Currency::CURRENCY_DIVINE_ORB, "divine", "Divine Orb", 16},
            {Currency::CURRENCY_VAAL_ORB, "vaal", "Vaal Orb", 14},
            {Currency::CURRENCY_PERANDUS_COIN, "coin", "Perandus Coin", 6},
            {Currency::CURRENCY_MIRROR_OF_KALANDRA, "mirror", "Mirror of Kalandra", 18},
            {Currency::CURRENCY_SILVER_COIN, "silver", "Silver Coin", 19},
        };
        return rows;
    }

} // namespace

// A pin, passing before and after the table: every existing tag, name and
// position. A changed tag would orphan stored prices and repost the shop.
void CurrencyTest::existingCurrenciesKeepTagsNamesAndOrder()
{
    QComboBox combo;
    Util::PopulateBuyoutCurrencyComboBox(&combo);
    for (size_t i = 0; i < existing().size(); ++i) {
        const Existing &row = existing()[i];
        const Currency currency(row.type);
        QCOMPARE(currency.AsTag(), QString(row.tag));
        QCOMPARE(currency.AsString(), QString(row.name));
        QVERIFY(Currency::FromTag(row.tag) == currency);
        // Position i in the table, after the combo's blank "no currency".
        QCOMPARE(static_cast<size_t>(row.type), i + 1);
        QCOMPARE(combo.itemText(static_cast<int>(i) + 1), QString(row.name));
    }
    QCOMPARE(combo.itemText(0), QString());
    QVERIFY(Currency::FromTag("not-a-tag") == Currency::CURRENCY_NONE);
    // "No currency" is stored as an empty tag, never a null one: the column
    // is NOT NULL.
    QCOMPARE(Currency().AsTag(), QString(""));
    QVERIFY(!Currency().AsTag().isNull());
}

// Every rank stays as it was, except the one the maintainer ruled on (#38).
void CurrencyTest::existingRanksAreKept()
{
    for (const Existing &row : existing()) {
        if ((row.type == Currency::CURRENCY_DIVINE_ORB)
            || (row.type == Currency::CURRENCY_EXALTED_ORB)) {
            continue;
        }
        QCOMPARE(Currency(row.type).AsRank(), row.rank);
    }
}

// #38: since the 3.25 swap, a Divine Orb is worth more than an Exalted Orb.
void CurrencyTest::divineRanksAboveExalted()
{
    QVERIFY(Currency(Currency::CURRENCY_DIVINE_ORB).AsRank()
            > Currency(Currency::CURRENCY_EXALTED_ORB).AsRank());
}

// The currency box says which currency each entry is by its tag, so code
// selects an entry by tag, not by its position.
void CurrencyTest::comboCarriesTheTag()
{
    QComboBox combo;
    Util::PopulateBuyoutCurrencyComboBox(&combo);
    QCOMPARE(combo.itemData(0).toString(), QString());
    for (size_t i = 0; i < existing().size(); ++i) {
        QCOMPARE(combo.itemData(static_cast<int>(i) + 1).toString(), QString(existing()[i].tag));
    }
}

// The 39 words the game's price dialog writes into a note, with the
// currency each names (trade-ground-truth T9, transcribed).
void CurrencyTest::gameWordsAreRecognised_data()
{
    QTest::addColumn<QString>("word");
    QTest::addColumn<QString>("name");
    const std::vector<std::pair<const char *, const char *>> t9 = {
        {"chaos", "Chaos Orb"},
        {"divine", "Divine Orb"},
        {"alch", "Orb of Alchemy"},
        {"exalted", "Exalted Orb"},
        {"alt", "Orb of Alteration"},
        {"mirror", "Mirror of Kalandra"},
        {"chrome", "Chromatic Orb"},
        {"blessed", "Blessed Orb"},
        {"fusing", "Orb of Fusing"},
        {"jewellers", "Jeweller's Orb"},
        {"regal", "Regal Orb"},
        {"vaal", "Vaal Orb"},
        {"chance", "Orb of Chance"},
        {"annul", "Orb of Annulment"},
        {"aug", "Orb of Augmentation"},
        {"regret", "Orb of Regret"},
        {"scour", "Orb of Scouring"},
        {"transmute", "Orb of Transmutation"},
        {"wisdom", "Scroll of Wisdom"},
        {"portal", "Portal Scroll"},
        {"scrap", "Armourer's Scrap"},
        {"whetstone", "Blacksmith's Whetstone"},
        {"gcp", "Gemcutter's Prism"},
        {"bauble", "Glassblower's Bauble"},
        {"offer", "Offering to the Goddess"},
        {"offer-dedication", "Dedication to the Goddess"},
        {"offer-gift", "Gift to the Goddess"},
        {"offer-tribute", "Tribute to the Goddess"},
        {"lesser-ember", "Lesser Eldritch Ember"},
        {"greater-ember", "Greater Eldritch Ember"},
        {"grand-ember", "Grand Eldritch Ember"},
        {"excep-ember", "Exceptional Eldritch Ember"},
        {"lesser-echor", "Lesser Eldritch Ichor"},
        {"greater-echor", "Greater Eldritch Ichor"},
        {"grand-echor", "Grand Eldritch Ichor"},
        {"excep-echor", "Exceptional Eldritch Ichor"},
        {"facetors", "Facetor's Lens"},
        {"engineers", "Engineer's Orb"},
        {"infused-engineers-orb", "Infused Engineer's Orb"},
    };
    QCOMPARE(t9.size(), size_t(39));
    for (const auto &[word, name] : t9) {
        QTest::newRow(word) << QString(word) << QString(name);
    }
}

void CurrencyTest::gameWordsAreRecognised()
{
    QFETCH(QString, word);
    QFETCH(QString, name);
    const Currency currency = Currency::FromWord(word);
    QVERIFY2(!(currency == Currency::CURRENCY_NONE), qPrintable(word));
    QCOMPARE(currency.AsString(), name);
}

// A note typed by hand may capitalise the word.
void CurrencyTest::wordsMatchInAnyCase()
{
    QVERIFY(Currency::FromWord(u"Chaos") == Currency(Currency::CURRENCY_CHAOS_ORB));
    QVERIFY(Currency::FromWord(u"DIVINE") == Currency(Currency::CURRENCY_DIVINE_ORB));
}

QTEST_MAIN(CurrencyTest)

#include "tst_currency.moc"
