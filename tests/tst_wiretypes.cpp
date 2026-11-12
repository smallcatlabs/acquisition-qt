// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Pins for the poe:: wire types against GGG's API reference: fields GGG adds
// are read, and the meaning Acquisition gives them where an Item is built.
//
// Fixtures follow the shapes in GGG's reference (developer/docs/reference,
// 3.30.0). Where Acquisition deliberately models a field differently from the
// documented value type, the fixture uses the modeled shape and says so.

#include <QtTest/QtTest>

#include <optional>
#include <string_view>

#include "item.h"
#include "itemlocation.h"
#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/itemmod.h"
#include "poe/types/itemproperty.h"
#include "poe/types/ladderentry.h"
#include "poe/types/league.h"
#include "poe/types/leagueaccount.h"
#include "poe/types/passivenode.h"
#include "testfixtures.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/json_readers.h"

class WireTypesTest : public QObject
{
    Q_OBJECT

private slots:
    // 3.30.0
    void itemModRareOnlyFlagIsRead();
    void rareOnlyAloneDoesNotMoveAnExplicitMod();
    void rareOnlyDoesNotOverrideCrafted();
    void doubleCorruptedCountsAsCorrupted();
    void characterWickermanIsRead();
    void passiveNodeVariantFieldsAreRead();
    void passiveNodeConnectionsAreRead();
    void leagueAccountVariantsAreRead();

    // Earlier reference changes the headers had not caught up with.
    void characterSpecialisationsAreRead();
    void itemPropertyIconIsRead();
    void leagueGoalIsRead();
    void leagueAccountMercenaryIndexIsRead();
    void ladderEntryProgressIsRead();
};

namespace {

    // The production readers' options (util/json_readers.cpp): unknown keys
    // are tolerated, so a field Acquisition does not model is skipped rather
    // than failing the parse. Used for types that have no production reader.
    template<typename T>
    std::optional<T> readTolerant(std::string_view json)
    {
        static constexpr glz::opts opts{.error_on_unknown_keys = false};
        T value{};
        const auto err = glz::read<opts>(value, json);
        if (err) {
            qWarning("readTolerant: %s", glz::format_error(err, json).c_str());
            return std::nullopt;
        }
        return value;
    }

    // A minimal PoE1 item; extraJson is spliced in after the last member and
    // must start with a comma.
    std::optional<poe::Item> readItem(const QString &extraJson)
    {
        const QByteArray json = QString(R"json({
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "0000000000000000000000000000000000000000000000000000000000000001",
            "name": "Doom Bite",
            "typeLine": "Imperial Claw",
            "baseType": "Imperial Claw",
            "rarity": "Rare",
            "identified": true,
            "ilvl": 84,
            "frameType": 2,
            "frameTypeId": "Rare",
            "x": 0,
            "y": 0%1
        })json")
                                    .arg(extraJson)
                                    .toUtf8();
        return readTolerant<poe::Item>(std::string_view(json.constData(), size_t(json.size())));
    }

    std::optional<Item> buildItem(const QString &extraJson)
    {
        const auto item = readItem(extraJson);
        if (!item) {
            return std::nullopt;
        }
        return Item(*item, makeTestStashLocation());
    }

    const std::vector<QString> &textMods(const Item &item, const QString &section)
    {
        return item.text_mods().at(section);
    }

} // namespace

void WireTypesTest::itemModRareOnlyFlagIsRead()
{
    const auto mod = readTolerant<poe::ItemMod>(
        R"json({"description": "+52 to maximum Life", "flags": {"rareonly": true}})json");
    QVERIFY(mod);
    QVERIFY(mod->flags);
    QCOMPARE(mod->flags->rareonly, std::optional<bool>(true));

    // Not a routing flag (see Flags::any()). Item::LoadModifiers would still
    // put a rareonly-only mod in explicitMods if any() counted it, via its
    // "no recognized flag" fallback, so the routing pins below cannot catch
    // that on their own.
    QVERIFY(!mod->flags->any());
}

void WireTypesTest::rareOnlyAloneDoesNotMoveAnExplicitMod()
{
    // rareonly says which items a mod can roll on, not what kind of mod it
    // is, so it must not route the mod out of explicitMods (Tom's ruling,
    // 2026-11-12).
    const auto item = buildItem(
        R"json(, "explicitMods": [{"description": "+52 to maximum Life", "flags": {"rareonly": true}}])json");
    QVERIFY(item);
    QCOMPARE(textMods(*item, "explicitMods"), std::vector<QString>{"+52 to maximum Life"});
    QVERIFY(textMods(*item, "craftedMods").empty());
    QVERIFY(!item->crafted());
}

