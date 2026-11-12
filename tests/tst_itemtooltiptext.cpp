// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "testfixtures.h"
#include "ui/itemtooltiptext.h"

class ItemTooltipTextTest : public QObject
{
    Q_OBJECT

private slots:
    void rareWithImplicitAndExplicitMods();
    void corruptedUnidentifiedItem();
    void displayMode3Property();
    void talismanRequirements();
    void vestigialAndDesecratedModsAreShown();
};

void ItemTooltipTextTest::rareWithImplicitAndExplicitMods()
{
    const Item item = makeTestItem(R"json({
        "baseType": "Stygian Vise",
        "explicitMods": [
            {"description": "+8 to Strength"},
            {"description": "+26 to maximum Energy Shield"},
            {"description": "+98 to maximum Life"},
            {"description": "+26 to maximum Mana"},
            {"description": "+48% to Lightning Resistance"},
            {"description": "+19% to Fire and Cold Resistances", "flags": {"crafted": true}}
        ],
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "rare-implicit-explicit",
        "identified": true,
        "ilvl": 83,
        "implicitMods": [{"description": "Has 1 Abyssal Socket"}],
        "inventoryId": "Belt",
        "name": "Kraken Shackle",
        "properties": [
            {
                "displayMode": 0,
                "name": "Quality (Attribute Modifiers)",
                "type": 6,
                "values": [["+5%", 1]]
            }
        ],
        "requirements": [
            {
                "displayMode": 0,
                "name": "Level",
                "type": 62,
                "values": [["67", 0]]
            }
        ],
        "typeLine": "Stygian Vise",
        "verified": false,
        "w": 2,
        "x": 0,
        "y": 0
    })json",
                                   makeTestStashLocation());

    const QString expected = "<center>"
                             "Quality (Attribute Modifiers): <font color='#88f'>+5%</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "Requires Level <font color='#fff'>67</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>Has 1 Abyssal Socket</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>+8 to Strength<br>"
                             "+26 to maximum Energy Shield<br>"
                             "+98 to maximum Life<br>"
                             "+26 to maximum Mana<br>"
                             "+48% to Lightning Resistance</font><br>"
                             "<font color='#b4b4ff'>+19% to Fire and Cold Resistances</font>"
                             "</center>";
    QCOMPARE(GenerateItemInfo(item, "Rare", true), expected);
}

void ItemTooltipTextTest::corruptedUnidentifiedItem()
{
    const Item item = makeTestItem(R"json({
        "baseType": "Breach Ring",
        "corrupted": true,
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "corrupted-unidentified",
        "identified": false,
        "ilvl": 82,
        "implicitMods": [{"description": "Properties are doubled while in a Breach"}],
        "inventoryId": "Stash1",
        "name": "",
        "typeLine": "Breach Ring",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 6
    })json",
                                   makeTestStashLocation());

    const QString expected = "<center>"
                             "<font color='#88f'>Properties are doubled while in a Breach</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#d20000'>Unidentified<br>Corrupted</font>"
                             "</center>";
    QCOMPARE(GenerateItemInfo(item, "Rare", true), expected);
}

void ItemTooltipTextTest::displayMode3Property()
{
    const Item item = makeTestItem(R"json({
        "baseType": "Facetor's Lens",
        "descrText": "Right click this item then left click a gem to apply it.",
        "explicitMods": [{"description": "Adds stored experience to a gem, up to its maximum level"}],
        "frameType": 5,
        "frameTypeId": "Currency",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "display-mode-3-property",
        "identified": true,
        "ilvl": 0,
        "inventoryId": "Stash1",
        "name": "",
        "properties": [
            {
                "displayMode": 3,
                "name": "Stored Experience: {0}",
                "type": 31,
                "values": [["350000000", 0]]
            }
        ],
        "typeLine": "Facetor's Lens",
        "verified": false,
        "w": 1,
        "x": 40,
        "y": 0
    })json",
                                   makeTestStashLocation());

    const QString expected = "<center>"
                             "Stored Experience: <font color='#fff'>350000000</font>"
                             "<br><img src=':/separators/SeparatorCurrency.png'><br>"
                             "<font color='#88f'>Adds stored experience to a gem, up to its "
                             "maximum level</font>"
                             "</center>";
    QCOMPARE(GenerateItemInfo(item, "Currency", true), expected);
}

void ItemTooltipTextTest::talismanRequirements()
{
    const Item item = makeTestItem(R"json({
        "baseType": "Deep One Talisman",
        "corrupted": true,
        "enchantMods": ["Allocates Crackling Speed"],
        "explicitMods": [
            {"description": "Adds 18 to 28 Fire Damage to Attacks"},
            {"description": "27% increased Global Critical Strike Chance"},
            {"description": "+23 to maximum Energy Shield"},
            {"description": "+54 to maximum Life"},
            {"description": "+16% to all Elemental Resistances"},
            {"description": "+11% to Chaos Resistance"}
        ],
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "talisman",
        "identified": true,
        "ilvl": 71,
        "implicitMods": [{"description": "28% increased Cold Damage"}],
        "inventoryId": "Stash1",
        "name": "Doom Idol",
        "requirements": [
            {
                "displayMode": 0,
                "name": "Level",
                "type": 62,
                "values": [["48", 0]]
            }
        ],
        "typeLine": "Deep One Talisman",
        "verified": false,
        "w": 1,
        "x": 2,
        "y": 3
    })json",
                                   makeTestStashLocation());

    const QString expected = "<center>"
                             "Requires Level <font color='#fff'>48</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#b4b4ff'>Allocates Crackling Speed</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>28% increased Cold Damage</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>Adds 18 to 28 Fire Damage to Attacks<br>"
                             "27% increased Global Critical Strike Chance<br>"
                             "+23 to maximum Energy Shield<br>"
                             "+54 to maximum Life<br>"
                             "+16% to all Elemental Resistances<br>"
                             "+11% to Chaos Resistance</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#d20000'>Corrupted</font>"
                             "</center>";
    QCOMPARE(GenerateItemInfo(item, "Rare", true), expected);
}

// #266: a desecrated mod is shown in the section it arrived in; a vestigial
// mod in a section of its own after the implicits. Neither has styling of
// its own.
void ItemTooltipTextTest::vestigialAndDesecratedModsAreShown()
{
    const Item item = makeTestItem(R"json({
        "baseType": "Leather Belt",
        "explicitMods": [
            {"description": "+80 to maximum Life", "flags": {"desecrated": true}},
            {"description": "+48% to Lightning Resistance"}
        ],
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "flagged",
        "identified": true,
        "ilvl": 83,
        "implicitMods": [
            {"description": "Has 1 Abyssal Socket"},
            {"description": "+30 to maximum Life", "flags": {"vestigial": true}}
        ],
        "inventoryId": "Belt",
        "name": "Kraken Shackle",
        "typeLine": "Leather Belt",
        "verified": false,
        "w": 2,
        "x": 0,
        "y": 0
    })json",
                                   makeTestStashLocation());

    const QString expected = "<center>"
                             "<font color='#88f'>Has 1 Abyssal Socket</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>+30 to maximum Life</font>"
                             "<br><img src=':/separators/SeparatorRare.png'><br>"
                             "<font color='#88f'>+80 to maximum Life<br>"
                             "+48% to Lightning Resistance</font>"
                             "</center>";
    QCOMPARE(GenerateItemInfo(item, "Rare", true), expected);
}

QTEST_GUILESS_MAIN(ItemTooltipTextTest)

#include "tst_itemtooltiptext.moc"
