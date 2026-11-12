#include <QtTest/QtTest>

#include <optional>

#include "filters/filtermatchers.h"
#include "filters/filterspec.h"
#include "itemcategories.h"
#include "modlist.h"
#include "testfixtures.h"

class FiltersTest : public QObject
{
    Q_OBJECT

private slots:
    void tabFilter();
    void nameFilter();
    void categoryFilter();
    void categoryChoiceMatchesExactly();
    void minMaxFilter();
    void percentPropertyFilters();
    void defaultAndRequiredFilters();
    void requirementSpellingsAndCatalystQuality();
    void socketColorFilters();
    void booleanFilter();
    void booleanPredicates();
    void rarityFilter();
    void modsFilter();
    void valuelessModFilter();
    void mapTierFromBaseType();
};

static const QString kIcon = "https://web.poecdn.com/image/test.png";

static std::shared_ptr<Item> makeFilterItem(
    const QString &id,
    const QString &extraJson,
    int frameType = 2,
    const QString &frameTypeId = "Rare",
    const QString &icon = "https://web.poecdn.com/image/test.png",
    const QString &baseType = "Test Item",
    bool identified = true)
{
    const QByteArray json = QString(R"json({
        "baseType": "%6",
        "frameType": %3,
        "frameTypeId": "%4",
        "h": 1,
        "icon": "%5",
        "id": "%1",
        "identified": %7,
        "ilvl": 1,
        "name": "Alpha Bite",
        "typeLine": "Test Item",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0%2
    })json")
                                .arg(id,
                                     extraJson,
                                     QString::number(frameType),
                                     frameTypeId,
                                     icon,
                                     baseType,
                                     identified ? "true" : "false")
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), makeTestStashLocation()));
}

static const FilterSpec *findFilterSpec(const FilterCatalog &catalog, const QString &caption)
{
    for (const auto &spec : catalog) {
        if (spec.caption == caption) {
            return &spec;
        }
    }
    return nullptr;
}

static const FilterSpec *findMinMaxFilterSpec(const FilterCatalog &catalog, const QString &caption)
{
    for (const auto &spec : catalog) {
        if ((spec.caption == caption) && std::holds_alternative<MinMaxPayload>(spec.payload)) {
            return &spec;
        }
    }
    return nullptr;
}

static const FilterSpec *findTextFilterSpec(const FilterCatalog &catalog, const QString &caption)
{
    for (const auto &spec : catalog) {
        if ((spec.caption == caption) && std::holds_alternative<TextPayload>(spec.payload)) {
            return &spec;
        }
    }
    return nullptr;
}

static const FilterSpec *findComboFilterSpec(const FilterCatalog &catalog, const QString &caption)
{
    for (const auto &spec : catalog) {
        if ((spec.caption == caption) && std::holds_alternative<ComboPayload>(spec.payload)) {
            return &spec;
        }
    }
    return nullptr;
}

static const FilterSpec *findColorsFilterSpec(const FilterCatalog &catalog,
                                              ColorsMatchKind matchKind)
{
    for (const auto &spec : catalog) {
        const auto *payload = std::get_if<ColorsPayload>(&spec.payload);
        if (payload && payload->matchKind == matchKind) {
            return &spec;
        }
    }
    return nullptr;
}

static void verifyBooleanPredicate(const FilterSpec &spec,
                                   const std::shared_ptr<Item> &matching,
                                   const std::shared_ptr<Item> &notMatching)
{
    BoolState state{true};
    QVERIFY(MatchesFilter(*matching, spec, state));
    QVERIFY(!MatchesFilter(*notMatching, spec, state));

    state.checked = false;
    QVERIFY(MatchesFilter(*notMatching, spec, state));
}

void FiltersTest::tabFilter()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *tab = findTextFilterSpec(catalog, "Tab");
    QVERIFY(tab);
    QCOMPARE(tab->refreshMode, RefreshMode::Debounced);
    const FilterState defaultState = MakeDefaultState(*tab);
    QVERIFY(std::holds_alternative<TextState>(defaultState));
    QVERIFY(!IsActive(defaultState));

    TextState state{"test"};
    QVERIFY(MatchesFilter(*makeFilterItem("tab-match", ""), *tab, state));
    state.query = "missing";
    QVERIFY(!MatchesFilter(*makeFilterItem("tab-miss", ""), *tab, state));
}

