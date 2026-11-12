#include <QtTest/QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

#include "itemcategories.h"
#include "modlist.h"
#include "search.h"
#include "testfixtures.h"
#include "ui/modsfilterform.h"
#include "ui/searchform.h"

class SearchTest : public QObject
{
    Q_OBJECT

private slots:
    void filterStateRestoresAcrossTabSwitch();
    void backgroundSearchIgnoresCurrentFormState();
    void categoryAnySentinelDeactivatesFilter();
    void booleanFormAdapterRoundTrip();
    void minMaxFormAdapterRoundTrip();
    void colorsFormAdapterRoundTrip();
    void modsEditSurvivesTabSwitchBeforeDebouncedRefresh();
    void modsFormAdapterRoundTrip();
    void modsFormAdapterFreeTextPersistsAndRestoresIndex();
    void modsFormAdapterDoesNotArmCompleterOnProgrammaticRows();
    void modsFormAdapterRepacksRows();
    void modsFormAdapterDisconnectsOnDestruction();
    void searchFormUnbindsDeletedSearch();
    void textAndComboFormAdapterRoundTrip();
};

static qsizetype findTextFilterIndex(const FilterCatalog &catalog, const QString &caption)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto &spec = catalog[index];
        if ((spec.caption == caption) && std::holds_alternative<TextPayload>(spec.payload)) {
            return index;
        }
    }
    return -1;
}

static qsizetype findComboFilterIndex(const FilterCatalog &catalog, const QString &caption)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto &spec = catalog[index];
        if ((spec.caption == caption) && std::holds_alternative<ComboPayload>(spec.payload)) {
            return index;
        }
    }
    return -1;
}

static qsizetype findMinMaxFilterIndex(const FilterCatalog &catalog, const QString &caption)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto &spec = catalog[index];
        if ((spec.caption == caption) && std::holds_alternative<MinMaxPayload>(spec.payload)) {
            return index;
        }
    }
    return -1;
}

static qsizetype findColorsFilterIndex(const FilterCatalog &catalog, ColorsMatchKind matchKind)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto *payload = std::get_if<ColorsPayload>(&catalog[index].payload);
        if (payload && payload->matchKind == matchKind) {
            return index;
        }
    }
    return -1;
}

static qsizetype findModsFilterIndex(const FilterCatalog &catalog)
{
    for (qsizetype index = 0; index < catalog.size(); ++index) {
        const auto &spec = catalog[index];
        if ((spec.caption == "Mods") && std::holds_alternative<ModsPayload>(spec.payload)) {
            return index;
        }
    }
    return -1;
}

// SearchForm materializes its combo models at construction, so the category
// choices must exist before the form is built or GetItemCategories() logs an
// error. Declared ahead of the catalog and form so it initializes first. The
// category tables are process-global singletons that warn on reload, so load
// them once for the whole test binary.
struct ItemCategoryFixture
{
    ItemCategoryFixture()
    {
        static const bool loaded = [] {
            InitItemClasses(R"json({"TestClass":{"name":"Weapons"}})json");
            InitItemBaseTypes(
                R"json({"Metadata/Items/TestSword":{"item_class":"TestClass","name":"Test Sword","release_state":"released"}})json");
            return true;
        }();
        Q_UNUSED(loaded);
    }
};

struct SearchHarness
{
    BuyoutManagerFixture buyoutFixture;
    ItemCategoryFixture categoryFixture;
    QWidget host;
    QVBoxLayout layout{&host};
    QObject receiver;
    int immediateChanges = 0;
    int delayedChanges = 0;
    FilterCallbacks callbacks{
        &receiver,
        [this] { ++immediateChanges; },
        [this] { ++delayedChanges; },
    };
    FilterCatalog catalog{BuildFilterCatalog(*buyoutFixture.manager)};
    SearchForm form{layout, catalog, callbacks};

    template<typename Widget>
    Widget *findByLabel(const QString &labelText,
                        const std::function<bool(const Widget *)> &matches = {})
    {
        for (auto *label : host.findChildren<QLabel *>()) {
            if (label->text() != labelText) {
                continue;
            }
            for (auto *widget : label->parentWidget()->findChildren<Widget *>()) {
                if (!matches || matches(widget)) {
                    return widget;
                }
            }
        }
        return nullptr;
    }
};

