// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest/QtTest>

#include "currencymanager.h"
#include "datastore/sqlitedatastore.h"
#include "itemsmanager.h"
#include "testfixtures.h"
#include "ui/currencydialog.h"

class CurrencyManagerTest : public QObject
{
    Q_OBJECT

private slots:
    // #285: the currency dialog and its history.
    void firstRunCountsWisdom();
    void ratioEditIsSavedAtOnce();
    void historyKeepsTwoUpdatesInOneSecond();
    void dialogOwnsItsRowsAndSkipsNone();
    void failedHistoryInsertKeepsLastValue();
    void hiddenColumnsAreOwnedByTheDialog();
    void typedRatioIsSavedWhenFinished();
};

namespace {

    // An ItemsManager over the buyout fixture, whose DataStore is a fresh
    // SqliteDataStore in a temporary directory: the first-run state.
    struct CurrencyFixture
    {
        CurrencyFixture()
            : settings(buyouts.tempDir.filePath("settings.ini"), QSettings::IniFormat)
            , items(settings, *buyouts.manager)
        {}

        BuyoutManagerFixture buyouts;
        QSettings settings;
        ItemsManager items;
    };

    // A stack of currency as the stash API sends it: the count is in the
    // "Stack Size" property, written "<count>/<max>".
    std::shared_ptr<Item> makeStack(const QString &id,
                                    const QString &type_line,
                                    int count,
                                    int max,
                                    const ItemLocation &location)
    {
        const QByteArray json = QString(R"({
            "verified": false,
            "w": 1,
            "h": 1,
            "icon": "https://web.poecdn.com/image/test.png",
            "stackSize": %3,
            "maxStackSize": %4,
            "id": "%1",
            "name": "",
            "typeLine": "%2",
            "baseType": "%2",
            "identified": true,
            "ilvl": 0,
            "properties": [
                {"name": "Stack Size", "values": [["%3/%4", 0]], "displayMode": 0}
            ],
            "frameType": 5,
            "x": 0,
            "y": 0
        })")
                                    .arg(id, type_line)
                                    .arg(count)
                                    .arg(max)
                                    .toUtf8();
        return std::make_shared<Item>(makeTestItem(json.constData(), location));
    }

} // namespace

// #285: on the very first run the wisdom total was 0 for the whole session,
// because only the second and later runs sized the per-currency counts.
void CurrencyManagerTest::firstRunCountsWisdom()
{
    CurrencyFixture fixture;
    QVERIFY(fixture.buyouts.data->Get("currency_items").isEmpty());
    const ItemLocation tab = makeTestStashLocation("stash-285a", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("wisdom-285a", "Scroll of Wisdom", 10, 40, tab)},
                                   {tab},
                                   false);

    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    currency.Update();

    QCOMPARE(currency.TotalWisdomValue(), 10);
}

// #285: a ratio typed into the currency dialog is saved when it is edited,
// not only when the application shuts down cleanly.
void CurrencyManagerTest::ratioEditIsSavedAtOnce()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    CurrencyDialog dialog(fixture.settings, currency);

    // The ratio boxes are the editable spin boxes; the value boxes are
    // read-only.
    QDoubleSpinBox *ratio = nullptr;
    for (auto *box : dialog.findChildren<QDoubleSpinBox *>()) {
        if (box->isEnabled()) {
            ratio = box;
            break;
        }
    }
    QVERIFY(ratio);
    ratio->setValue(42.5);

    QVERIFY(fixture.buyouts.data->Get("currency_items").contains("42.5"));
}

// #285: the history is keyed by a timestamp in seconds, so a second update
// in the same second was refused; the latest value now wins.
void CurrencyManagerTest::historyKeepsTwoUpdatesInOneSecond()
{
    BuyoutManagerFixture fixture;
    fixture.data->InsertCurrencyUpdate(CurrencyUpdate{.timestamp = 1000, .value = "1;2"});
    fixture.data->InsertCurrencyUpdate(CurrencyUpdate{.timestamp = 1000, .value = "3;4"});

    const auto history = fixture.data->GetAllCurrency();
    QCOMPARE(history.size(), size_t(1));
    QCOMPARE(history.front().value, QString("3;4"));
}

