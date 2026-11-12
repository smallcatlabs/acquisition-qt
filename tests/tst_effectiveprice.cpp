// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QApplication>
#include <QSettings>
#include <QSqlQuery>
#include <QtTest/QtTest>

#include "applybuyout.h"
#include "column.h"
#include "filters/filterspec.h"
#include "itemsmanager.h"
#include "testfixtures.h"

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
    void tabTargetsAreLockedAsBefore();

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

    const int chaos = Currency(Currency::CURRENCY_CHAOS_ORB).AsRank();
    const int none = -1;
    const int bo = Buyout::BUYOUT_TYPE_BUYOUT;
    using PS = PriceSource;
    // clang-format off
    //                                           tab name            tab   note             item   delta
    //   text            grey   rank   value  dated  head     priced locked refresh source           postable
    QTest::newRow("nothing")                  << "Dump"          << 0.0 << ""             << none << false
        << ""            << true  << 0     << 0.0 << false << "null"  << false << false << false << PS::None       << false;
    QTest::newRow("note")                     << "Dump"          << 0.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << true  << "now"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("manual item")              << "Dump"          << 0.0 << ""             << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    QTest::newRow("tab name")                 << "~price 2 chaos" << 0.0 << ""            << none << false
        << "price 2 chaos" << true << chaos << 2.0 << true << "now"   << true  << true  << true  << PS::TabName    << false;
    // Since #293 step 2 the date of a price shown from the tab is the tab
    // price's own (here 1970), not when it was first copied onto the item.
    QTest::newRow("manual tab")               << "Dump"          << 3.0 << ""             << none << false
        << "b/o 3 chaos" << true  << chaos << 3.0 << true  << "fixed" << true  << false << true  << PS::ManualTab  << true;
    QTest::newRow("note, tab name")           << "~price 2 chaos" << 0.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << true  << "now"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("manual item, manual tab")  << "Dump"          << 3.0 << ""             << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    // Today's inconsistency, pinned as it is (the maintainer's to rule on):
    // an item's own price in a game-named tab is refused by a price edit
    // but posted to the forum.
    QTest::newRow("manual item, tab name")    << "~price 2 chaos" << 0.0 << ""            << bo   << false
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << true  << true  << PS::ManualItem << true;
    QTest::newRow("note, manual tab")         << "Dump"          << 3.0 << "~b/o 5 chaos" << none << false
        << "b/o 5 chaos" << false << chaos << 5.0 << true  << "now"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("no price")                 << "Dump"          << 0.0 << ""             << int(Buyout::BUYOUT_TYPE_NO_PRICE) << false
        << "no price"    << false << 0     << 0.0 << true  << "fixed" << true  << false << true  << PS::ManualItem << true;
    QTest::newRow("ignore")                   << "Dump"          << 0.0 << ""             << int(Buyout::BUYOUT_TYPE_IGNORE) << false
        << "[ignore]"    << false << 0     << 0.0 << true  << "fixed" << true  << false << false << PS::ManualItem << false;
    // The per-delta pass (ApplyScopedPricing) as the second pass.
    QTest::newRow("note, delta")              << "Dump"          << 0.0 << "~b/o 5 chaos" << none << true
        << "b/o 5 chaos" << false << chaos << 5.0 << true  << "now"   << true  << true  << true  << PS::Note       << false;
    QTest::newRow("tab name, delta")          << "~price 2 chaos" << 0.0 << ""            << none << true
        << "price 2 chaos" << true << chaos << 2.0 << true << "now"   << true  << true  << true  << PS::TabName    << false;
    QTest::newRow("manual tab, delta")        << "Dump"          << 3.0 << ""             << none << true
        << "b/o 3 chaos" << true  << chaos << 3.0 << true  << "fixed" << true  << false << true  << PS::ManualTab  << true;
    QTest::newRow("manual item, tab name, delta") << "~price 2 chaos" << 0.0 << ""        << bo   << true
        << "b/o 7 chaos" << false << chaos << 7.0 << true  << "fixed" << true  << true  << true  << PS::ManualItem << true;
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
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("row_source");
    QTest::addColumn<bool>("inherited");
    QTest::addColumn<double>("row_value");
    addExpectedColumns();

    using PS = PriceSource;
    // clang-format off
    QTest::newRow("manual inherit row")  << "" << "manual" << false << 0.0
        << ""            << true  << 0     << 0.0 << false << "fixed" << false << false << false << PS::None      << false;
    QTest::newRow("game inherit row")    << "" << "game"   << false << 0.0
        << ""            << true  << 0     << 0.0 << false << "fixed" << false << true  << false << PS::Note      << false;
    // Since #293 step 2 an inherited copy is not loaded at all.
    QTest::newRow("left-behind inherited copy") << "b/o" << "manual" << true << 3.0
        << ""            << true  << 0     << 0.0 << false << "null"  << false << false << false << PS::None      << false;
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
    QCOMPARE(f.buyouts.manager->GetTab(named).value, 2.0);
    QCOMPARE(f.buyouts.manager->GetTab(manual).value, 9.0);
}

QTEST_MAIN(EffectivePriceTest)

#include "tst_effectiveprice.moc"