void FiltersTest::nameFilter()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *name = findTextFilterSpec(catalog, "Name");
    QVERIFY(name);
    QCOMPARE(name->refreshMode, RefreshMode::Debounced);

    TextState state{"alpha"};
    QVERIFY(MatchesFilter(*makeFilterItem("name-match", ""), *name, state));

    state.query = "ALPHA";
    QVERIFY(MatchesFilter(*makeFilterItem("name-match-case", ""), *name, state));

    state.query = "missing";
    QVERIFY(!MatchesFilter(*makeFilterItem("name-miss", ""), *name, state));
}

void FiltersTest::categoryFilter()
{
    InitItemClasses(R"json({"TestClass":{"name":"Weapons"}})json");
    InitItemBaseTypes(
        R"json({"Metadata/Items/TestSword":{"item_class":"TestClass","name":"Test Sword","release_state":"released"}})json");

    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *category = findComboFilterSpec(catalog, "Category");
    QVERIFY(category);
    QCOMPARE(category->refreshMode, RefreshMode::Debounced);
    const auto *payload = std::get_if<ComboPayload>(&category->payload);
    QVERIFY(payload);
    QCOMPARE(payload->matchKind, ComboMatchKind::CategoryContains);
    QCOMPARE(payload->anySentinel, kAnyFilterChoice);

    ComboState state{"weapons"};

    const auto weapon = makeFilterItem("category-match", "", 2, "Rare", {}, "Test Sword");
    const auto unknown = makeFilterItem("category-miss", "");
    QVERIFY(MatchesFilter(*weapon, *category, state));
    QVERIFY(!MatchesFilter(*unknown, *category, state));
}

// #269: a category chosen from the list matches that category only; free
// text typed into the box still matches by substring.
void FiltersTest::categoryChoiceMatchesExactly()
{
    InitItemClasses(R"json({
        "Jewel": {"name": "Jewels"},
        "AbyssJewel": {"name": "Abyss Jewels"},
        "One Hand Sword": {"name": "One Hand Swords"},
        "Thrusting One Hand Sword": {"name": "Thrusting One Hand Swords"}
    })json");
    InitItemBaseTypes(R"json({
        "Metadata/Items/Jewels/JewelInt": {"item_class": "Jewel", "name": "Cobalt Jewel", "release_state": "released"},
        "Metadata/Items/Jewels/JewelAbyssCaster": {"item_class": "AbyssJewel", "name": "Hypnotic Eye Jewel", "release_state": "released"},
        "Metadata/Items/Weapons/OneHandWeapons/OneHandSwords/OneHandSword1": {"item_class": "One Hand Sword", "name": "Rusted Sword", "release_state": "released"},
        "Metadata/Items/Weapons/OneHandWeapons/OneHandThrustingSwords/Rapier1": {"item_class": "Thrusting One Hand Sword", "name": "Rusted Spike", "release_state": "released"}
    })json");

    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *category = findComboFilterSpec(catalog, "Category");
    QVERIFY(category);

    const auto jewel = makeFilterItem("jewel", "", 2, "Rare", {}, "Cobalt Jewel");
    const auto abyss = makeFilterItem("abyss", "", 2, "Rare", {}, "Hypnotic Eye Jewel");
    const auto sword = makeFilterItem("sword", "", 2, "Rare", {}, "Rusted Sword");
    const auto rapier = makeFilterItem("rapier", "", 2, "Rare", {}, "Rusted Spike");
    QCOMPARE(jewel->category(), QString("jewels"));
    QCOMPARE(abyss->category(), QString("abyss jewels"));

    // The form lower-cases the chosen text before it reaches the matcher.
    ComboState state{"jewels"};
    QVERIFY(MatchesFilter(*jewel, *category, state));
    QVERIFY(!MatchesFilter(*abyss, *category, state));

    state.value = "abyss jewels";
    QVERIFY(MatchesFilter(*abyss, *category, state));
    QVERIFY(!MatchesFilter(*jewel, *category, state));

    state.value = "one hand swords";
    QVERIFY(MatchesFilter(*sword, *category, state));
    QVERIFY(!MatchesFilter(*rapier, *category, state));

    // Free text that is not a listed choice keeps substring matching.
    state.value = "jewel";
    QVERIFY(MatchesFilter(*jewel, *category, state));
    QVERIFY(MatchesFilter(*abyss, *category, state));
    state.value = "swords";
    QVERIFY(MatchesFilter(*sword, *category, state));
    QVERIFY(MatchesFilter(*rapier, *category, state));
    QVERIFY(!MatchesFilter(*jewel, *category, state));
}

