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
    void shownPriceIsReadAfterTheCommit();
    void emptiedBoxSaysSo();
    void numberWithoutCurrencyWaitsForOne();
    void noTargetsDisablesThePanel();
    void finishEditingKeepsTextItCouldNotCommit();
    void typeChoiceWithEmptyBoxStaysPendingQuietly();
    void currencyBoxSelectsByTag();
};

namespace {

    PriceTargets items(std::initializer_list<const char *> ids)
    {
        PriceTargets targets;
        for (const char *id : ids) {
            targets.rows.push_back(PriceTarget::ForItem(std::make_shared<Item>(makeTestItem(id))));
        }
        return targets;
    }

    // A panel over a little store of prices by item id: what a commit
    // writes, the panel reads back, as it does from the buyout manager.
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
            panel.setPriceLookup([this](const PriceTarget &target) {
                const auto it = prices.find(target.item->id());
                return (it != prices.end()) ? it->second : Buyout();
            });
            QObject::connect(&panel,
                             &BuyoutPanel::committed,
                             [this](const PriceTargets &targets, const Buyout &buyout) {
                                 for (const PriceTarget &row : targets.rows) {
                                     prices[row.item->id()] = buyout;
                                 }
                             });
            // An editable chaos price for one item.
            prices["item-a"] = makeChaosBuyout(5.0);
            prices["item-b"] = makeChaosBuyout(12.0);
            prices["item-c"] = makeChaosBuyout(13.0);
            panel.showTargets(items({"item-a"}));
        }

        std::map<QString, Buyout> prices;
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

    p.panel.showTargets(items({"item-b"}));
    QCOMPARE(p.value->text(), "12");
    QTest::keyClick(p.value, Qt::Key_Return);
    p.panel.showTargets(items({}));
    p.panel.showTargets(items({"item-c"}));
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
    Panel p;
    p.panel.showTargets(items({"item-a", "item-b"}));
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    p.panel.showTargets(items({"item-b", "item-a"}));
    QCOMPARE(p.value->text(), "150");
    QCOMPARE(committed.count(), 0);

    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
    QVERIFY(committedTargets(committed) == items({"item-a", "item-b"}));
}

// Shown other rows, the panel commits the typed value to the rows it was
// typed for, then shows the new rows' price, read after the commit.
void BuyoutPanelTest::otherTargetsCommitTheTypedValueToTheOldOnes()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);

    p.value->clear();
    QTest::keyClicks(p.value, "150");
    p.panel.showTargets(items({"item-b"}));

    QCOMPARE(committed.count(), 1);
    QVERIFY(committedTargets(committed) == items({"item-a"}));
    QCOMPARE(committedBuyout(committed).value, 150.0);
    QCOMPARE(p.value->text(), "12");

    p.panel.showTargets(items({}));
    QCOMPARE(committed.count(), 1);
}

// When the new rows include the rows typed for, what is shown is their
// price after the commit: the first target's, now the typed one.
void BuyoutPanelTest::shownPriceIsReadAfterTheCommit()
{
    Panel p;
    p.value->clear();
    QTest::keyClicks(p.value, "150");

    p.panel.showTargets(items({"item-a", "item-b"}));

    QCOMPARE(p.prices.at("item-a").value, 150.0);
    QCOMPARE(p.value->text(), "150");
    QVERIFY(!p.value->isModified());
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
    p.panel.showTargets(items({"item-b"}));

    QCOMPARE(committed.count(), 0);
    QCOMPARE(notices.count(), 1);
    QVERIFY(notices.front().front().toString().contains("not applied"));
    QCOMPARE(p.value->text(), "12");
}

// An emptied value box is not a price either: Enter says so and keeps the
// edit pending; other rows shown then say it was not applied.
void BuyoutPanelTest::emptiedBoxSaysSo()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);

    QTest::keyClick(p.value, Qt::Key_Backspace);
    QCOMPARE(p.value->text(), QString());
    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 0);
    QCOMPARE(notices.count(), 1);
    QVERIFY(p.value->isModified());

    p.panel.showTargets(items({"item-b"}));
    QCOMPARE(committed.count(), 0);
    QCOMPARE(notices.count(), 2);
    QVERIFY(notices.back().front().toString().contains("nothing was applied"));
}