static std::shared_ptr<Item> makeFormItem(const QString &id,
                                          const QString &name,
                                          const ItemLocation &location)
{
    const QByteArray json = QString(R"json({
        "baseType": "Test Item",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "%1",
        "identified": true,
        "ilvl": 1,
        "name": "%2",
        "typeLine": "Test Item",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0
    })json")
                                .arg(id, name)
                                .toUtf8();
    return std::make_shared<Item>(makeTestItem(json.constData(), location));
}

void SearchTest::filterStateRestoresAcrossTabSwitch()
{
    SearchHarness harness;
    auto *tab = harness.findByLabel<QLineEdit>("Tab");
    auto *name = harness.findByLabel<QLineEdit>("Name");
    auto *critMin = harness.findByLabel<QLineEdit>("Crit.", [](const QLineEdit *edit) {
        return edit->placeholderText() == "min";
    });
    auto *critMax = harness.findByLabel<QLineEdit>("Crit.", [](const QLineEdit *edit) {
        return edit->placeholderText() == "max";
    });
    auto *colorR = harness.findByLabel<QLineEdit>("Colors", [](const QLineEdit *edit) {
        return edit->placeholderText() == "R";
    });
    auto *colorG = harness.findByLabel<QLineEdit>("Colors", [](const QLineEdit *edit) {
        return edit->placeholderText() == "G";
    });
    auto *rarity = harness.findByLabel<QComboBox>("Rarity");
    auto *corrupted = harness.findByLabel<QCheckBox>("Corrupted");
    QVERIFY(tab);
    QVERIFY(name);
    QVERIFY(critMin);
    QVERIFY(critMax);
    QVERIFY(colorR);
    QVERIFY(colorG);
    QVERIFY(rarity);
    QVERIFY(corrupted);

    tab->setText("Alpha Tab");
    name->setText("alpha");
    critMin->setText("5");
    critMax->setText("10");
    colorR->setText("2");
    colorG->setText("1");
    rarity->setCurrentText("Rare");
    corrupted->setChecked(true);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.reset();
    harness.form.saveTo(searchB);
    harness.form.loadFrom(searchA);

    QCOMPARE(tab->text(), "Alpha Tab");
    QCOMPARE(name->text(), "alpha");
    QCOMPARE(critMin->text(), "5");
    QCOMPARE(critMax->text(), "10");
    QCOMPARE(colorR->text(), "2");
    QCOMPARE(colorG->text(), "1");
    QCOMPARE(rarity->currentText(), "Rare");
    QVERIFY(corrupted->isChecked());
}

// F33, driven through the form the way MainWindow does: the user fills in a
// name on search A, switches to B (which empties the form), and a background
// refilter of A must still use A's saved query. The old shared activity flag
// lived on the Filter object, so an empty form made the name filter inactive
// for every search and A came back with both items.
void SearchTest::backgroundSearchIgnoresCurrentFormState()
{
    SearchHarness harness;
    const ItemLocation firstTab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    const ItemLocation secondTab = makeTestStashLocation("stash-b", "Beta Tab", 1);
    harness.buyoutFixture.manager->SetStashTabLocations({firstTab, secondTab});

    Items items;
    items.push_back(makeFormItem("alpha-item", "Alpha Bite", firstTab));
    items.push_back(makeFormItem("beta-item", "Beta Guard", secondTab));

    auto *name = harness.findByLabel<QLineEdit>("Name");
    QVERIFY(name);

    Search background(*harness.buyoutFixture.manager, "Background", harness.catalog);
    name->setText("alpha");
    harness.form.saveTo(background);

    Search current(*harness.buyoutFixture.manager, "Current", harness.catalog);
    harness.form.reset();
    harness.form.saveTo(current);
    harness.form.loadFrom(current);
    QCOMPARE(name->text(), "");

    background.FilterItems(items);

    QCOMPARE(background.GetCaption(), "Background [1]");
    QCOMPARE(background.items().size(), 1);
    QCOMPARE(background.items().front()->id(), "alpha-item");
}

