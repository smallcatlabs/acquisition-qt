// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QApplication>
#include <QFile>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QTest>

#include <algorithm>
#include <set>

#include "applybuyout.h"
#include "column.h"
#include "filters/filterspec.h"
#include "itemsfromwire.h"
#include "itemsmanager.h"
#include "locationinventory.h"
#include "poe/types/character.h"
#include "testfixtures.h"
#include "testsuite.h"
#include "util/json_readers.h"

// #293 step 1: what every reader of an item's price returns, for each way
// an item can come by a price. The reader values were recorded from the
// code before the effective price was introduced (99025f13), and the
// readers must keep returning them; the provenance columns record what
// BuyoutManager::Effective says for the same state.
class EffectivePriceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void readersAgree_data();
    void readersAgree();
    void seededRowsReadAsBefore_data();
    void seededRowsReadAsBefore();
    void emptyPricedTabIsRefreshLocked();
    void upgradeKeepsTheTabPrice_data();
    void upgradeKeepsTheTabPrice();
    void tabTargetsAreLockedAsBefore();
    void activeGameRowsAreIgnoredAndReplaced();
    void itemThatCannotBeListedHasNoPrice();
    void partTakenOffTheWickermanWaitsForTheCharacter();
    void priceFollowsAnItemOntoTheGuardianAndBack();

private:
    QDateTime m_start;
};

Q_DECLARE_METATYPE(PriceSource)

namespace {

    constexpr const char *kItemId = "matrix-item";

    struct Fixture
    {
        Fixture()
            : settings(buyouts.tempDir.filePath("settings.ini"), QSettings::IniFormat)
            , items(settings, *buyouts.manager)
        {}

        BuyoutManagerFixture buyouts;
        QSettings settings;
        ItemsManager items;
    };