// A number with no currency chosen is not committed: the panel says a
// currency is needed and the number stays, pending, until one is chosen.
void BuyoutPanelTest::numberWithoutCurrencyWaitsForOne()
{
    Panel p;
    p.prices["item-d"] = Buyout();
    p.panel.showTargets(items({"item-d"}));
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);
    // An unpriced row keeps the currency box as it was; here none is set.
    p.currency->setCurrentIndex(Currency::CURRENCY_NONE);
    p.type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    emit p.type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    QTest::keyClicks(p.value, "5");

    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 0);
    QVERIFY(notices.back().front().toString().contains("currency"));
    QCOMPARE(p.value->text(), "5");

    QTest::keyClick(p.currency, Qt::Key_Down);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(committedBuyout(committed).value, 5.0);
    QCOMPARE(committedBuyout(committed).currency.type, Currency::CURRENCY_ORB_OF_ALTERATION);
}

// No targets: nothing to edit.
void BuyoutPanelTest::noTargetsDisablesThePanel()
{
    Panel p;
    QVERIFY(p.type->isEnabled());
    p.panel.showTargets(items({}));
    QVERIFY(!p.type->isEnabled());
    QVERIFY(!p.currency->isEnabled());
    QVERIFY(!p.value->isEnabled());
    QCOMPARE(p.value->text(), QString());
}

// finishEditing() (a cancelled window close, a manual shop update, a
// clipboard copy) on text that is not a price says why and keeps it
// pending: Enter afterwards still says why.
void BuyoutPanelTest::finishEditingKeepsTextItCouldNotCommit()
{
    Panel p;
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);
    p.value->clear();
    QTest::keyClicks(p.value, "abc");

    p.panel.finishEditing();
    QCOMPARE(notices.count(), 1);
    QCOMPARE(p.value->text(), "abc");

    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(notices.count(), 2);
    QCOMPARE(committed.count(), 0);
}

// Choosing a priced type on an unpriced row with the box empty is a first
// step, not an error: no notice, and the choice stays pending through the
// same rows being shown again; typing a number and Enter then commit once.
void BuyoutPanelTest::typeChoiceWithEmptyBoxStaysPendingQuietly()
{
    Panel p;
    p.prices["item-d"] = Buyout();
    p.panel.showTargets(items({"item-d"}));
    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    QSignalSpy notices(&p.panel, &BuyoutPanel::notice);

    p.type->setCurrentIndex(Buyout::BUYOUT_TYPE_BUYOUT);
    emit p.type->activated(Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(notices.count(), 0);
    QVERIFY(p.value->isEnabled());

    p.panel.showTargets(items({"item-d"}));
    QCOMPARE(p.type->currentIndex(), int(Buyout::BUYOUT_TYPE_BUYOUT));
    QVERIFY(p.value->isEnabled());

    QTest::keyClicks(p.value, "5");
    QTest::keyClick(p.value, Qt::Key_Return);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(committedBuyout(committed).type, Buyout::BUYOUT_TYPE_BUYOUT);
    QCOMPARE(committedBuyout(committed).value, 5.0);
    QCOMPARE(notices.count(), 0);
}

// The currency box shows and reads a currency by its tag, for every row of
// the table, the ones added after the original 19 included.
void BuyoutPanelTest::currencyBoxSelectsByTag()
{
    Panel p;
    Buyout annul = makeChaosBuyout(2.0);
    annul.currency = Currency::FromTag("annul");
    QVERIFY(!annul.currency.IsNone());
    p.prices["item-d"] = annul;
    p.panel.showTargets(items({"item-d"}));
    QCOMPARE(p.currency->currentData().toString(), QString("annul"));
    QCOMPARE(p.currency->currentText(), QString("Orb of Annulment"));

    QSignalSpy committed(&p.panel, &BuyoutPanel::committed);
    const int gift = p.currency->findData(QString("offer-gift"));
    QVERIFY(gift > 0);
    p.currency->setCurrentIndex(gift);
    emit p.currency->activated(gift);
    QCOMPARE(committed.count(), 1);
    QCOMPARE(committedBuyout(committed).currency.AsTag(), QString("offer-gift"));
    QCOMPARE(committedBuyout(committed).value, 2.0);
}

QTEST_MAIN(BuyoutPanelTest)

#include "tst_buyoutpanel.moc"