void SearchTest::categoryAnySentinelDeactivatesFilter()
{
    SearchHarness harness;
    const ItemLocation tab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    harness.buyoutFixture.manager->SetStashTabLocations({tab});

    Items items;
    items.push_back(makeFormItem("alpha-item", "Alpha Bite", tab));
    items.push_back(makeFormItem("beta-item", "Beta Guard", tab));

    const qsizetype categoryIndex = findComboFilterIndex(harness.catalog, "Category");
    QVERIFY(categoryIndex >= 0);
    const auto *payload = std::get_if<ComboPayload>(&harness.catalog[categoryIndex].payload);
    QVERIFY(payload);

    auto *category = harness.findByLabel<QComboBox>("Type");
    QVERIFY(category);

    Search search(*harness.buyoutFixture.manager, "Search", harness.catalog);

    const int weaponsIndex = category->findText("Weapons", Qt::MatchFixedString);
    QVERIFY(weaponsIndex >= 0);
    category->setCurrentIndex(weaponsIndex);
    harness.form.saveTo(search);
    QCOMPARE(std::get<ComboState>(search.filterStateAt(categoryIndex)).value, "weapons");
    QVERIFY(IsActive(search.filterStateAt(categoryIndex)));

    // Neither test item is a weapon, so an active category filter excludes both.
    search.FilterItems(items);
    QCOMPARE(search.items().size(), 0);

    // <any> saves as an empty string, which is what makes the filter inactive
    // and therefore match every item.
    const int anyIndex = category->findText(payload->anySentinel, Qt::MatchFixedString);
    QCOMPARE(anyIndex, 0);
    category->setCurrentIndex(anyIndex);
    harness.form.saveTo(search);
    QCOMPARE(std::get<ComboState>(search.filterStateAt(categoryIndex)).value, "");
    QVERIFY(!IsActive(search.filterStateAt(categoryIndex)));

    search.FilterItems(items);
    QCOMPARE(search.items().size(), 2);
}

void SearchTest::booleanFormAdapterRoundTrip()
{
    SearchHarness harness;
    auto *corrupted = harness.findByLabel<QCheckBox>("Corrupted");
    QVERIFY(corrupted);

    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 0);
    corrupted->click();
    QCOMPARE(harness.immediateChanges, 1);
    QCOMPARE(harness.delayedChanges, 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    harness.form.reset();
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchB);
    harness.form.loadFrom(searchA);

    QVERIFY(corrupted->isChecked());
    QCOMPARE(harness.immediateChanges, 1);
    QCOMPARE(harness.delayedChanges, 0);
}

void SearchTest::minMaxFormAdapterRoundTrip()
{
    SearchHarness harness;
    auto *critMin = harness.findByLabel<QLineEdit>("Crit.", [](const QLineEdit *edit) {
        return edit->placeholderText() == "min";
    });
    auto *critMax = harness.findByLabel<QLineEdit>("Crit.", [](const QLineEdit *edit) {
        return edit->placeholderText() == "max";
    });
    QVERIFY(critMin);
    QVERIFY(critMax);
    const qsizetype critIndex = findMinMaxFilterIndex(harness.catalog, "Crit.");
    QVERIFY(critIndex >= 0);

    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 0);
    critMin->setFocus();
    QTest::keyClick(critMin, Qt::Key_1);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);

    critMin->setText("garbage");
    critMax->setText("12.5");
    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    const auto &state = std::get<MinMaxState>(searchA.filterStateAt(critIndex));
    QVERIFY(state.min.has_value());
    QCOMPARE(*state.min, 0.0);
    QVERIFY(state.max.has_value());
    QCOMPARE(*state.max, 12.5);
    QVERIFY(state.isActive());

    harness.form.reset();
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchB);
    harness.form.loadFrom(searchB);
    QCOMPARE(critMin->text(), "");
    QCOMPARE(critMax->text(), "");
    harness.form.loadFrom(searchA);

    QCOMPARE(critMin->text(), "0");
    QCOMPARE(critMax->text(), "12.5");
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);
}