    std::shared_ptr<Item> makeItem(const ItemLocation &location, const QString &note)
    {
        const QString note_field = note.isEmpty() ? QString()
                                                  : QString(R"("note": "%1",)").arg(note);
        const QByteArray json = QString(R"({
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "id": "%1",
            "name": "",
            "typeLine": "Iron Ring",
            "baseType": "Iron Ring",
            "identified": true,
            "ilvl": 1,
            %2
            "x": 0,
            "y": 0
        })")
                                    .arg(kItemId, note_field)
                                    .toUtf8();
        return std::make_shared<Item>(makeTestItem(json.constData(), location));
    }

    Buyout typed(Buyout::BuyoutType type)
    {
        return Buyout(0, type, Currency::CURRENCY_NONE, QDateTime::fromSecsSinceEpoch(1));
    }

    const FilterSpec &pricedFilter(const FilterCatalog &catalog)
    {
        for (const FilterSpec &spec : catalog) {
            if (spec.caption == "Priced") {
                return spec;
            }
        }
        qFatal("EffectivePriceTest: no Priced filter");
    }

    // The expected reader values for one state.
    struct Expected
    {
        QString text;      // Price column
        bool grey;         // Price column colour
        int rank;          // Price column sort key
        double value;      // Price column sort key
        bool dated;        // Date column shows a date
        QString date_head; // Date column sort key: "null", "fixed" (1970), "now"
        bool priced;       // the Priced filter
        bool locked;       // a price edit on the item is refused
        bool refreshed;    // the item's tab is refresh-locked
        PriceSource source;
        bool postable;
    };

    void addExpectedColumns()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<bool>("grey");
        QTest::addColumn<int>("rank");
        QTest::addColumn<double>("value");
        QTest::addColumn<bool>("dated");
        QTest::addColumn<QString>("date_head");
        QTest::addColumn<bool>("priced");
        QTest::addColumn<bool>("locked");
        QTest::addColumn<bool>("refreshed");
        QTest::addColumn<PriceSource>("source");
        QTest::addColumn<bool>("postable");
    }

    Expected fetchExpected()
    {
        QFETCH(QString, text);
        QFETCH(bool, grey);
        QFETCH(int, rank);
        QFETCH(double, value);
        QFETCH(bool, dated);
        QFETCH(QString, date_head);
        QFETCH(bool, priced);
        QFETCH(bool, locked);
        QFETCH(bool, refreshed);
        QFETCH(PriceSource, source);
        QFETCH(bool, postable);
        return {
            text, grey, rank, value, dated, date_head, priced, locked, refreshed, source, postable};
    }

    // Every reader, against the expected values. Last, because it writes
    // when not refused: a price edit through ApplyBuyout.
    void checkReaders(Fixture &f,
                      const std::shared_ptr<Item> &item,
                      const ItemLocation &tab,
                      const Expected &e,
                      const QDateTime &start)
    {
        BuyoutManager &manager = *f.buyouts.manager;
        const PriceColumn price(manager);
        QCOMPARE(price.value(*item).toString(), e.text);
        QCOMPARE(price.color(*item) == QColor(0xaa, 0xaa, 0xaa), e.grey);
        const auto head = std::get<ItemSortKey::PriceHead>(price.key(*item).head);
        QCOMPARE(std::get<0>(head), e.rank);
        QCOMPARE(std::get<1>(head), e.value);

        const DateColumn date(manager);
        QCOMPARE(date.value(*item).isValid(), e.dated);
        const QDateTime when = std::get<0>(std::get<ItemSortKey::DateHead>(date.key(*item).head));
        if (e.date_head == "null") {
            QVERIFY(!when.isValid());
        } else if (e.date_head == "fixed") {
            QCOMPARE(when, QDateTime::fromSecsSinceEpoch(1));
        } else {
            QVERIFY2(when >= start.addSecs(-1), qPrintable(when.toString()));
        }

        const FilterCatalog catalog = BuildFilterCatalog(manager);
        const auto &payload = std::get<BoolPayload>(pricedFilter(catalog).payload);
        QCOMPARE(payload.predicate(*item), e.priced);

        QCOMPARE(manager.GetRefreshLocked(tab), e.refreshed);

        const EffectivePrice effective = manager.Effective(*item);
        QCOMPARE(effective.source, e.source);
        QCOMPARE(effective.IsPostable(), e.postable);
        // One invariant: a price the game set carries the game's source.
        QCOMPARE(effective.buyout.IsGameSet(), effective.IsGameSet());

        BuyoutTargets targets;
        targets.items.push_back(item.get());
        const ApplyBuyoutResult result = ApplyBuyout(manager,
                                                     f.items,
                                                     targets,
                                                     makeChaosBuyout(9.0));
        QCOMPARE(result.locked == 1, e.locked);
        // The edit rule is one predicate: the edit is made exactly when the
        // price in force says it may be (#459).
        QCOMPARE(result.applied == 1, effective.Editable());
        // An edit made is the price in force: the item's own (#459).
        if (result.applied == 1) {
            const EffectivePrice edited = manager.Effective(*item);
            QCOMPARE(edited.source, PriceSource::ManualItem);
            QCOMPARE(edited.buyout.value, 9.0);
        }
    }

} // namespace

void EffectivePriceTest::initTestCase()
{
    m_start = QDateTime::currentDateTime();
}