void WireTypesTest::rareOnlyDoesNotOverrideCrafted()
{
    const auto item = buildItem(
        R"json(, "explicitMods": [{"description": "+25% to Fire Resistance", "flags": {"crafted": true, "rareonly": true}}])json");
    QVERIFY(item);
    QCOMPARE(textMods(*item, "craftedMods"), std::vector<QString>{"+25% to Fire Resistance"});
    QVERIFY(textMods(*item, "explicitMods").empty());
    QVERIFY(item->crafted());
}

void WireTypesTest::doubleCorruptedCountsAsCorrupted()
{
    // Tom's ruling, 2026-11-12: a double-corrupted item is corrupted for
    // every purpose that reads Item::corrupted() (the Corrupted search
    // filter among them). The fixture deliberately omits "corrupted": the
    // reference does not say whether GGG sends both flags together, so
    // Acquisition must not depend on it.
    const auto item = buildItem(R"json(, "doubleCorrupted": true)json");
    QVERIFY(item);
    QVERIFY(item->corrupted());

    const auto plain = buildItem("");
    QVERIFY(plain);
    QVERIFY(!plain->corrupted());
}

void WireTypesTest::characterWickermanIsRead()
{
    const auto character = json::readCharacter(R"json({
        "id": "0000000000000000000000000000000000000000000000000000000000000002",
        "name": "TestWitch",
        "realm": "pc",
        "class": "Witch",
        "league": "Standard",
        "level": 90,
        "experience": 1000,
        "wickerman": [{
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "0000000000000000000000000000000000000000000000000000000000000003",
            "name": "",
            "typeLine": "Test Item",
            "baseType": "Test Item",
            "identified": true,
            "ilvl": 1,
            "frameTypeId": "Normal",
            "inventoryId": "Wickerman"
        }]
    })json");
    QVERIFY(character);
    QVERIFY(character->wickerman);
    QCOMPARE(character->wickerman->size(), size_t(1));
    QCOMPARE(character->wickerman->front().typeLine, QString("Test Item"));
}

void WireTypesTest::passiveNodeVariantFieldsAreRead()
{
    // "skill" is modeled as a string (see the note on PassiveNode::skill).
    const auto node = readTolerant<poe::PassiveNode>(R"json({
        "skill": "12345",
        "name": "Variant Notable",
        "isNotable": true,
        "isMultipleChoiceVariant": true,
        "variantEffects": [{
            "effect": 67890,
            "stats": ["10% increased Damage"],
            "reminderText": ["(Reminder)"]
        }],
        "out": [],
        "in": []
    })json");
    QVERIFY(node);
    QCOMPARE(node->isMultipleChoiceVariant, std::optional<bool>(true));
    QVERIFY(node->variantEffects);
    QCOMPARE(node->variantEffects->size(), size_t(1));
    const auto &effect = node->variantEffects->front();
    QCOMPARE(effect.effect, 67890u);
    QCOMPARE(effect.stats, std::vector<QString>{"10% increased Damage"});
    QVERIFY(effect.reminderText);
}

void WireTypesTest::passiveNodeConnectionsAreRead()
{
    // GGG documents connections[].to as "uint; skill hash". It is modeled as
    // a string, following PoE's other node identifiers and PassiveNode::skill
    // (Tom's ruling, 2026-11-12), so this fixture sends a string. GUESS: no
    // 3.30 reply has been seen yet.
    const auto node = readTolerant<poe::PassiveNode>(R"json({
        "skill": "12345",
        "out": ["222"],
        "in": [],
        "connections": [
            {"to": "222", "orbit": 0},
            {"to": "333", "orbit": 2, "orbitX": 1.5, "orbitY": -2.25}
        ]
    })json");
    QVERIFY(node);
    QVERIFY(node->connections);
    QCOMPARE(node->connections->size(), size_t(2));
    const auto &straight = node->connections->at(0);
    QCOMPARE(straight.to, QString("222"));
    QCOMPARE(straight.orbit, 0u);
    QVERIFY(!straight.orbitX);
    QVERIFY(!straight.orbitY);
    const auto &curve = node->connections->at(1);
    QCOMPARE(curve.to, QString("333"));
    QCOMPARE(curve.orbit, 2u);
    QCOMPARE(curve.orbitX, std::optional<float>(1.5f));
    QCOMPARE(curve.orbitY, std::optional<float>(-2.25f));
}