void SearchTest::colorsFormAdapterRoundTrip()
{
    SearchHarness harness;
    const qsizetype socketsIndex = findColorsFilterIndex(harness.catalog, ColorsMatchKind::Sockets);
    const qsizetype linksIndex = findColorsFilterIndex(harness.catalog, ColorsMatchKind::Links);
    QVERIFY(socketsIndex >= 0);
    QVERIFY(linksIndex >= 0);

    auto *socketR = harness.findByLabel<QLineEdit>("Colors", [](const QLineEdit *edit) {
        return edit->placeholderText() == "R";
    });
    auto *socketG = harness.findByLabel<QLineEdit>("Colors", [](const QLineEdit *edit) {
        return edit->placeholderText() == "G";
    });
    auto *socketB = harness.findByLabel<QLineEdit>("Colors", [](const QLineEdit *edit) {
        return edit->placeholderText() == "B";
    });
    auto *linkR = harness.findByLabel<QLineEdit>("Linked", [](const QLineEdit *edit) {
        return edit->placeholderText() == "R";
    });
    auto *linkG = harness.findByLabel<QLineEdit>("Linked", [](const QLineEdit *edit) {
        return edit->placeholderText() == "G";
    });
    auto *linkB = harness.findByLabel<QLineEdit>("Linked", [](const QLineEdit *edit) {
        return edit->placeholderText() == "B";
    });
    QVERIFY(socketR);
    QVERIFY(socketG);
    QVERIFY(socketB);
    QVERIFY(linkR);
    QVERIFY(linkG);
    QVERIFY(linkB);

    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 0);
    socketR->setFocus();
    QTest::keyClick(socketR, Qt::Key_1);
    QCOMPARE(harness.immediateChanges, 1);
    QCOMPARE(harness.delayedChanges, 0);
    linkB->setFocus();
    QTest::keyClick(linkB, Qt::Key_1);
    QCOMPARE(harness.immediateChanges, 2);
    QCOMPARE(harness.delayedChanges, 0);

    socketR->setText("garbage");
    socketG->setText("2");
    socketB->setText("");
    linkR->setText("1");
    linkG->setText("");
    linkB->setText("3");
    QCOMPARE(harness.immediateChanges, 2);
    QCOMPARE(harness.delayedChanges, 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    const auto &socketsState = std::get<ColorsState>(searchA.filterStateAt(socketsIndex));
    QVERIFY(socketsState.r.has_value());
    QCOMPARE(*socketsState.r, 0);
    QVERIFY(socketsState.g.has_value());
    QCOMPARE(*socketsState.g, 2);
    QVERIFY(!socketsState.b.has_value());
    QVERIFY(socketsState.isActive());

    const auto &linksState = std::get<ColorsState>(searchA.filterStateAt(linksIndex));
    QVERIFY(linksState.r.has_value());
    QCOMPARE(*linksState.r, 1);
    QVERIFY(!linksState.g.has_value());
    QVERIFY(linksState.b.has_value());
    QCOMPARE(*linksState.b, 3);
    QVERIFY(linksState.isActive());

    harness.form.reset();
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchB);

    const auto &emptySocketsState = std::get<ColorsState>(searchB.filterStateAt(socketsIndex));
    QVERIFY(!emptySocketsState.r.has_value());
    QVERIFY(!emptySocketsState.g.has_value());
    QVERIFY(!emptySocketsState.b.has_value());
    QVERIFY(!emptySocketsState.isActive());

    const auto &emptyLinksState = std::get<ColorsState>(searchB.filterStateAt(linksIndex));
    QVERIFY(!emptyLinksState.r.has_value());
    QVERIFY(!emptyLinksState.g.has_value());
    QVERIFY(!emptyLinksState.b.has_value());
    QVERIFY(!emptyLinksState.isActive());

    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    harness.form.loadFrom(searchA);
    QCOMPARE(socketR->text(), "0");
    QCOMPARE(socketG->text(), "2");
    QCOMPARE(socketB->text(), "");
    QCOMPARE(linkR->text(), "1");
    QCOMPARE(linkG->text(), "");
    QCOMPARE(linkB->text(), "3");
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 0);

    // F35: switching to a search without colors clears all color boxes.
    harness.form.loadFrom(searchB);
    QCOMPARE(socketR->text(), "");
    QCOMPARE(socketG->text(), "");
    QCOMPARE(socketB->text(), "");
    QCOMPARE(linkR->text(), "");
    QCOMPARE(linkG->text(), "");
    QCOMPARE(linkB->text(), "");
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 0);

    harness.form.loadFrom(searchA);
    QCOMPARE(socketR->text(), "0");
    QCOMPARE(socketG->text(), "2");
    QCOMPARE(socketB->text(), "");
    QCOMPARE(linkR->text(), "1");
    QCOMPARE(linkG->text(), "");
    QCOMPARE(linkB->text(), "3");
}

