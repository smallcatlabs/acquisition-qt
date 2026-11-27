// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTest>

#include "currencymanager.h"
#include "datastore/sqlitedatastore.h"
#include "itemsmanager.h"
#include "testfixtures.h"
#include "testsuite.h"
#include "ui/currencydialog.h"

class CurrencyManagerTest : public QObject
{
    Q_OBJECT

private slots:
    // #285: the currency dialog and its history.
    void firstRunCountsWisdom();
    void ratioEditIsSavedAtOnce();
    void historyKeepsTwoUpdatesInOneSecond();
    void dialogOwnsOneRowPerCurrency();
    void failedHistoryInsertKeepsLastValue();
    void hiddenColumnsAreOwnedByTheDialog();
    void typedRatioIsSavedWhenFinished();
    void historyOfExistingCurrenciesIsUnchanged();
    void legacyRatioListSkipsNoCurrency();
    void savedRatiosFromBeforeTheTableLoad();

    // #295: a failed read of the stored currency is not an empty store.
    void failedReadLeavesTheStoredCurrencyAlone();
    void ratiosFromBeforeVersion016AreReplacedOnce();
    void failedReadOfTheOldListWritesNothing();
    void failedReadRecordsNoHistory();
    void failedReadOfTheLastValueRecordsNoDuplicate();

    // #38: the dialog's second anchor is the Divine Orb.
    void dialogIsAnchoredOnDivine();
    void exaltedRatiosBecomeDivineRatios();
    void exaltedRatiosWithNoDivineRateAreNotConverted();
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

    // The fixture's real store, except that a read of one key fails: one
    // table holds every key, so the real store can only fail them together.
    class OneKeyFails : public DataStore
    {
    public:
        OneKeyFails(DataStore &store, const QString &key)
            : m_store(store)
            , m_key(key)
        {}
        bool Set(const QString &key, const QString &value) override
        {
            return m_store.Set(key, value);
        }
        ReadResult Read(const QString &key) override
        {
            if (key == m_key) {
                return std::unexpected(QString("test: the read of %1 fails").arg(key));
            }
            const StoredSettings read(m_store, {key});
            if (!read.Loaded()) {
                return std::unexpected(read.error());
            }
            return read.value(key);
        }
        bool InsertCurrencyUpdate(const CurrencyUpdate &update) override
        {
            return m_store.InsertCurrencyUpdate(update);
        }
        std::vector<CurrencyUpdate> GetAllCurrency() override { return m_store.GetAllCurrency(); }

    private:
        DataStore &m_store;
        QString m_key;
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
    QVERIFY(fixture.buyouts.data->Get("currency_items_divine").isEmpty());
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

    QVERIFY(fixture.buyouts.data->Get("currency_items_divine").contains("42.5"));
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
void CurrencyManagerTest::dialogOwnsOneRowPerCurrency()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    CurrencyDialog dialog(fixture.settings, currency);

    const auto rows = dialog.findChildren<CurrencyWidget *>();
    // The currencies the dialog has always covered: the 19 a price can be
    // set in, not those only read from notes.
    QCOMPARE(rows.size(), qsizetype(19));
    QCOMPARE(currency.currencies().size(), size_t(19));
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
    fixture.settings.setValue("show_divine", false);
    CurrencyDialog dialog(fixture.settings, currency);

    // Four spin boxes a row: the chaos and divine ratios and values.
    const qsizetype rows = qsizetype(currency.currencies().size());
    QCOMPARE(dialog.findChildren<QDoubleSpinBox *>().size(), 4 * rows);
    QStringList labels;
    for (const QLabel *label : dialog.findChildren<QLabel *>()) {
        labels.append(label->text());
    }
    QVERIFY(labels.contains("Amount a chaos Orb can buy"));
    QVERIFY(labels.contains("Total Divine Orbs"));
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
    const QString tag = currency.currencies().at(0)->currency.AsTag();
    QCOMPARE(rows.front()->name->text(), currency.currencies().at(0)->name);
    const auto savedRatio = [&fixture, &tag]() -> double {
        const auto saved = QJsonDocument::fromJson(
            fixture.buyouts.data->Get("currency_items_divine").toUtf8());
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

// The history string is positional: the total, then one count per currency
// the dialog covers. It is the same string as before the currency table.
// The literal was recorded by running this test, then asserting only that
// the string began with it and that any further fields were 0, against
// the enum-based code (b34d4531's currencymanager.cpp and currency.cpp),
// which wrote exactly these 20 fields: the exalted total with every ratio
// at its default of 1 (3 + 2 + 7), then the counts in the old enum order.
// Since #38 the total is in Divine Orbs: the same number at these ratios.
void CurrencyManagerTest::historyOfExistingCurrenciesIsUnchanged()
{
    CurrencyFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-hist", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("hist-chaos", "Chaos Orb", 3, 20, tab),
                                    makeStack("hist-divine", "Divine Orb", 2, 10, tab),
                                    makeStack("hist-silver", "Silver Coin", 7, 30, tab),
                                    makeStack("hist-portal", "Portal Scroll", 40, 40, tab)},
                                   {tab},
                                   false);
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    currency.Update();