void FiltersTest::minMaxFilter()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);

    for (const QString &caption :
         {"Crit.",   "DPS",    "pDPS",    "eDPS",    "cDPS",     "APS",      "Armour",
          "Evasion", "Shield", "Block",   "Sockets", "Links",    "R. Level", "R. Str",
          "R. Dex",  "R. Int", "Quality", "Level",   "Map Tier", "ilvl"}) {
        const auto *spec = findMinMaxFilterSpec(catalog, caption);
        QVERIFY(spec);
        QCOMPARE(spec->refreshMode, RefreshMode::Debounced);
        const FilterState defaultState = MakeDefaultState(*spec);
        QVERIFY(std::holds_alternative<MinMaxState>(defaultState));
        QVERIFY(!IsActive(defaultState));
    }

    const auto *crit = findMinMaxFilterSpec(catalog, "Crit.");
    QVERIFY(crit);
    const auto withCrit = makeFilterItem("crit",
                                         R"json(,
        "properties": [
            {
                "displayMode": 0,
                "name": "Critical Strike Chance",
                "type": 6,
                "values": [["20", 1]]
            }
        ])json");
    const auto withoutCrit = makeFilterItem("missing-crit", "");

    MinMaxState state;
    state.min = 10.0;
    QVERIFY(MatchesFilter(*withCrit, *crit, state));

    state.min = 25.0;
    QVERIFY(!MatchesFilter(*withCrit, *crit, state));

    state.min.reset();
    state.max = 20.0;
    QVERIFY(MatchesFilter(*withCrit, *crit, state));

    state.max = 19.0;
    QVERIFY(!MatchesFilter(*withCrit, *crit, state));

    state.min = 1.0;
    state.max.reset();
    QVERIFY(!MatchesFilter(*withoutCrit, *crit, state));

    state.min.reset();
    QVERIFY(MatchesFilter(*withoutCrit, *crit, state));
}

// #229: Crit and Block arrive as "8.00%" and "23%". The filters read the
// number, not the display string.
void FiltersTest::percentPropertyFilters()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto item = makeFilterItem("percent",
                                     R"json(,
        "properties": [
            {"displayMode": 0, "name": "Critical Strike Chance", "values": [["8.00%", 1]]},
            {"displayMode": 0, "name": "Chance to Block", "values": [["23%", 0]]}
        ])json");

    const auto *crit = findMinMaxFilterSpec(catalog, "Crit.");
    QVERIFY(crit);
    MinMaxState state;
    state.min = 7.5;
    QVERIFY(MatchesFilter(*item, *crit, state));
    state.min = 8.5;
    QVERIFY(!MatchesFilter(*item, *crit, state));
    state.min.reset();
    state.max = 5.0;
    QVERIFY(!MatchesFilter(*item, *crit, state));

    const auto *block = findMinMaxFilterSpec(catalog, "Block");
    QVERIFY(block);
    state = MinMaxState{};
    state.min = 20.0;
    QVERIFY(MatchesFilter(*item, *block, state));
    state.min = 24.0;
    QVERIFY(!MatchesFilter(*item, *block, state));
}