// A mods edit writes through to the bound search immediately but only arms the
// one global debounced refresh. Switching tabs before it fires gives that
// refresh to the other search, so the edited one must refilter when it is shown
// again — otherwise it displays a mod criterion its buckets do not reflect.
void SearchTest::modsEditSurvivesTabSwitchBeforeDebouncedRefresh()
{
    mod_list_model().setStringList({"Saved mod"});
    SearchHarness harness;
    const ItemLocation tab = makeTestStashLocation("stash-a", "Alpha Tab", 0);
    harness.buyoutFixture.manager->SetStashTabLocations({tab});

    Items items;
    items.push_back(makeFormItem("alpha-item", "Alpha Bite", tab));
    items.push_back(makeFormItem("beta-item", "Beta Guard", tab));

    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchA);
    searchA.FilterItems(items);
    QCOMPARE(searchA.items().size(), 2);

    // Edit a mod row on A. Neither test item carries the mod, so A should end
    // up matching nothing.
    auto *addButton = harness.host.findChild<QPushButton *>("modsAddButton");
    QVERIFY(addButton);
    addButton->click();
    auto *modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    const int savedModIndex = modCombo->findText("Saved mod", Qt::MatchFixedString);
    QVERIFY(savedModIndex >= 0);
    modCombo->setCurrentIndex(savedModIndex);
    QCOMPARE(std::get<ModsState>(searchA.filterStateAt(modsIndex)).rows.front().mod, "Saved mod");

    // Switch to B before the debounced refresh fires, then come back to A. The
    // tab-change path never re-runs the filters on its own.
    harness.form.loadFrom(searchB);
    harness.form.loadFrom(searchA);
    searchA.SetRefreshReason(RefreshReason::TabChanged);
    searchA.FilterItems(items);

    QCOMPARE(searchA.items().size(), 0);
    QCOMPARE(searchA.GetCaption(), "A [0]");
}

void SearchTest::modsFormAdapterRoundTrip()
{
    mod_list_model().setStringList({"Default mod", "Saved mod"});
    SearchHarness harness;
    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);
    auto *addButton = harness.host.findChild<QPushButton *>("modsAddButton");
    auto *rowsContainer = harness.host.findChild<QWidget *>("modsRowsContainer");
    QVERIFY(addButton);
    QVERIFY(rowsContainer);
    QCOMPARE(addButton->text(), "Add mod");
    QVERIFY(rowsContainer->isHidden());

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    addButton->click();
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);

    const auto &blankState = std::get<ModsState>(searchA.filterStateAt(modsIndex));
    QCOMPARE(blankState.rows.size(), 1);
    QVERIFY(blankState.rows.front().mod.isEmpty());
    // #271: the blank row is saved (F36(a)) but filters nothing.
    QVERIFY(!blankState.isActive());
    QVERIFY(!rowsContainer->isHidden());

    // F36(a): adding a blank row saves it before a tab switch can rebuild the form.
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.loadFrom(searchB);
    QVERIFY(rowsContainer->isHidden());
    harness.form.loadFrom(searchA);
    auto *modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    QCOMPARE(modCombo->currentText(), "");
    QCOMPARE(modCombo->currentIndex(), -1);
    QVERIFY(!rowsContainer->isHidden());

    // F36(b): rebuilt rows show their state value rather than the model's first entry.
    searchA.setFilterState(modsIndex, ModsState{{ModRow{"Saved mod", 1.0, 2.0}}});
    harness.form.loadFrom(searchB);
    QVERIFY(rowsContainer->isHidden());
    harness.form.loadFrom(searchA);
    modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    QCOMPARE(modCombo->currentText(), "Saved mod");
    QVERIFY(!rowsContainer->isHidden());

    // Row edits and deletes keep the existing delayed, not immediate, refresh path.
    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    const int savedModIndex = modCombo->findText("Saved mod", Qt::MatchFixedString);
    const int defaultModIndex = modCombo->findText("Default mod", Qt::MatchFixedString);
    QVERIFY(savedModIndex >= 0);
    QVERIFY(defaultModIndex >= 0);
    QCOMPARE(modCombo->currentIndex(), savedModIndex);
    modCombo->setCurrentIndex(defaultModIndex);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);
    QCOMPARE(std::get<ModsState>(searchA.filterStateAt(modsIndex)).rows.front().mod, "Default mod");

    const auto buttons = harness.host.findChildren<QPushButton *>();
    const auto deleteButton = std::find_if(buttons.cbegin(),
                                           buttons.cend(),
                                           [](const QPushButton *button) {
                                               return button->text() == "X";
                                           });
    QVERIFY(deleteButton != buttons.cend());
    (*deleteButton)->click();
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 2);
    QVERIFY(std::get<ModsState>(searchA.filterStateAt(modsIndex)).rows.empty());

    // F36(c): row-container visibility is entirely derived from the current row count.
    QVERIFY(rowsContainer->isHidden());
    harness.form.loadFrom(searchB);
    QVERIFY(rowsContainer->isHidden());
}

