// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QApplication>
#include <QSettings>
#include <QtTest/QtTest>

#include "applybuyout.h"
#include "column.h"
#include "filters/filterspec.h"
#include "itemsmanager.h"
#include "testfixtures.h"

// #293 step 1: what every reader of an item's price returns, for each way
// an item can come by a price. Recorded from the code as it was before the
// effective price was introduced; the readers must keep returning it.
class EffectivePriceTest : public QObject
{
    Q_OBJECT

private slots:
    void readersAgree_data();
    void readersAgree();
};

namespace {

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
            "id": "matrix-item",
            "name": "",
            "typeLine": "Iron Ring",
            "baseType": "Iron Ring",
            "identified": true,
            "ilvl": 1,
            %1
            "x": 0,
            "y": 0
        })")
                                    .arg(note_field)
                                    .toUtf8();
        return std::make_shared<Item>(makeTestItem(json.constData(), location));
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

} // namespace

void EffectivePriceTest::readersAgree_data()
{
    // How the item comes by its price.
    QTest::addColumn<QString>("tab_name");
    QTest::addColumn<double>("tab_manual"); // 0: none
    QTest::addColumn<QString>("note");
    QTest::addColumn<double>("item_manual"); // 0: none
    // What the readers return.
    QTest::addColumn<QString>("text");   // Price column
    QTest::addColumn<bool>("grey");      // Price column colour: inherited
    QTest::addColumn<int>("rank");       // Price column sort key
    QTest::addColumn<double>("value");   // Price column sort key
    QTest::addColumn<bool>("dated");     // Date column shows a date
    QTest::addColumn<bool>("priced");    // the Priced filter
    QTest::addColumn<bool>("locked");    // a price edit is refused
    QTest::addColumn<bool>("refreshed"); // the tab is refresh-locked

    const int chaos = Currency(Currency::CURRENCY_CHAOS_ORB).AsRank();
    // clang-format off
    QTest::newRow("nothing")                  << "Dump" << 0.0 << "" << 0.0
        << "" << true << 0 << 0.0 << false << false << false << false;
    QTest::newRow("note")                     << "Dump" << 0.0 << "~b/o 5 chaos" << 0.0
        << "b/o 5 chaos" << false << chaos << 5.0 << true << true << true << true;
    QTest::newRow("manual item")              << "Dump" << 0.0 << "" << 7.0
        << "b/o 7 chaos" << false << chaos << 7.0 << true << true << false << true;
    QTest::newRow("tab name")                 << "~price 2 chaos" << 0.0 << "" << 0.0
        << "price 2 chaos" << true << chaos << 2.0 << true << true << true << true;
    QTest::newRow("manual tab")               << "Dump" << 3.0 << "" << 0.0
        << "b/o 3 chaos" << true << chaos << 3.0 << true << true << false << true;
    QTest::newRow("note, tab name")           << "~price 2 chaos" << 0.0 << "~b/o 5 chaos" << 0.0
        << "b/o 5 chaos" << false << chaos << 5.0 << true << true << true << true;
    QTest::newRow("manual item, manual tab")  << "Dump" << 3.0 << "" << 7.0
        << "b/o 7 chaos" << false << chaos << 7.0 << true << true << false << true;
    QTest::newRow("manual item, tab name")    << "~price 2 chaos" << 0.0 << "" << 7.0
        << "b/o 7 chaos" << false << chaos << 7.0 << true << true << true << true;
    QTest::newRow("note, manual tab")         << "Dump" << 3.0 << "~b/o 5 chaos" << 0.0
        << "b/o 5 chaos" << false << chaos << 5.0 << true << true << true << true;
    // clang-format on
}

void EffectivePriceTest::readersAgree()
{
    QFETCH(QString, tab_name);
    QFETCH(double, tab_manual);
    QFETCH(QString, note);
    QFETCH(double, item_manual);
    QFETCH(QString, text);
    QFETCH(bool, grey);
    QFETCH(int, rank);
    QFETCH(double, value);
    QFETCH(bool, dated);
    QFETCH(bool, priced);
    QFETCH(bool, locked);
    QFETCH(bool, refreshed);

    Fixture f;
    BuyoutManager &manager = *f.buyouts.manager;
    const ItemLocation tab = makeTestStashLocation("stash-matrix", tab_name, 0);
    const auto item = makeItem(tab, note);
    if (tab_manual > 0) {
        QVERIFY(manager.SetTab(tab, makeChaosBuyout(tab_manual)));
    }
    f.items.OnItemsRefreshed({item}, {tab}, false);
    if (item_manual > 0) {
        QVERIFY(manager.Set(*item, makeChaosBuyout(item_manual)));
        f.items.OnItemsRefreshed({item}, {tab}, false);
    }

    const PriceColumn price(manager);
    QCOMPARE(price.value(*item).toString(), text);
    QCOMPARE(price.color(*item) == QColor(0xaa, 0xaa, 0xaa), grey);
    const auto head = std::get<ItemSortKey::PriceHead>(price.key(*item).head);
    QCOMPARE(std::get<0>(head), rank);
    QCOMPARE(std::get<1>(head), value);

    const DateColumn date(manager);
    QCOMPARE(date.value(*item).isValid(), dated);

    const FilterCatalog catalog = BuildFilterCatalog(manager);
    const auto &payload = std::get<BoolPayload>(pricedFilter(catalog).payload);
    QCOMPARE(payload.predicate(*item), priced);

    QCOMPARE(manager.GetRefreshLocked(tab), refreshed);

    // Last: a price edit writes when it is not refused.
    BuyoutTargets targets;
    targets.items.push_back(item.get());
    const ApplyBuyoutResult result = ApplyBuyout(manager, f.items, targets, makeChaosBuyout(9.0));
    QCOMPARE(result.locked == 1, locked);
}

QTEST_MAIN(EffectivePriceTest)

#include "tst_effectiveprice.moc"