    QCOMPARE(fixture.buyouts.data->Get("currency_last_value"),
             QString("12;0;0;0;3;0;0;0;0;0;0;0;0;0;0;2;0;0;0;7"));
    // Portal scrolls count toward the wisdom total, as before, not the
    // history.
    QCOMPARE(currency.TotalWisdomValue(), 40);
}

// The pre-JSON ratio list ("currency_base") is positional over the old
// enum, whose entry 0 was "no currency": entry i is the dialog's row i - 1.
void CurrencyManagerTest::legacyRatioListSkipsNoCurrency()
{
    CurrencyFixture fixture;
    fixture.buyouts.data->Set("currency_base", "0;5;7");
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);

    QCOMPARE(currency.currencies().at(0)->currency, Currency::CURRENCY_ORB_OF_ALTERATION);
    QCOMPARE(currency.currencies().at(0)->divine.value1, 5.0);
    QCOMPARE(currency.currencies().at(1)->currency, Currency::CURRENCY_ORB_OF_FUSING);
    QCOMPARE(currency.currencies().at(1)->divine.value1, 7.0);
}

// Saved ratios as b34d4531 wrote them: one entry per old enum value,
// "no currency" first with an empty tag. They load by tag, and the ones
// against the Exalted Orb, which then bought 0.8 Divine Orbs, are restated
// against the Divine Orb (#38).
void CurrencyManagerTest::savedRatiosFromBeforeTheTableLoad()
{
    CurrencyFixture fixture;
    QString saved = R"([{"currency":"","count":0,"chaos_ratio":1,"exalt_ratio":1})";
    for (const char *tag : {"alt",
                            "fuse",
                            "alch",
                            "chaos",
                            "gcp",
                            "exa",
                            "chrom",
                            "jew",
                            "chance",
                            "chisel",
                            "scour",
                            "blessed",
                            "regret",
                            "regal",
                            "divine",
                            "vaal",
                            "coin",
                            "mirror",
                            "silver"}) {
        const bool alt = QString(tag) == "alt";
        const bool divine = QString(tag) == "divine";
        saved += QString(R"(,{"currency":"%1","count":0,"chaos_ratio":%2,"exalt_ratio":%3})")
                     .arg(tag)
                     .arg(alt ? 12.5 : 1)
                     .arg(divine ? 0.8 : 1);
    }
    saved += "]";
    fixture.buyouts.data->Set("currency_items", saved);

    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);

    QCOMPARE(currency.currencies().size(), size_t(19));
    for (const auto &row : currency.currencies()) {
        const bool alt = row->currency == Currency::CURRENCY_ORB_OF_ALTERATION;
        const bool divine = row->currency == Currency::CURRENCY_DIVINE_ORB;
        QCOMPARE(row->chaos.value1, alt ? 12.5 : 1.0);
        QCOMPARE(row->divine.value1, divine ? 1.0 : 1.25);
    }
}

// #295: a read of the stored currency that fails is not taken for an empty
// store. Before, the manager set up the defaults and its destructor saved
// them over the user's ratios.
void CurrencyManagerTest::failedReadLeavesTheStoredCurrencyAlone()
{
    CurrencyFixture fixture;
    DataStore &store = *fixture.buyouts.data;
    {
        // The user's ratios, saved when an earlier session ends.
        CurrencyManager earlier(fixture.settings, store, fixture.items);
        earlier.currencies().at(0)->divine.value1 = 12.5;
    }
    const QString stored = store.Get("currency_items_divine");
    QVERIFY(stored.contains("12.5"));

    // A second connection hides the table, so reading the store fails.
    const QString connection = "currency-295";
    const auto rename = [&](const char *from, const char *to) {
        QSqlDatabase db = QSqlDatabase::database(connection);
        QSqlQuery q(db);
        QVERIFY2(q.exec(QString("ALTER TABLE %1 RENAME TO %2").arg(from, to)),
                 qPrintable(q.lastError().text()));
    };
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(fixture.buyouts.tempDir.filePath("acquisition.sqlite"));
        QVERIFY(db.open());
    }
    rename("data", "data_hidden");
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
        // The table is back before the manager is destroyed, so a write
        // there would succeed.
        rename("data_hidden", "data");
    }
    QSqlDatabase::database(connection).close();
    QSqlDatabase::removeDatabase(connection);

    QCOMPARE(store.Get("currency_items_divine"), stored);
}