void SearchTest::modsFormAdapterFreeTextPersistsAndRestoresIndex()
{
    mod_list_model().setStringList({"First mod", "Saved mod"});
    SearchHarness harness;
    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchA);

    auto *addButton = harness.host.findChild<QPushButton *>("modsAddButton");
    QVERIFY(addButton);
    addButton->click();

    auto *modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    QCOMPARE(modCombo->currentIndex(), -1);
    auto *lineEdit = modCombo->lineEdit();
    QVERIFY(lineEdit);
    lineEdit->setFocus();
    QTest::keyClicks(lineEdit, "Free typed mod");

    const auto &typedState = std::get<ModsState>(searchA.filterStateAt(modsIndex));
    QCOMPARE(typedState.rows.size(), 1);
    QCOMPARE(typedState.rows.front().mod, "Free typed mod");

    harness.form.loadFrom(searchB);
    harness.form.loadFrom(searchA);
    modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    QCOMPARE(modCombo->currentText(), "Free typed mod");
    QCOMPARE(modCombo->currentIndex(), -1);

    searchA.setFilterState(modsIndex, ModsState{{ModRow{"Saved mod", std::nullopt, std::nullopt}}});
    harness.form.loadFrom(searchA);
    modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    const int savedModIndex = modCombo->findText("Saved mod", Qt::MatchFixedString);
    const int firstModIndex = modCombo->findText("First mod", Qt::MatchFixedString);
    QVERIFY(savedModIndex >= 0);
    QVERIFY(firstModIndex >= 0);
    QCOMPARE(modCombo->currentIndex(), savedModIndex);

    modCombo->setCurrentIndex(firstModIndex);
    QCOMPARE(std::get<ModsState>(searchA.filterStateAt(modsIndex)).rows.front().mod, "First mod");
}

void SearchTest::modsFormAdapterDoesNotArmCompleterOnProgrammaticRows()
{
    mod_list_model().setStringList({"Default mod", "Saved mod"});
    SearchHarness harness;
    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);

    auto *addButton = harness.host.findChild<QPushButton *>("modsAddButton");
    QVERIFY(addButton);
    addButton->click();
    auto *modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    auto *timer = modCombo->findChild<QTimer *>("modsRowCompleterTimer");
    QVERIFY(timer);
    QVERIFY(!timer->isActive());
    QSignalSpy blankRowTimeouts(timer, &QTimer::timeout);
    QTest::qWait(timer->interval() + 75);
    QCOMPARE(blankRowTimeouts.count(), 0);

    searchA.setFilterState(modsIndex, ModsState{{ModRow{"Saved mod", std::nullopt, std::nullopt}}});
    harness.form.loadFrom(searchA);
    modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
    QVERIFY(modCombo);
    timer = modCombo->findChild<QTimer *>("modsRowCompleterTimer");
    QVERIFY(timer);
    QVERIFY(!timer->isActive());
    QSignalSpy restoredRowTimeouts(timer, &QTimer::timeout);
    QTest::qWait(timer->interval() + 75);
    QCOMPARE(restoredRowTimeouts.count(), 0);
}