void WireTypesTest::leagueAccountVariantsAreRead()
{
    const auto account = readTolerant<poe::LeagueAccount>(R"json({
        "atlas_passive_trees": [{
            "name": "Tree 1",
            "hashes": [1, 2, 3],
            "variants": {"12345": 67890}
        }],
        "mercenaries": []
    })json");
    QVERIFY(account);
    QCOMPARE(account->atlas_passive_trees.size(), size_t(1));
    const auto &variants = account->atlas_passive_trees.front().variants;
    QCOMPARE(variants.size(), size_t(1));
    QCOMPARE(variants.at("12345"), 67890);
}

void WireTypesTest::characterSpecialisationsAreRead()
{
    // GGG spells the key "specialisations"; the member was spelled
    // "specializations", so the key was skipped as unknown.
    const auto character = json::readCharacter(R"json({
        "id": "0000000000000000000000000000000000000000000000000000000000000004",
        "name": "TestMonk",
        "realm": "pc",
        "class": "Monk",
        "level": 70,
        "experience": 1000,
        "passives": {
            "hashes": [1, 2],
            "hashes_ex": [],
            "specialisations": {"set1": [10, 11], "set2": [], "set3": [12]},
            "skill_overrides": {},
            "jewel_data": {}
        }
    })json");
    QVERIFY(character);
    QVERIFY(character->passives);
    const auto &specialisations = character->passives->specialisations;
    QVERIFY(specialisations);
    QCOMPARE(specialisations->size(), size_t(3));
    QCOMPARE(specialisations->at("set1"), (std::vector<int>{10, 11}));
    QCOMPARE(specialisations->at("set3"), std::vector<int>{12});
}

void WireTypesTest::itemPropertyIconIsRead()
{
    const auto item = readItem(R"json(, "properties": [{
        "name": "Quality",
        "values": [["+20%", 1]],
        "displayMode": 0,
        "type": 6,
        "icon": "https://web.poecdn.com/image/quality.png"
    }])json");
    QVERIFY(item);
    QVERIFY(item->properties);
    QCOMPARE(item->properties->size(), size_t(1));
    QCOMPARE(item->properties->front().icon,
             std::optional<QString>("https://web.poecdn.com/image/quality.png"));
}

void WireTypesTest::leagueGoalIsRead()
{
    const auto leagues = json::readLeagueList(R"json([{
        "id": "Standard",
        "realm": "pc",
        "goal": "Reach level 100"
    }])json");
    QVERIFY(leagues);
    QCOMPARE(leagues->size(), size_t(1));
    QCOMPARE(leagues->front().goal, std::optional<QString>("Reach level 100"));
}

void WireTypesTest::leagueAccountMercenaryIndexIsRead()
{
    const auto account = readTolerant<poe::LeagueAccount>(R"json({
        "atlas_passive_trees": [],
        "mercenaries": [{
            "index": 2,
            "name": "Mercenary",
            "level": 68,
            "build": "Archer",
            "build_hash": 12345,
            "skills": [],
            "items": []
        }]
    })json");
    QVERIFY(account);
    QCOMPARE(account->mercenaries.size(), size_t(1));
    QCOMPARE(account->mercenaries.front().index, 2u);
}

void WireTypesTest::ladderEntryProgressIsRead()
{
    // "the values of this depend on the league objective": kept as the raw
    // object, since the reference gives no shape for it.
    const auto entry = readTolerant<poe::LadderEntry>(R"json({
        "rank": 1,
        "character": {
            "id": "0000000000000000000000000000000000000000000000000000000000000005",
            "name": "TestRanger",
            "level": 100,
            "class": "Ranger",
            "progress": {"objective": 3}
        }
    })json");
    QVERIFY(entry);
    QVERIFY(entry->character.progress);
    QCOMPARE(QString::fromStdString(entry->character.progress->str),
             QString(R"json({"objective": 3})json"));
}

QTEST_GUILESS_MAIN(WireTypesTest)
#include "tst_wiretypes.moc"