void FiltersTest::defaultAndRequiredFilters()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto withoutQuality = makeFilterItem("default-quality", "");
    const auto *quality = findMinMaxFilterSpec(catalog, "Quality");
    QVERIFY(quality);
    MinMaxState qualityState;
    qualityState.min = 0.0;
    QVERIFY(MatchesFilter(*withoutQuality, *quality, qualityState));
    qualityState.min = 1.0;
    QVERIFY(!MatchesFilter(*withoutQuality, *quality, qualityState));

    const auto withRequirement = makeFilterItem("requirement",
                                                R"json(,
        "requirements": [
            {"displayMode": 0, "name": "Level", "values": [["12", 0]]}
        ])json");
    const auto withoutRequirement = makeFilterItem("no-requirement", "");
    const auto *required = findMinMaxFilterSpec(catalog, "R. Level");
    QVERIFY(required);
    MinMaxState requiredState;
    requiredState.min = 12.0;
    QVERIFY(MatchesFilter(*withRequirement, *required, requiredState));
    QVERIFY(!MatchesFilter(*withoutRequirement, *required, requiredState));
    requiredState.min.reset();
    requiredState.max = 0.0;
    QVERIFY(MatchesFilter(*withoutRequirement, *required, requiredState));
}

// #263: requirements arrive as both "Int" and "Intelligence" (likewise Str
// and Dex), and catalyst quality as "Quality (<kind>)".
void FiltersTest::requirementSpellingsAndCatalystQuality()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);

    const auto longForm = makeFilterItem("long-form",
                                         R"json(,
        "requirements": [
            {"displayMode": 0, "name": "Level", "values": [["68", 0]]},
            {"displayMode": 1, "name": "Strength", "values": [["111", 0]]},
            {"displayMode": 1, "name": "Dexterity", "values": [["98", 0]]},
            {"displayMode": 1, "name": "Intelligence", "values": [["242", 0]]}
        ])json");
    const auto shortForm = makeFilterItem("short-form",
                                          R"json(,
        "requirements": [
            {"displayMode": 0, "name": "Level", "values": [["68", 0]]},
            {"displayMode": 1, "name": "Str", "values": [["111", 0]]},
            {"displayMode": 1, "name": "Dex", "values": [["98", 0]]},
            {"displayMode": 1, "name": "Int", "values": [["242", 0]]}
        ])json");

    struct Expected
    {
        const char *caption;
        double value;
    };
    for (const auto &[caption, value] :
         {Expected{"R. Str", 111.0}, Expected{"R. Dex", 98.0}, Expected{"R. Int", 242.0}}) {
        const auto *spec = findMinMaxFilterSpec(catalog, caption);
        QVERIFY2(spec, caption);
        MinMaxState state;
        state.min = value;
        state.max = value;
        QVERIFY2(MatchesFilter(*longForm, *spec, state), caption);
        QVERIFY2(MatchesFilter(*shortForm, *spec, state), caption);
        state.min = value + 1;
        state.max.reset();
        QVERIFY2(!MatchesFilter(*longForm, *spec, state), caption);
    }

    const auto catalyst = makeFilterItem("catalyst",
                                         R"json(,
        "properties": [
            {"displayMode": 0, "name": "Stack Size", "values": [["3/10", 0]]},
            {"displayMode": 0, "name": "Quality (Attribute Modifiers)", "values": [["+20%", 1]]}
        ])json");
    const auto *quality = findMinMaxFilterSpec(catalog, "Quality");
    QVERIFY(quality);
    MinMaxState qualityState;
    qualityState.min = 15.0;
    QVERIFY(MatchesFilter(*catalyst, *quality, qualityState));
    qualityState.min = 21.0;
    QVERIFY(!MatchesFilter(*catalyst, *quality, qualityState));
}

void FiltersTest::socketColorFilters()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *sockets = findColorsFilterSpec(catalog, ColorsMatchKind::Sockets);
    const auto *links = findColorsFilterSpec(catalog, ColorsMatchKind::Links);
    QVERIFY(sockets);
    QVERIFY(links);
    QCOMPARE(sockets->caption, "Colors");
    QCOMPARE(links->caption, "Linked");
    QCOMPARE(sockets->group, FilterGroup::Sockets);
    QCOMPARE(links->group, FilterGroup::Sockets);
    QCOMPARE(sockets->refreshMode, RefreshMode::Immediate);
    QCOMPARE(links->refreshMode, RefreshMode::Immediate);
    QVERIFY(std::holds_alternative<ColorsState>(MakeDefaultState(*sockets)));
    QVERIFY(std::holds_alternative<ColorsState>(MakeDefaultState(*links)));
    QVERIFY(!IsActive(MakeDefaultState(*sockets)));
    QVERIFY(!IsActive(MakeDefaultState(*links)));

    const auto item = makeFilterItem("sockets",
                                     R"json(,
        "sockets": [
            {"group": 0, "attr": "S", "sColour": "R"},
            {"group": 0, "attr": "G", "sColour": "W"},
            {"group": 1, "attr": "I", "sColour": "B"}
        ])json");

    ColorsState socketsState;
    socketsState.r = 1;
    socketsState.b = 1;
    QVERIFY(MatchesFilter(*item, *sockets, socketsState));

    socketsState.b = 3;
    QVERIFY(!MatchesFilter(*item, *sockets, socketsState));

    ColorsState linksState;
    linksState.r = 1;
    linksState.b = 1;
    QVERIFY(MatchesFilter(*item, *links, linksState));

    linksState.b = 2;
    QVERIFY(!MatchesFilter(*item, *links, linksState));
}