void EffectivePriceTest::readersAgree_data()
{
    // How the item comes by its price.
    QTest::addColumn<QString>("tab_name");
    QTest::addColumn<double>("tab_manual"); // 0: none
    QTest::addColumn<QString>("note");
    QTest::addColumn<int>("item_manual"); // -1: none; else a Buyout type
    QTest::addColumn<bool>("delta");      // the second pass is a tab delta
    addExpectedColumns();

    // The Price column's sort key: the currency's (SortKey), and below
    // every currency for none.
    const int chaos = Currency(Currency::CURRENCY_CHAOS_ORB).SortKey();
    const int nokey = Currency().SortKey();
    const int none = -1;
    const int bo = Buyout::BUYOUT_TYPE_BUYOUT;
    using PS = PriceSource;
    // clang-format off
    //                                           tab name            tab   note             item   delta
    //   text            grey   rank   value  dated  head     priced locked refresh source           postable
    QTest::newRow("nothing")                  << "Dump"          << 0.0 << ""             << none << false
        << ""            << true  << nokey << 0.0 << false << "null"  << false << false << false << PS::None       << false;
    QTest::newRow("note")                     << "Dump"          << 0.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << false << "null"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("manual item")              << "Dump"          << 0.0 << ""             << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    // #459: a tab's name prices the item without locking it; a price of
    // the item's own may be set, and then wins (#365).
    QTest::newRow("tab name")                 << "~price 2 chaos" << 0.0 << ""            << none << false
        << "price 2 chaos" << true << chaos << 2.0 << false << "null"  << true  << false << true  << PS::TabName    << false;
    // Since #293 step 3 a price the game set (a note, a tab's name) has no
    // date: it is never stored, and the game does not say when it was set.
    // Since #293 step 2 the date of a price shown from the tab is the tab
    // price's own (here 1970), not when it was first copied onto the item.
    QTest::newRow("manual tab")               << "Dump"          << 3.0 << ""             << none << false
        << "b/o 3 chaos" << true  << chaos << 3.0 << true  << "fixed" << true  << false << true  << PS::ManualTab  << true;
    QTest::newRow("note, tab name")           << "~price 2 chaos" << 0.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << false << "null"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("manual item, manual tab")  << "Dump"          << 3.0 << ""             << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    // #365: an item's own price is in force in a tab priced by its name
    // (T12: the site lists the forum's price), so it can be edited as well
    // as posted.
    QTest::newRow("manual item, tab name")    << "~price 2 chaos" << 0.0 << ""            << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    QTest::newRow("note, manual tab")         << "Dump"          << 3.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << false << "null"   << true  << true  << true  << PS::Note       << false;
    // Since #293 step 3 the price set in Acquisition stays stored under the
    // game's; the game's is in force, locked and not posted.
    QTest::newRow("note, manual item")        << "Dump"          << 0.0 << "~b/o 5 chaos" << bo   << false
        << "b/o 5 chaos" << false << chaos << 5.0 << false << "null"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("tab name, manual tab")     << "~price 2 chaos" << 3.0 << ""            << none << false
        << "price 2 chaos" << true << chaos << 2.0 << false << "null"  << true  << false << true  << PS::TabName    << false;
    QTest::newRow("no price")                 << "Dump"          << 0.0 << ""             << int(Buyout::BUYOUT_TYPE_NO_PRICE) << false
        << "no price"    << false << nokey << 0.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    QTest::newRow("ignore")                   << "Dump"          << 0.0 << ""             << int(Buyout::BUYOUT_TYPE_IGNORE) << false
        << "[ignore]"    << false << nokey << 0.0 << true  << "fixed" << true  << false << false << PS::ManualItem << false;
    // The per-delta pass (ApplyScopedPricing) as the second pass.
    QTest::newRow("note, delta")              << "Dump"          << 0.0 << "~b/o 5 chaos" << none << true
        << "b/o 5 chaos" << false << chaos << 5.0 << false << "null"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("tab name, delta")          << "~price 2 chaos" << 0.0 << ""            << none << true
        << "price 2 chaos" << true << chaos << 2.0 << false << "null"  << true  << false << true  << PS::TabName    << false;
    QTest::newRow("manual tab, delta")        << "Dump"          << 3.0 << ""             << none << true
        << "b/o 3 chaos" << true  << chaos << 3.0 << true  << "fixed" << true  << false << true  << PS::ManualTab  << true;
    QTest::newRow("manual item, tab name, delta") << "~price 2 chaos" << 0.0 << ""        << bo   << true
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    // clang-format on
}

