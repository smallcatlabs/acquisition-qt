#include <QDoubleSpinBox>
#include <QLabel>
#include <QSettings>
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

QTEST_MAIN(CurrencyManagerTest)

#include "tst_currencymanager.moc"
