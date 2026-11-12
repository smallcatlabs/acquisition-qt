#include <QtTest/QtTest>

#include <cmath>
#include <memory>

#include "item.h"
#include "modlist.h"
#include "testfixtures.h"
#include "util/util.h"

// Item is built once from a wire item; these tests pin what its constructor
// derives from the game's own formats.
class ItemTest : public QObject
{
    Q_OBJECT

private slots:
    void sameModInTwoSectionsIsSummed();
    void numericPropertiesKeepDisplayStrings();
    void stackSizeCount();
    void matchModWritesOutputOnMismatch();
    void requirementNamesAreCanonical();
    void catalystQualityCountsAsQuality();
};

// The fixture shape of tst_filters.cpp's makeFilterItem: a minimal wire item
// with extra JSON appended.
static std::shared_ptr<Item> makeItem(const QString &id, const QString &extraJson)
{
    const QByteArray json = QString(R"json({
        "baseType": "Leather Belt",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "%1",
        "identified": true,
        "ilvl": 84,
        "name": "Test Item",
        "typeLine": "Leather Belt",
        "verified": false,
        "w": 2,
        "x": 0,
        "y": 0%2
    })json")
                                .arg(id, extraJson)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), makeTestStashLocation()));
}

// #262: the same mod on two sections adds up. Pseudo-mods are computed per
// mod line, independently of the natural-mod entry, so the sum must not
// reach a pseudo total twice.
void ItemTest::sameModInTwoSectionsIsSummed()
{
    InitStatTranslations();
    AddStatTranslations(R"json([
        {"English":[{"format":["+#"],"string":"{0} to maximum Life"}]},
        {"English":[{"format":["+#"],"string":"{0}% to Fire Resistance"}]}
    ])json");
    InitModList();

    const auto item = makeItem("life-twice", R"json(,
        "implicitMods": [{"description": "+30 to maximum Life"},
                         {"description": "+10% to Fire Resistance"}],
        "explicitMods": [{"description": "+80 to maximum Life"},
                         {"description": "+20% to Fire Resistance"}]
    )json");

    const ModTable &table = item->mod_table();
    QCOMPARE(table.at("+# to maximum Life"), 110.0);
    QCOMPARE(table.at("+#% to Fire Resistance"), 30.0);

    // Pseudo totals that include these lines count each line once.
    QCOMPARE(table.at("+#% total to Fire Resistance"), 30.0);
    QCOMPARE(table.at("+#% total Elemental Resistance"), 30.0);
    QCOMPARE(table.at("+#% total Resistance"), 30.0);
    // No pseudo-mod in pseudomods.cpp sums maximum Life; if one is added,
    // it must read 110 here, not 220.
    for (const auto &[name, value] : table) {
        if (name.contains("total") && name.contains("Life")) {
            QCOMPARE(value, 110.0);
        }
    }
}

// #229: property values arrive in display form ("8.00%", "+20%"). The
// display strings stay as they are for the columns and the tooltip; the
// numeric view carries the parsed number.
void ItemTest::numericPropertiesKeepDisplayStrings()
{
    const auto item = makeItem("weapon", R"json(,
        "properties": [
            {"displayMode": 0, "name": "Quality", "values": [["+20%", 1]]},
            {"displayMode": 0, "name": "Physical Damage", "values": [["35-120", 1]]},
            {"displayMode": 0, "name": "Critical Strike Chance", "values": [["8.00%", 1]]},
            {"displayMode": 0, "name": "Attacks per Second", "values": [["1.50", 1]]},
            {"displayMode": 0, "name": "Chance to Block", "values": [["23%", 0]]},
            {"displayMode": 0, "name": "Level", "values": [["20 (Max)", 0]]}
        ])json");

    // Display strings: unchanged, including the existing Quality and Level
    // normalisation.
    const auto &display = item->properties();
    QCOMPARE(display.at("Critical Strike Chance"), QString("8.00%"));
    QCOMPARE(display.at("Chance to Block"), QString("23%"));
    QCOMPARE(display.at("Quality"), QString("20"));
    QCOMPARE(display.at("Level"), QString("20"));
    QCOMPARE(display.at("Physical Damage"), QString("35-120"));

    const auto &numeric = item->numeric_properties();
    QCOMPARE(numeric.at("Critical Strike Chance"), 8.0);
    QCOMPARE(numeric.at("Chance to Block"), 23.0);
    QCOMPARE(numeric.at("Quality"), 20.0);
    QCOMPARE(numeric.at("Level"), 20.0);
    QCOMPARE(numeric.at("Attacks per Second"), 1.5);
    // A range is not a number; it is left out rather than read as 0.
    QVERIFY(!numeric.contains("Physical Damage"));
}