// #295: on a first run (no currency_items), a failed read of the list from
// before the JSON format is not taken for no list: nothing is written, so
// the old list is still there to migrate next time.
void CurrencyManagerTest::failedReadOfTheOldListWritesNothing()
{
    CurrencyFixture fixture;
    DataStore &real = *fixture.buyouts.data;
    QVERIFY(real.Set("currency_base", "0;5;7"));
    OneKeyFails store(real, "currency_base");
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
    }
    QCOMPARE(real.Get("currency_items", "absent"), QString("absent"));
    QCOMPARE(real.Get("currency_items_divine", "absent"), QString("absent"));
    QCOMPARE(real.Get("currency_base"), QString("0;5;7"));
}

// #295: after a failed read no history row is written either: its total
// would be computed from the default ratios.
void CurrencyManagerTest::failedReadRecordsNoHistory()
{
    CurrencyFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-295h", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("chaos-295h", "Chaos Orb", 3, 20, tab)}, {tab}, false);
    QVERIFY(fixture.buyouts.data->Set("currency_items_divine", "[]"));
    OneKeyFails store(*fixture.buyouts.data, "currency_items_divine");
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
        currency.Update();
    }
    QVERIFY(fixture.buyouts.data->GetAllCurrency().empty());
    QCOMPARE(fixture.buyouts.data->Get("currency_items_divine"), QString("[]"));
}

// #295: when the last recorded value cannot be read, the update is not taken
// for a change from nothing: no row is recorded that may repeat the last.
void CurrencyManagerTest::failedReadOfTheLastValueRecordsNoDuplicate()
{
    CurrencyFixture fixture;
    const ItemLocation tab = makeTestStashLocation("stash-295l", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("chaos-295l", "Chaos Orb", 3, 20, tab)}, {tab}, false);
    {
        CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
        currency.Update();
    }
    QCOMPARE(fixture.buyouts.data->GetAllCurrency().size(), size_t(1));
    // Rows are keyed by the second (#285): move the first one back, so a
    // repeat within this second is a second row, not a replacement.
    const QString connection = "currency-295l";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(fixture.buyouts.tempDir.filePath("acquisition.sqlite"));
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec("UPDATE currency SET timestamp = 1"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);

    OneKeyFails store(*fixture.buyouts.data, "currency_last_value");
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
        currency.Update();
    }
    QCOMPARE(fixture.buyouts.data->GetAllCurrency().size(), size_t(1));
}

// #295 review: from v0.9 to v0.15 currency_items was a JSON object keyed by
// the currency's name (v0.15.2's Serialize); since v0.16 it is read as an
// array, so a league file kept from then cannot be parsed. Its ratios are
// replaced by the defaults, and from then on edits and history are saved.
// Since #38 they are saved under their own key, and the old one is left.
void CurrencyManagerTest::ratiosFromBeforeVersion016AreReplacedOnce()
{
    CurrencyFixture fixture;
    DataStore &store = *fixture.buyouts.data;
    const QString v015 = R"({"Orb of Alteration":{"count":0,"chaos_ratio":1.0,)"
                         R"("exalt_ratio":12.5,"currency":"alt"},"Chaos Orb":{"count":3,)"
                         R"("chaos_ratio":1.0,"exalt_ratio":1.0,"currency":"chaos"}})";
    QVERIFY(store.Set("currency_items", v015));
    const ItemLocation tab = makeTestStashLocation("stash-295v", "Currency", 0);
    fixture.items.OnItemsRefreshed({makeStack("chaos-295v", "Chaos Orb", 3, 20, tab)}, {tab}, false);
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
        currency.currencies().at(0)->divine.value1 = 7;
        currency.SaveCurrencyItems();
        currency.Update();
    }
    const QString saved = store.Get("currency_items_divine");
    QVERIFY2(saved.startsWith("[") && saved.contains("\"divine_ratio\":7"), qPrintable(saved));
    QCOMPARE(store.Get("currency_items"), v015);
    QCOMPARE(store.GetAllCurrency().size(), size_t(1));
}

namespace {

