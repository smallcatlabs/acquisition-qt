// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QComboBox>
#include <QTest>

#include <algorithm>

#include "currency.h"
#include "testsuite.h"
#include "ui/widgetutil.h"
#include "util/util.h"

// The currency table (#301, #273, #38).
class CurrencyTest : public QObject
{
    Q_OBJECT

private slots:
    void existingCurrenciesKeepTagsNamesAndOrder();
    void currenciesSortByWorth();
    void comboCarriesTheTag();
    void comboOffersOnlyChoosableCurrencies();
    void gameWordsAreRecognised_data();
    void gameWordsAreRecognised();
    void wordsMatchInAnyCase();
    void aPriceIsWrittenWithAWordANoteReads();
    void onlyJewellersIsWrittenDifferentlyFromItsKey();
};

namespace {

    struct Existing
    {
        Currency::CurrencyType type;
        const char *tag;
        const char *name;
    };

    // The 19 currencies the application has always had, in their order
    // (positional in the currency history), with the tags stored in users'
    // databases and posted to the forum, and their names.
    const std::vector<Existing> &existing()
    {
        static const std::vector<Existing> rows = {
            {Currency::CURRENCY_ORB_OF_ALTERATION, "alt", "Orb of Alteration"},
            {Currency::CURRENCY_ORB_OF_FUSING, "fuse", "Orb of Fusing"},
            {Currency::CURRENCY_ORB_OF_ALCHEMY, "alch", "Orb of Alchemy"},
            {Currency::CURRENCY_CHAOS_ORB, "chaos", "Chaos Orb"},
            {Currency::CURRENCY_GCP, "gcp", "Gemcutter's Prism"},
            {Currency::CURRENCY_EXALTED_ORB, "exa", "Exalted Orb"},
            {Currency::CURRENCY_CHROMATIC_ORB, "chrom", "Chromatic Orb"},
            {Currency::CURRENCY_JEWELLERS_ORB, "jew", "Jeweller's Orb"},
            {Currency::CURRENCY_ORB_OF_CHANCE, "chance", "Orb of Chance"},
            {Currency::CURRENCY_CARTOGRAPHERS_CHISEL, "chisel", "Cartographer's Chisel"},
            {Currency::CURRENCY_ORB_OF_SCOURING, "scour", "Orb of Scouring"},
            {Currency::CURRENCY_BLESSED_ORB, "blessed", "Blessed Orb"},
            {Currency::CURRENCY_ORB_OF_REGRET, "regret", "Orb of Regret"},
            {Currency::CURRENCY_REGAL_ORB, "regal", "Regal Orb"},
            {Currency::CURRENCY_DIVINE_ORB, "divine", "Divine Orb"},
            {Currency::CURRENCY_VAAL_ORB, "vaal", "Vaal Orb"},
            {Currency::CURRENCY_PERANDUS_COIN, "coin", "Perandus Coin"},
            {Currency::CURRENCY_MIRROR_OF_KALANDRA, "mirror", "Mirror of Kalandra"},
            {Currency::CURRENCY_SILVER_COIN, "silver", "Silver Coin"},
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

// #38: the Price column's order of currencies, most valuable first, as the
// maintainer ruled on November 27, 2026: the order of their values in the
// current challenge league, from an economy snapshot taken that day, with
// equal values ordered by their values in Standard.
void CurrencyTest::currenciesSortByWorth()
{
    const QStringList order = {
        "Mirror of Kalandra",
        "Divine Orb",
        "Exceptional Eldritch Ichor",
        "Exceptional Eldritch Ember",
        "Grand Eldritch Ember",
        "Orb of Annulment",
        "Grand Eldritch Ichor",
        "Gemcutter's Prism",
        "Chromatic Orb",
        "Greater Eldritch Ember",
        "Exalted Orb",
        "Chaos Orb",
        "Orb of Regret",
        "Glassblower's Bauble",
        "Orb of Scouring",
        "Orb of Chance",
        "Vaal Orb",
        "Greater Eldritch Ichor",
        "Lesser Eldritch Ember",
        "Orb of Augmentation",
        "Regal Orb",
        "Lesser Eldritch Ichor",
        "Orb of Alteration",
        "Armourer's Scrap",
        "Orb of Fusing",
        "Blacksmith's Whetstone",
        "Orb of Alchemy",
        "Blessed Orb",
        "Portal Scroll",
        "Orb of Transmutation",
        "Scroll of Wisdom",
        "Jeweller's Orb",
        // Offered by the game's price dialog, but not currencies with a
        // market value: below the ones that have one, the greater offering
        // first (the table's later row).
        "Tribute to the Goddess",
        "Gift to the Goddess",
        "Dedication to the Goddess",
        "Offering to the Goddess",
        // No longer in the game, or no longer something an item can be
        // priced in: below every other, the table's later row first.
        "Infused Engineer's Orb",
        "Engineer's Orb",
        "Facetor's Lens",
        "Silver Coin",
        "Perandus Coin",
        "Cartographer's Chisel",
    };
    const auto &table = CurrencyTable::Default();
    QCOMPARE(static_cast<size_t>(order.size()), table.size());

    QStringList sorted;
    for (const CurrencyInfo &row : table.rows()) {
        sorted.append(row.name);
    }
    const auto key = [&table](const QString &name) {
        for (size_t row = 0; row < table.size(); ++row) {
            if (table.at(row).name == name) {
                return Currency::AtRow(row).SortKey();
            }
        }
        return -1;
    };
    std::sort(sorted.begin(), sorted.end(), [&key](const QString &a, const QString &b) {
        return key(a) > key(b);
    });
    QCOMPARE(sorted, order);

    // No two share a key, and an item with no price sorts below them all.
    for (qsizetype i = 1; i < order.size(); ++i) {
        QVERIFY2(key(order[i - 1]) > key(order[i]), qPrintable(order[i]));
    }
    QVERIFY(key(order.last()) > Currency().SortKey());
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

// The combo offers the currencies a price can be set in (the original 19,
// which the forum reads), not those only read from notes (T32: the trade
// site does not even read `facetors`).
void CurrencyTest::comboOffersOnlyChoosableCurrencies()
{
    QComboBox combo;
    Util::PopulateBuyoutCurrencyComboBox(&combo);
    QCOMPARE(combo.count(), static_cast<int>(existing().size()) + 1);
    QCOMPARE(combo.findData(QString("annul")), -1);
    QCOMPARE(combo.findData(QString("facetors")), -1);
    QVERIFY(!Currency::FromWord(u"annul").IsNone());
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

// Whatever word a price is written with, a price note in that word names
// the same currency: what the application posts, it can read back.
void CurrencyTest::aPriceIsWrittenWithAWordANoteReads()
{
    const CurrencyTable &table = CurrencyTable::Default();
    QVERIFY(table.size() >= existing().size());
    for (size_t row = 0; row < table.size(); ++row) {
        const Currency currency = Currency::AtRow(row);
        QVERIFY2(!currency.AsWord().isEmpty(), qPrintable(currency.AsString()));
        QVERIFY2(Currency::FromWord(currency.AsWord()) == currency, qPrintable(currency.AsWord()));
    }
}

// #426. A Jeweller's price is written `jewellers`, the game's own word (T9),
// and still stored under `jew`. The ruling was for this currency alone: the
// others are written with their key, as they always were.
void CurrencyTest::onlyJewellersIsWrittenDifferentlyFromItsKey()
{
    const Currency jewellers(Currency::CURRENCY_JEWELLERS_ORB);
    QCOMPARE(jewellers.AsWord(), QString("jewellers"));
    QCOMPARE(jewellers.AsTag(), QString("jew"));
    QVERIFY(Currency::FromTag("jew") == jewellers);

    const CurrencyTable &table = CurrencyTable::Default();
    for (size_t row = 0; row < table.size(); ++row) {
        const Currency currency = Currency::AtRow(row);
        if (currency != jewellers) {
            QCOMPARE(currency.AsWord(), currency.AsTag());
        }
    }
}

ACQ_TEST_MAIN(CurrencyTest)

#include "tst_currency.moc"