void EffectivePriceTest::readersAgree()
{
    QFETCH(QString, tab_name);
    QFETCH(double, tab_manual);
    QFETCH(QString, note);
    QFETCH(int, item_manual);
    QFETCH(bool, delta);
    const Expected e = fetchExpected();

    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const ItemLocation tab = makeTestStashLocation("stash-matrix", tab_name, 0);
    const auto item = makeItem(tab, note);
    if (tab_manual > 0) {
        QVERIFY(manager.SetTab(tab, makeChaosBuyout(tab_manual)));
    }
    f.items.OnItemsRefreshed({item}, {tab}, false);
    if (item_manual >= 0) {
        const auto type = static_cast<Buyout::BuyoutType>(item_manual);
        QVERIFY(
            manager.Set(*item,
                        type == Buyout::BUYOUT_TYPE_BUYOUT ? makeChaosBuyout(7.0) : typed(type)));
    }
    if (delta) {
        f.items.OnTabRefreshed(tab, {item});
    } else {
        f.items.OnItemsRefreshed({item}, {tab}, false);
    }
    checkReaders(f, item, tab, e, m_start);
}

// Rows no pass would leave as they are, read as they are found on load:
// a stored "inherit" row; a game row of type inherit (an older build or a
// legacy import can hold one); an inherited copy whose tab no longer has a
// price (left behind, say, by an item absent when the tab was cleared).
void EffectivePriceTest::seededRowsReadAsBefore_data()
{
    const int nokey = Currency().SortKey();
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("row_source");
    QTest::addColumn<bool>("inherited");
    QTest::addColumn<double>("row_value");
    addExpectedColumns();

    using PS = PriceSource;
    // clang-format off
    QTest::newRow("manual inherit row")  << "" << "manual" << false << 0.0
        << ""            << true  << nokey << 0.0 << false << "fixed" << false << false << false << PS::None      << false;
    // Since #293 step 3 a game row is not loaded at all.
    QTest::newRow("game inherit row")    << "" << "game"   << false << 0.0
        << ""            << true  << nokey << 0.0 << false << "null"  << false << false << false << PS::None      << false;
    // Since #293 step 2 an inherited copy is not loaded at all.
    QTest::newRow("left-behind inherited copy") << "b/o" << "manual" << true << 3.0
        << ""            << true  << nokey << 0.0 << false << "null"  << false << false << false << PS::None      << false;
    // clang-format on
}

void EffectivePriceTest::seededRowsReadAsBefore()
{
    QFETCH(QString, type);
    QFETCH(QString, row_source);
    QFETCH(bool, inherited);
    QFETCH(double, row_value);
    const Expected e = fetchExpected();

    Fixture f;
    const ItemLocation tab = makeTestStashLocation("stash-seeded", "Dump", 0);
    const auto item = makeItem(tab, QString());
    QSqlQuery q(*f.buyouts.db);
    QVERIFY(q.prepare("INSERT INTO item_buyouts (item_id, location_id, location_type, currency, "
                      "inherited, last_update, source, type, value) VALUES (:id, :location, "
                      "'stash', :currency, :inherited, :when, :source, :type, :value)"));
    q.bindValue(":id", kItemId);
    q.bindValue(":location", tab.id());
    q.bindValue(":currency", row_value > 0 ? "chaos" : "");
    q.bindValue(":inherited", inherited ? 1 : 0);
    q.bindValue(":when", QDateTime::fromSecsSinceEpoch(1));
    q.bindValue(":source", row_source);
    q.bindValue(":type", type);
    q.bindValue(":value", row_value);
    QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    f.buyouts.manager->ReloadBuyouts();
    QVERIFY(f.buyouts.manager->BuyoutsLoaded());

    checkReaders(f, item, tab, e, m_start);
}