void SearchTest::modsFormAdapterRepacksRows()
{
    mod_list_model().setStringList({"Saved mod"});
    SearchHarness harness;
    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    searchA.setFilterState(modsIndex, ModsState{{ModRow{"Saved mod", std::nullopt, std::nullopt}}});

    auto *rowsContainer = harness.host.findChild<QWidget *>("modsRowsContainer");
    QVERIFY(rowsContainer);
    for (int cycle = 0; cycle < 3; ++cycle) {
        harness.form.loadFrom(searchB);
        harness.form.loadFrom(searchA);

        auto *modCombo = harness.host.findChild<QComboBox *>("modsRowCombo");
        QVERIFY(modCombo);
        auto *grid = static_cast<QGridLayout *>(rowsContainer->layout());
        QVERIFY(grid);
        const auto *topLeft = grid->itemAtPosition(0, 0);
        QVERIFY(topLeft);
        QCOMPARE(topLeft->widget(), modCombo);
        QCOMPARE(grid->rowCount(), 2);
        QVERIFY(grid->itemAtPosition(2, 0) == nullptr);
    }
}

void SearchTest::modsFormAdapterDisconnectsOnDestruction()
{
    QWidget host;
    QVBoxLayout layout{&host};
    QObject receiver;
    int delayedChanges = 0;
    int stateChanges = 0;
    const FilterCallbacks callbacks{
        &receiver,
        [] {},
        [&delayedChanges] { ++delayedChanges; },
    };
    QPushButton *addButton = nullptr;
    {
        auto form = std::make_unique<ModsFilterForm>(&layout,
                                                     callbacks,
                                                     RefreshMode::Debounced,
                                                     [&stateChanges] { ++stateChanges; });
        addButton = host.findChild<QPushButton *>("modsAddButton");
        QVERIFY(addButton);
    }

    addButton->click();
    QCOMPARE(stateChanges, 0);
    QCOMPARE(delayedChanges, 0);
}

