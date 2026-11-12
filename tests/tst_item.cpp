#include <QtTest/QtTest>

#include <cmath>
#include <memory>

#include "item.h"
#include "modlist.h"
#include "testfixtures.h"

// Item is built once from a wire item; these tests pin what its constructor
// derives from the game's own formats.
class ItemTest : public QObject
{
    Q_OBJECT

private slots:
    void sameModInTwoSectionsIsSummed();
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

QTEST_GUILESS_MAIN(ItemTest)

#include "tst_item.moc"
