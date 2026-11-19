// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "buyout.h"
#include "testsuite.h"

class BuyoutTest : public QObject
{
    Q_OBJECT

private slots:
    void nullSemantics();
    void statePredicates();
    void aPriceIsWrittenWithItsCurrencysWord();
};

void BuyoutTest::nullSemantics()
{
    const Buyout nullBuyout;
    QVERIFY(nullBuyout.IsNull());

    Buyout changedValue;
    changedValue.value = 1.0;
    QVERIFY(!changedValue.IsNull());

    Buyout changedType;
    changedType.type = Buyout::BUYOUT_TYPE_BUYOUT;
    QVERIFY(!changedType.IsNull());

    Buyout changedSource;
    changedSource.source = Buyout::BUYOUT_SOURCE_GAME;
    QVERIFY(!changedSource.IsNull());

    Buyout changedCurrency;
    changedCurrency.currency = Currency::CURRENCY_CHAOS_ORB;
    QVERIFY(!changedCurrency.IsNull());

    Buyout changedLastUpdate;
    changedLastUpdate.last_update = QDateTime::fromSecsSinceEpoch(1);
    QVERIFY(!changedLastUpdate.IsNull());

    Buyout changedInherited;
    changedInherited.inherited = true;
    QVERIFY(!changedInherited.IsNull());
}

void BuyoutTest::statePredicates()
{
    const Buyout nullBuyout;
    QVERIFY(nullBuyout.IsInherited());
    QVERIFY(!nullBuyout.IsActive());
    QVERIFY(!nullBuyout.RequiresRefresh());

    Buyout inheritedPrice(5.0,
                          Buyout::BUYOUT_TYPE_BUYOUT,
                          Currency::CURRENCY_CHAOS_ORB,
                          QDateTime::fromSecsSinceEpoch(1));
    inheritedPrice.inherited = true;
    QVERIFY(inheritedPrice.IsInherited());
    QVERIFY(inheritedPrice.IsActive());
    QVERIFY(inheritedPrice.RequiresRefresh());

    Buyout priced(5.0,
                  Buyout::BUYOUT_TYPE_BUYOUT,
                  Currency::CURRENCY_CHAOS_ORB,
                  QDateTime::fromSecsSinceEpoch(1));
    QVERIFY(!priced.IsInherited());
    QVERIFY(priced.IsActive());
    QVERIFY(priced.RequiresRefresh());

    Buyout ignored;
    ignored.type = Buyout::BUYOUT_TYPE_IGNORE;
    QVERIFY(!ignored.IsInherited());
    QVERIFY(ignored.IsActive());
    QVERIFY(!ignored.RequiresRefresh());

    Buyout noPrice;
    noPrice.type = Buyout::BUYOUT_TYPE_NO_PRICE;
    QVERIFY(!noPrice.IsInherited());
    QVERIFY(noPrice.IsActive());
    QVERIFY(noPrice.RequiresRefresh());
}

// #426. The text of a price, as the Price column and a priced tab's title
// show it.
void BuyoutTest::aPriceIsWrittenWithItsCurrencysWord()
{
    const QDateTime when = QDateTime::fromSecsSinceEpoch(1);
    const Buyout jewellers(5, Buyout::BUYOUT_TYPE_BUYOUT, Currency::CURRENCY_JEWELLERS_ORB, when);
    QCOMPARE(jewellers.AsText(), QString("b/o 5 jewellers"));
    QCOMPARE(jewellers.CurrencyAsTag(), QString("jew"));

    const Buyout chaos(5, Buyout::BUYOUT_TYPE_FIXED, Currency::CURRENCY_CHAOS_ORB, when);
    QCOMPARE(chaos.AsText(), QString("price 5 chaos"));
}

ACQ_TEST_GUILESS_MAIN(BuyoutTest)

#include "tst_buyout.moc"