void FiltersTest::booleanFilter()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *corrupted = findFilterSpec(catalog, "Corrupted");
    QVERIFY(corrupted);
    QVERIFY(std::holds_alternative<BoolPayload>(corrupted->payload));

    const FilterState defaultState = MakeDefaultState(*corrupted);
    QVERIFY(std::holds_alternative<BoolState>(defaultState));
    QVERIFY(!IsActive(defaultState));

    BoolState state{true};

    QVERIFY(MatchesFilter(*makeFilterItem("corrupted", R"json(, "corrupted": true)json"),
                          *corrupted,
                          state));
    QVERIFY(!MatchesFilter(*makeFilterItem("not-corrupted", ""), *corrupted, state));

    state.checked = false;
    QVERIFY(MatchesFilter(*makeFilterItem("unchecked", ""), *corrupted, state));
}

void FiltersTest::booleanPredicates()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto ordinary = makeFilterItem("ordinary", "");

    // One item per predicate, each setting only the flag that predicate is
    // supposed to read. Every item is then checked against every other
    // predicate, so a payload wired to the wrong Item accessor fails here
    // rather than passing on an item that happens to have every flag set.
    struct BooleanCase
    {
        QString caption;
        std::shared_ptr<Item> item;
    };
    const std::vector<BooleanCase> cases{
        {"Alt. art",
         makeFilterItem("alt-art", "", 2, "Rare", "https://web.poecdn.com/image/RedBeak2.png")},
        {"Priced", makeFilterItem("priced", "")},
        {"Unidentified",
         makeFilterItem("unidentified",
                        "",
                        2,
                        "Rare",
                        "https://web.poecdn.com/image/test.png",
                        "Test Item",
                        false)},
        {"Influenced", makeFilterItem("influenced", R"json(, "influences": {"shaper": true})json")},
        {"Crafted",
         makeFilterItem(
             "crafted",
             R"json(, "explicitMods": [{"description": "Crafted modifier", "flags": {"crafted": true}}])json")},
        {"Enchanted",
         makeFilterItem("enchanted", R"json(, "enchantMods": ["Enchanted modifier"])json")},
        {"Corrupted", makeFilterItem("corrupted", R"json(, "corrupted": true)json")},
        {"Fractured", makeFilterItem("fractured", R"json(, "fractured": true)json")},
        {"Split", makeFilterItem("split", R"json(, "split": true)json")},
        {"Synthesized", makeFilterItem("synthesized", R"json(, "synthesised": true)json")},
        {"Mutated", makeFilterItem("mutated", R"json(, "mutated": true)json")},
    };

    // The priced predicate reads the buyout manager rather than the item JSON.
    const auto &pricedItem = cases.at(1).item;
    buyoutFixture.manager->Set(*pricedItem, makeChaosBuyout(5.0));

    for (const auto &current : cases) {
        const auto *spec = findFilterSpec(catalog, current.caption);
        QVERIFY2(spec, qPrintable(current.caption));
        QVERIFY(std::holds_alternative<BoolPayload>(spec->payload));
        QCOMPARE(spec->refreshMode, RefreshMode::Immediate);
        const FilterState defaultState = MakeDefaultState(*spec);
        QVERIFY(std::holds_alternative<BoolState>(defaultState));
        QVERIFY(!IsActive(defaultState));

        // Matches its own item, rejects a plain one, and an unchecked box
        // matches everything.
        verifyBooleanPredicate(*spec, current.item, ordinary);

        // ...and rejects every other predicate's item. This includes the
        // Influenced/Fractured and Influenced/Synthesized pairs: the F38 fix
        // gave the Influenced filter its own six-influence predicate, so the
        // fractured/synthesised markers on Item's influence list no longer
        // leak into it.
        for (const auto &other : cases) {
            if (other.caption == current.caption) {
                continue;
            }
            const bool matched = MatchesFilter(*other.item, *spec, BoolState{true});
            QVERIFY2(!matched,
                     qPrintable(
                         QString("%1 matched the %2 item").arg(current.caption, other.caption)));
        }
    }

    // The altart predicate is an icon-needle list, so check a few more needles.
    const auto *altart = findFilterSpec(catalog, "Alt. art");
    QVERIFY(altart);
    for (const QString &needle : {"RedBeak2.png", "dGlsbGF0ZUFsdCI7czoy", "WinterHeart.png"}) {
        const auto item = makeFilterItem("alt-" + needle,
                                         "",
                                         2,
                                         "Rare",
                                         "https://web.poecdn.com/image/" + needle);
        QVERIFY(MatchesFilter(*item, *altart, BoolState{true}));
    }
}