void SearchTest::searchFormUnbindsDeletedSearch()
{
    mod_list_model().setStringList({"Saved mod"});
    SearchHarness harness;
    const qsizetype modsIndex = findModsFilterIndex(harness.catalog);
    QVERIFY(modsIndex >= 0);

    auto search = std::make_unique<Search>(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(*search);
    harness.form.unbind(*search);

    auto *addButton = harness.host.findChild<QPushButton *>("modsAddButton");
    QVERIFY(addButton);
    addButton->click();
    QVERIFY(std::get<ModsState>(search->filterStateAt(modsIndex)).rows.empty());

    search.reset();
    addButton->click();
}

void SearchTest::textAndComboFormAdapterRoundTrip()
{
    SearchHarness harness;
    const qsizetype tabIndex = findTextFilterIndex(harness.catalog, "Tab");
    const qsizetype nameIndex = findTextFilterIndex(harness.catalog, "Name");
    const qsizetype categoryIndex = findComboFilterIndex(harness.catalog, "Category");
    const qsizetype rarityIndex = findComboFilterIndex(harness.catalog, "Rarity");
    QVERIFY(tabIndex >= 0);
    QVERIFY(nameIndex >= 0);
    QVERIFY(categoryIndex >= 0);
    QVERIFY(rarityIndex >= 0);

    auto *tab = harness.findByLabel<QLineEdit>("Tab");
    auto *name = harness.findByLabel<QLineEdit>("Name");
    auto *category = harness.findByLabel<QComboBox>("Type");
    auto *rarity = harness.findByLabel<QComboBox>("Rarity");
    QVERIFY(tab);
    QVERIFY(name);
    QVERIFY(category);
    QVERIFY(rarity);

    const auto *categoryPayload = std::get_if<ComboPayload>(&harness.catalog[categoryIndex].payload);
    const auto *rarityPayload = std::get_if<ComboPayload>(&harness.catalog[rarityIndex].payload);
    QVERIFY(categoryPayload);
    QVERIFY(rarityPayload);
    const QStringList categoryChoices = categoryPayload->choices();
    const QStringList rarityChoices = rarityPayload->choices();
    QCOMPARE(category->count(), categoryChoices.size());
    QCOMPARE(rarity->count(), rarityChoices.size());
    for (qsizetype index = 0; index < categoryChoices.size(); ++index) {
        QCOMPARE(category->itemText(static_cast<int>(index)), categoryChoices.at(index));
    }
    for (qsizetype index = 0; index < rarityChoices.size(); ++index) {
        QCOMPARE(rarity->itemText(static_cast<int>(index)), rarityChoices.at(index));
    }

    const int weaponsIndex = category->findText("Weapons", Qt::MatchFixedString);
    QVERIFY(weaponsIndex >= 0);
    const int rareIndex = rarity->findText("Rare", Qt::MatchFixedString);
    QVERIFY(rareIndex >= 0);

    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    tab->setFocus();
    QTest::keyClick(tab, Qt::Key_T);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);
    name->setFocus();
    QTest::keyClick(name, Qt::Key_N);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 2);

    tab->setText("Alpha Tab");
    name->setText("Alpha");
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 2);

    QVERIFY(category->currentIndex() != weaponsIndex);
    category->setCurrentIndex(weaponsIndex);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 3);
    QVERIFY(rarity->currentIndex() != rareIndex);
    rarity->setCurrentIndex(rareIndex);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 4);

    Search searchA(*harness.buyoutFixture.manager, "A", harness.catalog);
    harness.form.saveTo(searchA);
    QCOMPARE(std::get<TextState>(searchA.filterStateAt(tabIndex)).query, "Alpha Tab");
    QCOMPARE(std::get<TextState>(searchA.filterStateAt(nameIndex)).query, "Alpha");
    QCOMPARE(std::get<ComboState>(searchA.filterStateAt(categoryIndex)).value, "weapons");
    QCOMPARE(std::get<ComboState>(searchA.filterStateAt(rarityIndex)).value, "Rare");

    harness.form.reset();
    Search searchB(*harness.buyoutFixture.manager, "B", harness.catalog);
    harness.form.saveTo(searchB);
    QCOMPARE(std::get<ComboState>(searchB.filterStateAt(categoryIndex)).value, "");
    QCOMPARE(std::get<ComboState>(searchB.filterStateAt(rarityIndex)).value, "");

    category->setCurrentText("");
    harness.form.saveTo(searchB);
    QCOMPARE(std::get<ComboState>(searchB.filterStateAt(categoryIndex)).value, "");

    searchB.setFilterState(rarityIndex, ComboState{"Rare"});
    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    harness.form.loadFrom(searchB);
    QCOMPARE(rarity->currentText(), "Rare");
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 2);

    searchB.setFilterState(rarityIndex, ComboState{"Missing rarity"});
    harness.immediateChanges = 0;
    harness.delayedChanges = 0;
    harness.form.loadFrom(searchB);
    QCOMPARE(rarity->currentIndex(), 0);
    QCOMPARE(rarity->currentText(), rarityPayload->anySentinel);
    QCOMPARE(harness.immediateChanges, 0);
    QCOMPARE(harness.delayedChanges, 1);

    category->setCurrentIndex(weaponsIndex);
    searchB.setFilterState(categoryIndex, ComboState{"Missing category"});
    harness.form.loadFrom(searchB);
    QCOMPARE(category->currentIndex(), 0);
    QCOMPARE(category->currentText(), categoryPayload->anySentinel);

    harness.form.loadFrom(searchA);
    QCOMPARE(tab->text(), "Alpha Tab");
    QCOMPARE(name->text(), "Alpha");
    QCOMPARE(category->currentText(), "Weapons");
    QCOMPARE(rarity->currentText(), "Rare");
}

QTEST_MAIN(SearchTest)

#include "tst_searchform.moc"