// #285: the dialog created a row for the "no currency" entry and never used
// it, and no row had an owner, so none was ever deleted.
void CurrencyManagerTest::dialogOwnsItsRowsAndSkipsNone()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    CurrencyDialog dialog(fixture.settings, currency);

    const auto rows = dialog.findChildren<CurrencyWidget *>();
    QCOMPARE(rows.size(), qsizetype(currency.currencies().size()) - 1);
    for (const CurrencyWidget *row : rows) {
        QVERIFY(!row->name->text().isEmpty());
    }
}

// #285: when the history insert failed, the last recorded value still
// advanced, so the update was never written: the next one compared equal
// and was skipped too. The value now advances only with its history row.
void CurrencyManagerTest::failedHistoryInsertKeepsLastValue()
{
    CurrencyFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-285e", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("chaos-285e", "Chaos Orb", 3, 20, tab)}, {tab}, false);
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    const QString before = fixture.buyouts.data->Get("currency_last_value");

    // A second connection to the same file blocks the insert.
    const QString connection = "currency-285e";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(fixture.buyouts.tempDir.filePath("acquisition.sqlite"));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("CREATE TRIGGER block_currency BEFORE INSERT ON currency "
                       "BEGIN SELECT RAISE(FAIL, 'test: insert blocked'); END"));
    }
    currency.Update();
    QCOMPARE(fixture.buyouts.data->Get("currency_last_value"), before);
    QVERIFY(fixture.buyouts.data->GetAllCurrency().empty());

    {
        QSqlDatabase db = QSqlDatabase::database(connection);
        QSqlQuery q(db);
        QVERIFY(q.exec("DROP TRIGGER block_currency"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    currency.Update();
    QVERIFY(fixture.buyouts.data->Get("currency_last_value") != before);
    QCOMPARE(fixture.buyouts.data->GetAllCurrency().size(), size_t(1));
}

// Controls of a column hidden when the dialog is built were never placed in
// its layout, so they had no parent and leaked with their connections.
void CurrencyManagerTest::hiddenColumnsAreOwnedByTheDialog()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    fixture.settings.setValue("show_chaos", false);
    fixture.settings.setValue("show_exalt", false);
    CurrencyDialog dialog(fixture.settings, currency);

    // Four spin boxes a row: the chaos and exalt ratios and values.
    const qsizetype rows = qsizetype(currency.currencies().size()) - 1;
    QCOMPARE(dialog.findChildren<QDoubleSpinBox *>().size(), 4 * rows);
    QStringList labels;
    for (const QLabel *label : dialog.findChildren<QLabel *>()) {
        labels.append(label->text());
    }
    QVERIFY(labels.contains("Amount a chaos Orb can buy"));
    QVERIFY(labels.contains("Total Exalted Orbs"));
}

// A ratio typed into a spin box is saved when the edit is finished, not at
// each keystroke (typing "150" saved 1, 15 and 150).
void CurrencyManagerTest::typedRatioIsSavedWhenFinished()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    fixture.settings.setValue("show_chaos", true);
    CurrencyDialog dialog(fixture.settings, currency);

    // The saved chaos ratio of the first row's currency, read from the
    // stored JSON array by its own key.
    const auto rows = dialog.findChildren<CurrencyWidget *>();
    QVERIFY(!rows.isEmpty());
    QDoubleSpinBox *ratio = rows.front()->chaos_ratio;
    const QString tag = currency.currencies().at(1)->currency.AsTag();
    QCOMPARE(rows.front()->name->text(), currency.currencies().at(1)->name);
    const auto savedRatio = [&fixture, &tag]() -> double {
        const auto saved = QJsonDocument::fromJson(
            fixture.buyouts.data->Get("currency_items").toUtf8());
        for (const auto &entry : saved.array()) {
            if (entry.toObject().value("currency").toString() == tag) {
                return entry.toObject().value("chaos_ratio").toDouble();
            }
        }
        return -1;
    };
    QCOMPARE(savedRatio(), 1.0);

    ratio->clear();
    QTest::keyClicks(ratio, "150");
    QCOMPARE(savedRatio(), 1.0);

    QTest::keyClick(ratio, Qt::Key_Return);
    QCOMPARE(savedRatio(), 150.0);

    // An arrow step is a finished edit of its own and is saved at once.
    QTest::keyClick(ratio, Qt::Key_Up);
    QCOMPARE(ratio->value(), 151.0);
    QCOMPARE(savedRatio(), 151.0);
}

QTEST_MAIN(CurrencyManagerTest)

#include "tst_currencymanager.moc"