// The upgrade path: a store written before #293 holds, for an item in a tab
// with a price set in Acquisition, only a copy of that price on the item.
// The copy is ignored; the item shows the tab's price, with its value,
// date and provenance, is posted with it, and keeps the tab refresh-locked.
// Beside it, an item's own row of type "inherit" reveals the tab's price
// (it is no price of its own), and a game row of type "inherit" hides it
// (a price the game set, though empty) until a refresh clears that row,
// as a game price whose note has gone is cleared.
void EffectivePriceTest::upgradeKeepsTheTabPrice_data()
{
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("row_source");
    QTest::addColumn<bool>("inherited");
    QTest::addColumn<bool>("hidden_before_refresh");
    QTest::newRow("inherited copy") << "b/o" << "manual" << true << false;
    QTest::newRow("own inherit row") << "" << "manual" << false << false;
    // Since #293 step 3 a game row is ignored on load, so it hides nothing.
    QTest::newRow("game inherit row") << "" << "game" << false << false;
}

void EffectivePriceTest::upgradeKeepsTheTabPrice()
{
    QFETCH(QString, type);
    QFETCH(QString, row_source);
    QFETCH(bool, inherited);
    QFETCH(bool, hidden_before_refresh);

    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const ItemLocation tab = makeTestStashLocation("stash-upgrade", "Dump", 0);
    const auto item = makeItem(tab, QString());
    const QDateTime tab_date = QDateTime::fromSecsSinceEpoch(1000);
    QVERIFY(manager.SetTab(tab,
                           Buyout(3.0,
                                  Buyout::BUYOUT_TYPE_BUYOUT,
                                  Currency::CURRENCY_CHAOS_ORB,
                                  tab_date)));
    QSqlQuery q(*f.buyouts.db);
    QVERIFY(q.prepare("INSERT INTO item_buyouts (item_id, location_id, location_type, currency, "
                      "inherited, last_update, source, type, value) VALUES (:id, :location, "
                      "'stash', 'chaos', :inherited, :when, :source, :type, 7)"));
    q.bindValue(":id", kItemId);
    q.bindValue(":location", tab.id());
    q.bindValue(":inherited", inherited ? 1 : 0);
    q.bindValue(":when", QDateTime::fromSecsSinceEpoch(5));
    q.bindValue(":source", row_source);
    q.bindValue(":type", type);
    QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    manager.ReloadBuyouts();
    QVERIFY(manager.BuyoutsLoaded());

    const auto shows_tab_price = [&] {
        const EffectivePrice price = manager.Effective(*item);
        return (price.source == PriceSource::ManualTab) && (price.buyout.value == 3.0)
               && (price.buyout.last_update == tab_date) && price.buyout.inherited;
    };
    QCOMPARE(shows_tab_price(), !hidden_before_refresh);

    f.items.OnItemsRefreshed({item}, {tab}, false);
    QVERIFY(shows_tab_price());
    const auto postable = manager.PostablePrices(f.items.items());
    QCOMPARE(postable.size(), size_t(1));
    QCOMPARE(postable.front().second.value, 3.0);
    QVERIFY(manager.GetRefreshLocked(tab));
}

// A tab whose own price requires a refresh is locked from the tab list even
// with no items loaded (#148).
void EffectivePriceTest::emptyPricedTabIsRefreshLocked()
{
    Fixture f;
    const ItemLocation named = makeTestStashLocation("stash-empty-named", "~price 2 chaos", 0);
    const ItemLocation manual = makeTestStashLocation("stash-empty-manual", "Dump", 1);
    QVERIFY(f.buyouts.manager->SetTab(manual, makeChaosBuyout(3.0)));
    f.items.OnItemsRefreshed({}, {named, manual}, false);
    QVERIFY(f.buyouts.manager->GetRefreshLocked(named));
    QVERIFY(f.buyouts.manager->GetRefreshLocked(manual));
}