void FiltersTest::rarityFilter()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *rarity = findComboFilterSpec(catalog, "Rarity");
    QVERIFY(rarity);
    QCOMPARE(rarity->refreshMode, RefreshMode::Debounced);
    const auto *payload = std::get_if<ComboPayload>(&rarity->payload);
    QVERIFY(payload);
    QCOMPARE(payload->matchKind, ComboMatchKind::Rarity);
    QCOMPARE(payload->anySentinel, kAnyFilterChoice);

    const auto normal = makeFilterItem("normal", "", 0, "Normal");
    const auto rare = makeFilterItem("rare", "", 2, "Rare");
    const auto unique = makeFilterItem("unique", "", 3, "Unique");
    const auto foil = makeFilterItem("foil", "", 9, "Foil");

    ComboState state{"Any Non-Unique"};
    QVERIFY(MatchesFilter(*normal, *rarity, state));
    QVERIFY(MatchesFilter(*rare, *rarity, state));
    QVERIFY(!MatchesFilter(*unique, *rarity, state));

    state.value = "Unique";
    QVERIFY(MatchesFilter(*unique, *rarity, state));
    QVERIFY(!MatchesFilter(*foil, *rarity, state));

    state.value = "Unique (Foil)";
    QVERIFY(MatchesFilter(*foil, *rarity, state));
    QVERIFY(!MatchesFilter(*unique, *rarity, state));
}

void FiltersTest::modsFilter()
{
    InitStatTranslations();
    AddStatTranslations(R"json([
        {"English":[{"format":["+#"],"string":"{0} to maximum Life"}]}
    ])json");
    InitModList();

    const auto item = makeFilterItem("mods",
                                     R"json(,
        "explicitMods": [{"description": "+42 to maximum Life"}]
    )json");
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *mods = findFilterSpec(catalog, "Mods");
    QVERIFY(mods);
    QCOMPARE(mods->group, FilterGroup::Mods);
    QCOMPARE(mods->refreshMode, RefreshMode::Debounced);
    QVERIFY(std::holds_alternative<ModsPayload>(mods->payload));

    const FilterState defaultState = MakeDefaultState(*mods);
    const auto *defaultMods = std::get_if<ModsState>(&defaultState);
    QVERIFY(defaultMods);
    QVERIFY(defaultMods->rows.empty());
    QVERIFY(!defaultMods->isActive());

    ModsState state;
    ModRow emptyRow;
    emptyRow.min = 100.0;
    emptyRow.max = 1.0;
    state.rows.push_back(emptyRow);
    QVERIFY(state.isActive());
    QCOMPARE(state.rows.size(), 1);
    QVERIFY(state.rows.front().mod.isEmpty());
    QVERIFY(MatchesFilter(*item, *mods, state));

    state.rows.push_back(ModRow{"+# to maximum Life", 40.0, 45.0});
    QVERIFY(MatchesFilter(*item, *mods, state));
    state.rows.back().min = 43.0;
    QVERIFY(!MatchesFilter(*item, *mods, state));
    state.rows.back().min = 40.0;
    state.rows.back().max = 41.0;
    QVERIFY(!MatchesFilter(*item, *mods, state));
    state.rows.back().max = 45.0;
    state.rows.push_back(ModRow{"+# to missing stat", std::nullopt, std::nullopt});
    QVERIFY(!MatchesFilter(*item, *mods, state));
}

