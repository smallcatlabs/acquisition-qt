#include <QComboBox>
#include <QLineEdit>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include "buyout.h"
#include "testfixtures.h"
#include "ui/buyoutpanel.h"

// The price editor on its own (#252): what it commits, and when.
class BuyoutPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void typingCommitsOnlyWhenEditingFinishes();
    void finishingTwiceCommitsOnce();
    void showingAPriceCommitsNothing();
    void textThatIsNotAPriceCommitsNothingAndShows();
    void comboChoiceCommitsAtOnce();
};

namespace {

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
            // An editable chaos price, as a selected unpriced-by-game row
            // would show it.
            panel.setBuyout(makeChaosBuyout(5.0));
        }

        BuyoutPanel panel;
        QComboBox *type{nullptr};
        QComboBox *currency{nullptr};
        QLineEdit *value{nullptr};
    };

} // namespace

// Typing "150" committed 1, 15 and 150, one write each. Now nothing is
// committed until editing finishes, and then 150 once.
void BuyoutPanelTest::typingCommitsOnlyWhenEditingFinishes()
{
    Panel p;
    QVERIFY(p.value->isEnabled());
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    QCOMPARE(committed.count(), 0);

    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
    const auto bo = committed.front().front().value<Buyout>();
    QCOMPARE(bo.value, 150.0);
    QCOMPARE(bo.type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(bo.currency.type, Currency::CURRENCY_CHAOS_ORB);
}

// Return pressed again on text already committed commits nothing more.
void BuyoutPanelTest::finishingTwiceCommitsOnce()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "7");
    QTest::keyClick(p.value, Qt::Key_Return);
    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
}

// A price shown because the selection changed is never committed as if the
// user had entered it, even when editing then "finishes".
void BuyoutPanelTest::showingAPriceCommitsNothing()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "9");
    p.panel.setBuyout(makeChaosBuyout(12.0));
    QCOMPARE(p.value->text(), "12");
    QTest::keyClick(p.value, Qt::Key_Return);

    QCOMPARE(committed.count(), 0);
}

void BuyoutPanelTest::textThatIsNotAPriceCommitsNothingAndShows()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "79/20");
    QTest::keyClick(p.value, Qt::Key_Return);

    QCOMPARE(committed.count(), 0);
    QCOMPARE(p.value->text(), "79/20");
    QVERIFY(!p.value->hasAcceptableInput());
    QVERIFY(!p.value->toolTip().isEmpty());

    // Once the text is a price again, the box no longer says otherwise.
    p.value->clear();
    QTest::keyClicks(p.value, "79");
    QVERIFY(p.value->hasAcceptableInput());
    QVERIFY(p.value->toolTip().isEmpty());
}

// The type and currency are choices, not typing: they commit when chosen,
// with the value the box holds.
void BuyoutPanelTest::comboChoiceCommitsAtOnce()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.currency->setCurrentIndex(Currency::CURRENCY_DIVINE_ORB);
    emit p.currency->activated(Currency::CURRENCY_DIVINE_ORB);

    QCOMPARE(committed.count(), 1);
    const auto bo = committed.front().front().value<Buyout>();
    QCOMPARE(bo.currency.type, Currency::CURRENCY_DIVINE_ORB);
    QCOMPARE(bo.value, 5.0);
}

QTEST_MAIN(BuyoutPanelTest)

#include "tst_buyoutpanel.moc"
