#include <QtTest/QtTest>

#include <clocale>
#include <cmath>
#include <memory>
#include <string>

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
    void resonatorAndAbyssSocketsHaveNoColour();
    void valuelessModStoresZero();
    void decimalModValueIgnoresProcessLocale();
    void ignoredFirstFormatStillSubstitutesTheRest();
    void resonatorSocketsExportAsResonators();
    void vestigialAndDesecratedModsAreKept();
};

// Real RePoE stat translations, cut from the game data (see
// tests/fixtures/repoe/README.md).
static QByteArray readRepoeFixture(const char *name)
{
    const QString path = QFINDTESTDATA(QString("fixtures/repoe/%1").arg(name));
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        qFatal("missing RePoE fixture %s", name);
    }
    return file.readAll();
}

static void loadRealStatTranslations()
{
    InitStatTranslations();
    AddStatTranslations(readRepoeFixture("stat_translations.json"));
    InitModList();
}

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

// #267: the socket attribute is decided on the whole string. "DV" (a Delve
// resonator socket) is not a Dexterity socket, and "A" (abyss) has no colour
// either; both still count as sockets and take part in links.
void ItemTest::resonatorAndAbyssSocketsHaveNoColour()
{
    const auto resonator = makeItem("resonator", R"json(,
        "sockets": [
            {"group": 0, "attr": "DV", "sColour": "DV"},
            {"group": 0, "attr": "DV", "sColour": "DV"}
        ])json");
    QCOMPARE(resonator->sockets_cnt(), 2);
    QCOMPARE(resonator->links_cnt(), 2);
    QCOMPARE(resonator->sockets().r, 0);
    QCOMPARE(resonator->sockets().g, 0);
    QCOMPARE(resonator->sockets().b, 0);
    QCOMPARE(resonator->sockets().w, 0);

    const auto belt = makeItem("abyss-belt", R"json(,
        "sockets": [
            {"group": 0, "attr": "A", "sColour": "A"}
        ])json");
    QCOMPARE(belt->sockets_cnt(), 1);
    QCOMPARE(belt->links_cnt(), 1);
    QCOMPARE(belt->sockets().g, 0);

    // The ordinary attributes are unchanged.
    const auto armour = makeItem("armour", R"json(,
        "sockets": [
            {"group": 0, "attr": "S", "sColour": "R"},
            {"group": 0, "attr": "D", "sColour": "G"},
            {"group": 1, "attr": "I", "sColour": "B"},
            {"group": 1, "attr": "G", "sColour": "W"}
        ])json");
    QCOMPARE(armour->sockets().r, 1);
    QCOMPARE(armour->sockets().g, 1);
    QCOMPARE(armour->sockets().b, 1);
    QCOMPARE(armour->sockets().w, 1);
    QCOMPARE(armour->links_cnt(), 2);
}

// #265: a mod with no number has no value to average; it stores 0, not the
// NaN of a division by a zero count, which passed every min/max.
void ItemTest::valuelessModStoresZero()
{
    InitStatTranslations();
    AddStatTranslations(R"json([
        {"English":[{"format":["ignore"],"string":"Cannot be Frozen"}]}
    ])json");
    InitModList();

    const auto item = makeItem("valueless", R"json(,
        "explicitMods": [{"description": "Cannot be Frozen"}]
    )json");
    const ModTable &table = item->mod_table();
    QVERIFY(table.contains("Cannot be Frozen"));
    QVERIFY(!std::isnan(table.at("Cannot be Frozen")));
    QCOMPARE(table.at("Cannot be Frozen"), 0.0);

    double value = 42.0;
    QVERIFY(Util::MatchMod("Cannot be Frozen", "Cannot be Frozen", &value));
    QCOMPARE(value, 0.0);
}