// #229: the count comes from the part before the slash. A value without a
// slash is not a format we have seen from the game; it must not trip
// QString::first(-1).
void ItemTest::stackSizeCount()
{
    const auto stack = makeItem("stack", R"json(,
        "properties": [
            {"displayMode": 0, "name": "Stack Size", "values": [["123/5000", 0]]}
        ])json");
    QCOMPARE(stack->count(), 123);

    const auto noSlash = makeItem("no-slash", R"json(,
        "properties": [
            {"displayMode": 0, "name": "Stack Size", "values": [["7", 0]]}
        ])json");
    QCOMPARE(noSlash->count(), 7);
}

// #229: AddModToTable sums MatchMod's output into pseudo-mods whether or not
// it matched, so MatchMod writes its output on every path.
void ItemTest::matchModWritesOutputOnMismatch()
{
    double value = 42.0;
    QVERIFY(!Util::MatchMod("+#% to Fire Resistance", "+12% to Cold Resistance", &value));
    QCOMPARE(value, 0.0);

    // A line that matches the template up to its end and then runs on also
    // reports 0, not the partial value.
    value = 42.0;
    QVERIFY(!Util::MatchMod("+# to maximum Life", "+80 to maximum Life and Mana", &value));
    QCOMPARE(value, 0.0);
}

// #263: both spellings of a stat requirement land on the short key; the
// display list keeps the name the game sent.
void ItemTest::requirementNamesAreCanonical()
{
    const auto item = makeItem("requirements", R"json(,
        "requirements": [
            {"displayMode": 0, "name": "Level", "values": [["68", 0]]},
            {"displayMode": 1, "name": "Strength", "values": [["111", 0]]},
            {"displayMode": 1, "name": "Dex", "values": [["98", 0]]},
            {"displayMode": 1, "name": "Intelligence", "values": [["242", 0]]}
        ])json");

    const auto &requirements = item->requirements();
    QCOMPARE(requirements.size(), size_t(4));
    QCOMPARE(requirements.at("Level"), 68);
    QCOMPARE(requirements.at("Str"), 111);
    QCOMPARE(requirements.at("Dex"), 98);
    QCOMPARE(requirements.at("Int"), 242);

    const auto &text = item->text_requirements();
    QCOMPARE(text.size(), size_t(4));
    QCOMPARE(text[1].name, QString("Strength"));
    QCOMPARE(text[3].name, QString("Intelligence"));
}

// #263: catalyst quality is a "Quality (<kind>)" property. The numeric view
// reports it as Quality; the display strings keep the game's name, and a
// plain Quality property wins over it.
void ItemTest::catalystQualityCountsAsQuality()
{
    const auto catalyst = makeItem("catalyst", R"json(,
        "properties": [
            {"displayMode": 0, "name": "Stack Size", "values": [["3/10", 0]]},
            {"displayMode": 0, "name": "Quality (Attribute Modifiers)", "values": [["+20%", 1]]}
        ])json");
    QCOMPARE(catalyst->numeric_properties().at("Quality"), 20.0);
    QCOMPARE(catalyst->numeric_properties().at("Quality (Attribute Modifiers)"), 20.0);
    QVERIFY(!catalyst->properties().contains("Quality"));
    QCOMPARE(catalyst->properties().at("Quality (Attribute Modifiers)"), QString("+20%"));

    for (const bool plainFirst : {true, false}) {
        const QString plain
            = R"json({"displayMode": 0, "name": "Quality", "values": [["+5%", 1]]})json";
        const QString typed
            = R"json({"displayMode": 0, "name": "Quality (Attribute Modifiers)", "values": [["+20%", 1]]})json";
        const auto both = makeItem("both",
                                   QString(R"json(, "properties": [%1, %2])json")
                                       .arg(plainFirst ? plain : typed, plainFirst ? typed : plain));
        QCOMPARE(both->numeric_properties().at("Quality"), 5.0);
    }
}

QTEST_GUILESS_MAIN(ItemTest)

#include "tst_item.moc"
