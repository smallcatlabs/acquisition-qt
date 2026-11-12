// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "buyout.h"
#include "testfixtures.h"
#include "ui/buyoutpanel.h"

// The price editor on its own (#252): what it commits, when, and to which
// rows. Nothing here gives the panel focus or shows it: no commit depends on
// either.
class BuyoutPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void typingCommitsOnlyOnEnter();
    void enterAgainCommitsNothingMore();
    void showingAPriceCommitsNothing();
    void textThatIsNotAPriceCommitsNothingAndShows();
    void comboChoiceCommitsAtOnce();
    void sameTargetsKeepTheTypedValue();
    void otherTargetsCommitTheTypedValueToTheOldOnes();
    void otherTargetsDropTextThatIsNotAPrice();
};

namespace {

    PriceTargets items(std::initializer_list<QString> ids)
    {
        PriceTargets targets;
        targets.item_ids = ids;
        return targets;
    }

    struct Panel
    {
        Panel()
        {
            type = panel.findChild<QComboBox *>("buyoutTypeComboBox");
            currency = panel.findChild<QComboBox *>("buyoutCurrencyComboBox");
            value = panel.findChild<QLineEdit *>("buyoutValueLineEdit");
            if (!type || !currency || !value) {
                qFatal("BuyoutPanelTest: a control is missing");
            }
            // An editable chaos price for one item.
            panel.showPrice(items({"item-a"}), makeChaosBuyout(5.0));
        }

        BuyoutPanel panel;
        QComboBox *type{nullptr};
        QComboBox *currency{nullptr};
        QLineEdit *value{nullptr};
    };

    Buyout committedBuyout(const QSignalSpy &spy, qsizetype n = 0)
    {
        return spy.at(n).at(1).value<Buyout>();
    }

    PriceTargets committedTargets(const QSignalSpy &spy, qsizetype n = 0)
    {
        return spy.at(n).at(0).value<PriceTargets>();
    }

} // namespace

// Typing "150" committed 1, 15 and 150, one write each. Now nothing is
// committed until Enter, and then 150 once, to the rows shown.
void BuyoutPanelTest::typingCommitsOnlyOnEnter()
{
    Panel p;
    QVERIFY(p.value->isEnabled());
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    QCOMPARE(committed.count(), 0);

    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(committedBuyout(committed).value, 150.0);
    QCOMPARE(committedBuyout(committed).currency.type, Currency::CURRENCY_CHAOS_ORB);
    QVERIFY(committedTargets(committed) == items({"item-a"}));
}

void BuyoutPanelTest::enterAgainCommitsNothingMore()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "7");
    QTest::keyClick(p.value, Qt::Key_Return);
    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
}

// A price the panel was shown is not the user's: Enter on it, showing other
// prices and rows, and showing nothing commit nothing.
void BuyoutPanelTest::showingAPriceCommitsNothing()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.panel.showPrice(items({"item-a"}), makeChaosBuyout(12.0));
    QCOMPARE(p.value->text(), "12");
    QTest::keyClick(p.value, Qt::Key_Return);
    p.panel.showNothing(items({"item-b"}));
    p.panel.showPrice(items({"item-c"}), makeChaosBuyout(13.0));
    p.panel.finishEditing();

    QCOMPARE(committed.count(), 0);
}

void BuyoutPanelTest::textThatIsNotAPriceCommitsNothingAndShows()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);

    p.value->clear();
    QTest::keyClicks(p.value, "79/20");
    QTest::keyClick(p.value, Qt::Key_Return);

    QCOMPARE(committed.count(), 0);
    QCOMPARE(p.value->text(), "79/20");
    QVERIFY(!p.value->toolTip().isEmpty());
    QCOMPARE(notices.count(), 1);
    QVERIFY(notices.front().front().toString().contains("not a price"));

    // Once the text is a price again, the box no longer says otherwise.
    p.value->clear();
    QTest::keyClicks(p.value, "79");
    QVERIFY(p.value->toolTip().isEmpty());
}

// The type and currency are choices, not typing: they commit when chosen,
// with the value the box holds, typed or not, once.
void BuyoutPanelTest::comboChoiceCommitsAtOnce()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    p.currency->setCurrentIndex(Currency::CURRENCY_DIVINE_ORB);
    emit p.currency->activated(Currency::CURRENCY_DIVINE_ORB);

    QCOMPARE(committed.count(), 1);
    QCOMPARE(committedBuyout(committed).currency.type, Currency::CURRENCY_DIVINE_ORB);
    QCOMPARE(committedBuyout(committed).value, 150.0);
    // The typed value is committed; Enter now has nothing to add.
    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
}

// Shown the same rows again, as a refresh does when it re-adopts the
// selection, the panel keeps the typed value; Enter then commits it.
void BuyoutPanelTest::sameTargetsKeepTheTypedValue()
{
    PriceTargets both;
    both.item_ids = {"item-a", "item-b"};
    PriceTargets reordered;
    reordered.item_ids = {"item-b", "item-a"};
    BuyoutPanel panel;
    auto *value = panel.findChild<QLineEdit *>("buyoutValueLineEdit");
    panel.showPrice(both, makeChaosBuyout(5.0));
    QSignalSpy committed(&panel, &BuyoutPanel::committed);

    value->clear();
    QTest::keyClicks(value, "150");
    panel.showPrice(reordered, makeChaosBuyout(12.0));
    QCOMPARE(value->text(), "150");
    QCOMPARE(committed.count(), 0);

    QTest::keyClick(value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
    QVERIFY(committedTargets(committed) == both);
}

// Shown other rows, the panel commits the typed value to the rows it was
// typed for, then shows the new rows' price.
void BuyoutPanelTest::otherTargetsCommitTheTypedValueToTheOldOnes()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    p.panel.showPrice(items({"item-b"}), makeChaosBuyout(12.0));

    QCOMPARE(committed.count(), 1);
    QVERIFY(committedTargets(committed) == items({"item-a"}));
    QCOMPARE(committedBuyout(committed).value, 150.0);
    QCOMPARE(p.value->text(), "12");

    p.panel.showNothing(items({}));
    QCOMPARE(committed.count(), 1);
}

// Text that is not a price, pending when other rows are shown, is dropped,
// and the panel says it was not applied.
void BuyoutPanelTest::otherTargetsDropTextThatIsNotAPrice()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);

    p.value->clear();
    QTest::keyClicks(p.value, "abc");
    p.panel.showPrice(items({"item-b"}), makeChaosBuyout(12.0));

    QCOMPARE(committed.count(), 0);
    QCOMPARE(notices.count(), 1);
    QVERIFY(notices.front().front().toString().contains("not applied"));
    QCOMPARE(p.value->text(), "12");
}

QTEST_MAIN(BuyoutPanelTest)

#include "tst_buyoutpanel.moc"