// #265: a mod with no number is present, with value 0; a minimum above 0
// rejects it instead of passing a NaN.
void FiltersTest::valuelessModFilter()
{
    InitStatTranslations();
    AddStatTranslations(R"json([
        {"English":[{"format":["ignore"],"string":"Cannot be Frozen"}]}
    ])json");
    InitModList();

    const auto item = makeFilterItem("valueless",
                                     R"json(,
        "explicitMods": [{"description": "Cannot be Frozen"}]
    )json");
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *mods = findFilterSpec(catalog, "Mods");
    QVERIFY(mods);

    ModsState state;
    state.rows.push_back(ModRow{"Cannot be Frozen", std::nullopt, std::nullopt});
    QVERIFY(MatchesFilter(*item, *mods, state));
    state.rows.back().min = 1.0;
    QVERIFY(!MatchesFilter(*item, *mods, state));
}

// #229: maps carry no "Map Tier" property; the tier is only in the base
// type. The census in that issue found the property on 0 of 7,273 maps in
// the maintainer's corpus, with base types such as "Map (Tier 16)". The
// base type "Map (Tier N)" is a real RePoE base (class MapKey); "Shaper's
// Orb (Tier N)" is a quest item and has no map tier.
void FiltersTest::mapTierFromBaseType()
{
    BuyoutManagerFixture buyoutFixture;
    const FilterCatalog catalog = BuildFilterCatalog(*buyoutFixture.manager);
    const auto *tier = findMinMaxFilterSpec(catalog, "Map Tier");
    QVERIFY(tier);

    const auto map16 = makeFilterItem("map16", "", 2, "Rare", kIcon, "Map (Tier 16)");
    const auto map3 = makeFilterItem("map3", "", 0, "Normal", kIcon, "Map (Tier 3)");
    const auto orb = makeFilterItem("orb", "", 0, "Normal", kIcon, "Shaper's Orb (Tier 3)");
    const auto sword = makeFilterItem("sword", "", 2, "Rare", kIcon, "Rusted Sword");

    MinMaxState state;
    state.min = 14.0;
    QVERIFY(MatchesFilter(*map16, *tier, state));
    QVERIFY(!MatchesFilter(*map3, *tier, state));
    QVERIFY(!MatchesFilter(*orb, *tier, state));
    QVERIFY(!MatchesFilter(*sword, *tier, state));

    state.min.reset();
    state.max = 5.0;
    QVERIFY(MatchesFilter(*map3, *tier, state));
    QVERIFY(!MatchesFilter(*map16, *tier, state));
    QVERIFY(!MatchesFilter(*orb, *tier, state));
    QVERIFY(!MatchesFilter(*sword, *tier, state));

    // A property, where an item has one, wins over the base type.
    const auto withProperty = makeFilterItem("map-property",
                                             R"json(,
        "properties": [{"displayMode": 0, "name": "Map Tier", "values": [["7", 0]]}]
    )json",
                                             0,
                                             "Normal",
                                             kIcon,
                                             "Map (Tier 3)");
    QVERIFY(!MatchesFilter(*withProperty, *tier, state));
    state.max = 7.0;
    QVERIFY(MatchesFilter(*withProperty, *tier, state));

    // An unexpected shape yields no tier rather than a wrong one.
    state = MinMaxState{.min = 0.0, .max = std::nullopt};
    for (const QString &base :
         {"Map (Tier )", "Map (Tier 16", "Map (Tier x)", "Map (Tier 3) Extra"}) {
        QVERIFY2(!MatchesFilter(*makeFilterItem("odd", "", 0, "Normal", kIcon, base), *tier, state),
                 qPrintable(base));
    }
}

QTEST_GUILESS_MAIN(FiltersTest)

#include "tst_filters.moc"