// A price edit on a tab: refused for a tab named with a price, applied to
// a tab with a price set in Acquisition or none.
void EffectivePriceTest::tabTargetsAreLockedAsBefore()
{
    Fixture f;
    const ItemLocation named = makeTestStashLocation("stash-t-named", "~price 2 chaos", 0);
    const ItemLocation manual = makeTestStashLocation("stash-t-manual", "Dump", 1);
    const ItemLocation plain = makeTestStashLocation("stash-t-plain", "Plain", 2);
    QVERIFY(f.buyouts.manager->SetTab(manual, makeChaosBuyout(3.0)));
    f.items.OnItemsRefreshed({}, {named, manual, plain}, false);

    BuyoutTargets targets;
    targets.tabs = {named, manual, plain};
    const ApplyBuyoutResult result = ApplyBuyout(*f.buyouts.manager,
                                                 f.items,
                                                 targets,
                                                 makeChaosBuyout(9.0));
    QCOMPARE(result.locked, 1);
    QCOMPARE(result.applied, 2);
    QCOMPARE(f.buyouts.manager->EffectiveTab(named).buyout.value, 2.0);
    QCOMPARE(f.buyouts.manager->GetTab(manual).value, 9.0);
}

// A store as the previous release leaves it: an active price read from the
// game (type b/o, source game) in each table. Both rows are ignored on
// load, so nothing is priced or locked by them, and a later price set in
// Acquisition replaces each row.
void EffectivePriceTest::activeGameRowsAreIgnoredAndReplaced()
{
    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const ItemLocation tab = makeTestStashLocation("stash-old-game", "Dump", 0);
    const auto item = makeItem(tab, QString());
    QSqlQuery q(*f.buyouts.db);
    QVERIFY(q.prepare("INSERT INTO item_buyouts (item_id, location_id, location_type, currency, "
                      "inherited, last_update, source, type, value) VALUES (:id, :location, "
                      "'stash', 'chaos', 0, :when, 'game', 'b/o', 5)"));
    q.bindValue(":id", kItemId);
    q.bindValue(":location", tab.id());
    q.bindValue(":when", QDateTime::fromSecsSinceEpoch(5));
    QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    QVERIFY(q.prepare("INSERT INTO location_buyouts (location_id, location_type, currency, "
                      "inherited, last_update, source, type, value) VALUES (:location, 'stash', "
                      "'chaos', 0, :when, 'game', 'b/o', 4)"));
    q.bindValue(":location", tab.id());
    q.bindValue(":when", QDateTime::fromSecsSinceEpoch(5));
    QVERIFY2(q.exec(), qPrintable(q.lastError().text()));
    manager.ReloadBuyouts();
    QVERIFY(manager.BuyoutsLoaded());

    f.items.OnItemsRefreshed({item}, {tab}, false);
    QCOMPARE(manager.Effective(*item).source, PriceSource::None);
    QCOMPARE(manager.EffectiveTab(tab).source, PriceSource::None);
    QVERIFY(!manager.GetRefreshLocked(tab));
    QVERIFY(manager.PostablePrices(f.items.items()).empty());

    QVERIFY(manager.Set(*item, makeChaosBuyout(3.0)));
    QVERIFY(manager.SetTab(tab, makeChaosBuyout(2.0)));
    QVERIFY(q.exec("SELECT source, value FROM item_buyouts"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("manual"));
    QCOMPARE(q.value(1).toDouble(), 3.0);
    QVERIFY(!q.next());
    QVERIFY(q.exec("SELECT source, value FROM location_buyouts"));
    QVERIFY(q.next());
    QCOMPARE(q.value(0).toString(), QString("manual"));
    QCOMPARE(q.value(1).toDouble(), 2.0);
    QVERIFY(!q.next());

    manager.ReloadBuyouts();
    QCOMPARE(manager.Effective(*item).source, PriceSource::ManualItem);
    QCOMPARE(manager.Effective(*item).buyout.value, 3.0);
    QCOMPARE(manager.EffectiveTab(tab).source, PriceSource::ManualTab);
    QCOMPARE(manager.EffectiveTab(tab).buyout.value, 2.0);
}

// #418, F63: an item on a character's guardian cannot be listed for sale
// where it is. It shows under its character, and has no price from any
// source: not its note, not a price set on it, not its character's. The
// character's own price is untouched. An edit on a selection that holds it
// leaves it alone and says why, and nothing posts it or another copy of its
// id. A price set on it before it went in stays stored, and is in force
// again once it is taken out.
void EffectivePriceTest::itemThatCannotBeListedHasNoPrice()
{
    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    poe::Character character;
    character.id = "character-418";
    character.name = "Builder";
    const ItemLocation self(character);
    const ItemLocation guardian = self.inContainer(ItemLocation::Container::Guardian);
    const ItemLocation stash = makeTestStashLocation();

    const auto in_stash = makeItem(stash, "");
    QVERIFY(manager.Set(*in_stash, makeChaosBuyout(5.0)));
    QCOMPARE(manager.Effective(*in_stash).source, PriceSource::ManualItem);

    // Moved onto the guardian, with a price in its note, and its character
    // priced too.
    QVERIFY(manager.SetTab(self, makeChaosBuyout(7.0)));
    const auto in_guardian = makeItem(guardian, "~b/o 3 chaos");
    QVERIFY(in_guardian->HasNotePrice());
    const EffectivePrice price = manager.Effective(*in_guardian);
    QCOMPARE(price.source, PriceSource::Unlistable);
    QVERIFY(price.CannotBeListed());
    QVERIFY(!price.Editable());
    QVERIFY(price.buyout.IsNull());
    QVERIFY(!price.IsGameSet());
    QVERIFY(!price.IsPostable());
    QCOMPARE(manager.EffectiveTab(self).source, PriceSource::ManualTab);
    QCOMPARE(manager.Get(*in_guardian).value, 5.0);

    QVERIFY(manager.PostablePrices({in_guardian}).empty());
    QVERIFY(manager.PostablePrices({in_stash, in_guardian}).empty());
    QCOMPARE(manager.PostablePrices({in_stash}).size(), size_t(1));

    // A selection of the guardian's item and an ordinary one, as the price
    // editor resolves and applies it.
    const auto ordinary = std::make_shared<Item>(makeTestItem("ordinary-418"));
    LocationInventory locations;
    locations.ResetTo({self, stash});
    PriceTargets rows;
    rows.rows.push_back(PriceTarget::ForItem(in_guardian));
    rows.rows.push_back(PriceTarget::ForItem(ordinary));
    const ResolvedTargets resolved = ResolvePriceTargets(rows, {in_guardian, ordinary}, locations);
    QCOMPARE(resolved.missing, 0);
    QCOMPARE(resolved.targets.items.size(), size_t(2));
    const ApplyBuyoutResult result = ApplyBuyout(manager,
                                                 f.items,
                                                 resolved.targets,
                                                 makeChaosBuyout(9.0));
    QCOMPARE(result.unlistable, 1);
    QCOMPARE(result.applied, 1);
    QCOMPARE(result.locked, 0);
    QVERIFY(ApplyBuyoutNotice(result, true).contains("cannot be listed"));
    QCOMPARE(manager.Get(*in_guardian).value, 5.0);
    QCOMPARE(manager.Get(*ordinary).value, 9.0);

    const auto taken_out = makeItem(stash, "");
    QCOMPARE(manager.Effective(*taken_out).source, PriceSource::ManualItem);
    QCOMPARE(manager.Effective(*taken_out).buyout.value, 5.0);
}

namespace {

    poe::Character realCharacter(const char *file)
    {
        QFile reply(QFINDTESTDATA(QString("fixtures/payloads/%1").arg(file)));
        if (!reply.open(QIODevice::ReadOnly)) {
            qFatal("missing payload %s", file);
        }
        const auto character = json::readCharacter(reply.readAll());
        if (!character) {
            qFatal("unreadable payload %s", file);
        }
        return *character;
    }

    std::set<QString> postableIds(const BuyoutManager &manager, const Items &items)
    {
        std::set<QString> ids;
        for (const auto &[item, buyout] : manager.PostablePrices(items)) {
            ids.insert(item->id());
        }
        return ids;
    }

} // namespace

// #418, the rule kept on review: while any copy of an id cannot be listed,
// no copy of it is posted. A part taken off the Wickerman, put in a stash
// tab and priced there, with only that tab refreshed: the character's last
// reply still has the part on the Wickerman, so the priced stash copy waits.
// Refreshing the character, whose reply no longer has it, lets it be posted.
// The part is a real one (character-1.json).
void EffectivePriceTest::partTakenOffTheWickermanWaitsForTheCharacter()
{
    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const poe::Character character = realCharacter("character-1.json");
    QVERIFY(character.wickerman && !character.wickerman->empty());
    const ItemLocation location(character);
    const ItemLocation stash = makeTestStashLocation();

    Items published;
    AppendWireItems(character, location, published);
    f.items.OnItemsRefreshed(published, {location, stash}, false);

    Items in_stash;
    AppendWireItems({character.wickerman->front()}, stash, in_stash);
    const QString part = in_stash.front()->id();
    f.items.OnTabRefreshed(stash, in_stash);
    QVERIFY(manager.Set(*in_stash.front(), makeChaosBuyout(4.0)));
    QCOMPARE(manager.Effective(*in_stash.front()).source, PriceSource::ManualItem);
    QVERIFY(!postableIds(manager, f.items.items()).contains(part));

    poe::Character fetched = character;
    fetched.wickerman->erase(fetched.wickerman->begin());
    Items replaced;
    AppendWireItems(fetched, location, replaced);
    f.items.OnTabRefreshed(location, replaced);
    QVERIFY(postableIds(manager, f.items.items()).contains(part));
}

// #418: an item of a real reply (character-2.json's guardian) priced where
// it can be listed, then in the guardian in the next reply, then outside it
// again: no price in force while it is there, the stored price kept, and
// in force again after.
void EffectivePriceTest::priceFollowsAnItemOntoTheGuardianAndBack()
{
    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const poe::Character on_guardian = realCharacter("character-2.json");
    QVERIFY(on_guardian.guardian && !on_guardian.guardian->empty());
    const ItemLocation location(on_guardian);
    const poe::Item moving = on_guardian.guardian->front();

    poe::Character off_guardian = on_guardian;
    off_guardian.guardian->erase(off_guardian.guardian->begin());
    off_guardian.inventory->push_back(moving);
    off_guardian.inventory->back().inventoryId = "MainInventory";

    const auto find = [&moving](const Items &items) {
        const auto it = std::ranges::find_if(items, [&moving](const auto &item) {
            return item->id() == moving.id;
        });
        if (it == items.end()) {
            qFatal("the moving item is missing");
        }
        return *it;
    };

    Items first;
    AppendWireItems(off_guardian, location, first);
    QVERIFY(manager.Set(*find(first), makeChaosBuyout(6.0)));
    QCOMPARE(manager.Effective(*find(first)).source, PriceSource::ManualItem);

    Items second;
    AppendWireItems(on_guardian, location, second);
    QCOMPARE(manager.Effective(*find(second)).source, PriceSource::Unlistable);
    QCOMPARE(manager.Get(*find(second)).value, 6.0);

    Items third;
    AppendWireItems(off_guardian, location, third);
    QCOMPARE(manager.Effective(*find(third)).source, PriceSource::ManualItem);
    QCOMPARE(manager.Effective(*find(third)).buyout.value, 6.0);
}

ACQ_TEST_MAIN(EffectivePriceTest)

#include "tst_effectiveprice.moc"