    // currency_items as the versions before the Divine anchor wrote it: one
    // entry per currency the dialog covers, each with what one Exalted Orb
    // buys. `exalted` gives the ratios that are not the default of 1.
    QString exaltedRatios(const QHash<QString, double> &exalted)
    {
        QStringList entries;
        for (const auto &row : CurrencyTable::Default().rows()) {
            if (!row.choosable) {
                continue;
            }
            entries.append(QString(R"({"currency":"%1","count":0,"chaos_ratio":1,"exalt_ratio":%2})")
                               .arg(row.tag)
                               .arg(exalted.value(row.tag, 1.0)));
        }
        return "[" + entries.join(",") + "]";
    }

    // The Divine ratio saved for a currency, -1 when there is none.
    double savedDivineRatio(DataStore &store, const QString &tag)
    {
        const auto saved = QJsonDocument::fromJson(store.Get("currency_items_divine").toUtf8());
        for (const auto &entry : saved.array()) {
            const QJsonObject object = entry.toObject();
            if ((object.value("currency").toString() == tag) && object.contains("divine_ratio")) {
                return object.value("divine_ratio").toDouble();
            }
        }
        return -1;
    }

} // namespace

// #38: a Divine Orb has been worth more than an Exalted Orb since the two
// swapped roles, so the dialog's ratios and its total are in Divine Orbs.
void CurrencyManagerTest::dialogIsAnchoredOnDivine()
{
    CurrencyFixture fixture;
    CurrencyManager currency(fixture.settings, *fixture.buyouts.data, fixture.items);
    CurrencyDialog dialog(fixture.settings, currency);

    QStringList labels;
    for (const QLabel *label : dialog.findChildren<QLabel *>()) {
        labels.append(label->text());
    }
    QVERIFY(labels.contains("Amount a Divine Orb can buy"));
    QVERIFY(labels.contains("Value in Divine Orb"));
    QVERIFY(labels.contains("Total Divine Orbs"));
    // Only the Exalted Orb's own row still names it.
    QCOMPARE(labels.filter("Exalted"), QStringList{"Exalted Orb"});
    QStringList boxes;
    for (const QCheckBox *box : dialog.findChildren<QCheckBox *>()) {
        boxes.append(box->text());
    }
    QVERIFY2(boxes.contains("show divine ratio"), qPrintable(boxes.join(" | ")));
}

// #38: the ratios a user entered against the Exalted Orb are restated
// against the Divine Orb, through their own Exalted-to-Divine rate. What
// was stored stays where it was, for an older version to read.
void CurrencyManagerTest::exaltedRatiosBecomeDivineRatios()
{
    CurrencyFixture fixture;
    DataStore &store = *fixture.buyouts.data;
    // One Exalted Orb buys a tenth of a Divine Orb, 15 Chaos Orbs and 60
    // Orbs of Alteration; Perandus Coins are not counted.
    const QString stored = exaltedRatios({{"divine", 0.1}, {"chaos", 15}, {"alt", 60}, {"coin", 0}});
    QVERIFY(store.Set("currency_items", stored));
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
    }

    // So one Divine Orb buys ten times that.
    QCOMPARE(savedDivineRatio(store, "divine"), 1.0);
    QCOMPARE(savedDivineRatio(store, "exa"), 10.0);
    QCOMPARE(savedDivineRatio(store, "chaos"), 150.0);
    QCOMPARE(savedDivineRatio(store, "alt"), 600.0);
    QCOMPARE(savedDivineRatio(store, "coin"), 0.0);
    QCOMPARE(store.Get("currency_items"), stored);

    // It is done once: a later session reads the Divine ratios, edited or
    // not, and does not convert again.
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
    }
    QCOMPARE(savedDivineRatio(store, "chaos"), 150.0);
}

// #38: with no Exalted-to-Divine rate (the Divine Orb was not counted), the
// Exalted ratios say nothing about a Divine Orb: the Divine ratios start at
// their defaults, and the Exalted ones stay stored.
void CurrencyManagerTest::exaltedRatiosWithNoDivineRateAreNotConverted()
{
    CurrencyFixture fixture;
    DataStore &store = *fixture.buyouts.data;
    const QString stored = exaltedRatios({{"divine", 0}, {"chaos", 15}});
    QVERIFY(store.Set("currency_items", stored));
    {
        CurrencyManager currency(fixture.settings, store, fixture.items);
    }

    QCOMPARE(savedDivineRatio(store, "divine"), 1.0);
    QCOMPARE(savedDivineRatio(store, "chaos"), 1.0);
    QCOMPARE(store.Get("currency_items"), stored);
}

ACQ_TEST_MAIN(CurrencyManagerTest)

#include "tst_currencymanager.moc"