// #132: QCoreApplication adopts the system locale on Unix, so a process
// whose LC_NUMERIC uses a decimal comma must still read "0.26" as 0.26.
void ItemTest::decimalModValueIgnoresProcessLocale()
{
    // Restores the process locale on every exit from this test.
    struct NumericLocaleGuard
    {
        std::string saved = std::setlocale(LC_NUMERIC, nullptr);
        ~NumericLocaleGuard() { std::setlocale(LC_NUMERIC, saved.c_str()); }
    } guard;

    bool haveCommaLocale = false;
    for (const char *candidate : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"}) {
        if (std::setlocale(LC_NUMERIC, candidate) != nullptr) {
            haveCommaLocale = true;
            break;
        }
    }
    if (!haveCommaLocale) {
        QSKIP("No comma-decimal locale (de_DE or fr_FR) is installed on this machine");
    }
    QCOMPARE(std::localeconv()->decimal_point[0], ',');

    loadRealStatTranslations();
    // A line from a real ring.
    double direct = 0.0;
    QVERIFY(Util::MatchMod("#% of Physical Attack Damage Leeched as Life",
                           "0.26% of Physical Attack Damage Leeched as Life",
                           &direct));
    QCOMPARE(direct, 0.26);

    const auto item = makeItem("leech", R"json(,
        "explicitMods": [{"description": "0.26% of Physical Attack Damage Leeched as Life"}]
    )json");
    QCOMPARE(item->mod_table().at("#% of Physical Attack Damage Leeched as Life"), 0.26);
}

// #265: a translation whose first stat is not displayed ("ignore") still
// shows the others. Their placeholders become "#" like any other; they were
// left as a literal "{1}", which no item line can match.
void ItemTest::ignoredFirstFormatStillSubstitutesTheRest()
{
    loadRealStatTranslations();

    const QStringList names = mod_list_model().stringList();
    QVERIFY(names.contains("Projectiles have #% chance to Return to you"));
    QVERIFY(!names.contains("Projectiles have {1}% chance to Return to you"));
    QVERIFY(names.contains("Projectiles Return to you"));

    const auto item = makeItem("return", R"json(,
        "explicitMods": [{"description": "Projectiles have 30% chance to Return to you"}]
    )json");
    QCOMPARE(item->mod_table().at("Projectiles have #% chance to Return to you"), 30.0);
}

// #267: a Delve resonator socket ("DV") is exported to Path of Building as
// itself, not as a green socket; abyss and coloured sockets keep their
// letters.
void ItemTest::resonatorSocketsExportAsResonators()
{
    const auto resonator = makeItem("resonator", R"json(,
        "sockets": [
            {"group": 0, "attr": "DV", "sColour": "DV"},
            {"group": 0, "attr": "DV", "sColour": "DV"},
            {"group": 1, "attr": "DV", "sColour": "DV"}
        ])json");
    QVERIFY2(resonator->POBformat().contains("\nSockets: DV-DV DV\n"),
             qPrintable(resonator->POBformat()));

    const auto gear = makeItem("gear", R"json(,
        "sockets": [
            {"group": 0, "attr": "S", "sColour": "R"},
            {"group": 0, "attr": "D", "sColour": "G"},
            {"group": 1, "attr": "I", "sColour": "B"},
            {"group": 1, "attr": "G", "sColour": "W"},
            {"group": 2, "attr": "A", "sColour": "A"}
        ])json");
    QVERIFY2(gear->POBformat().contains("\nSockets: R-G B-W A\n"), qPrintable(gear->POBformat()));
}

// #266: a mod flagged vestigial or desecrated stays in the section of the
// array it arrived in, and is searchable there. Both were dropped.
void ItemTest::vestigialAndDesecratedModsAreKept()
{
    loadRealStatTranslations();

    const auto item = makeItem("flagged", R"json(,
        "implicitMods": [
            {"description": "+30 to maximum Life", "flags": {"vestigial": true}},
            {"description": "+12 to maximum Life", "flags": {"desecrated": true}}
        ],
        "explicitMods": [
            {"description": "+80 to maximum Life", "flags": {"desecrated": true}},
            {"description": "+5 to maximum Life", "flags": {"vestigial": true}},
            {"description": "0.26% of Physical Attack Damage Leeched as Life"}
        ]
    )json");

    const auto &text = item->text_mods();
    QCOMPARE(text.at("implicitMods"),
             (std::vector<QString>{"+30 to maximum Life", "+12 to maximum Life"}));
    QCOMPARE(text.at("explicitMods"),
             (std::vector<QString>{"+80 to maximum Life",
                                   "+5 to maximum Life",
                                   "0.26% of Physical Attack Damage Leeched as Life"}));
    QCOMPARE(item->mod_table().at("+# to maximum Life"), 127.0);

    const QString pob = item->POBformat();
    QVERIFY2(pob.contains("\nImplicits: 2\n+30 to maximum Life\n+12 to maximum Life\n"),
             qPrintable(pob));
    QVERIFY2(pob.contains("\n+80 to maximum Life\n+5 to maximum Life\n"), qPrintable(pob));
}

QTEST_GUILESS_MAIN(ItemTest)

#include "tst_item.moc"
