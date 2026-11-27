// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Guillaume DUPUY <glorf@glorf.fr>

#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

#include "currency.h"
#include "datastore/datastore.h"

class QSettings;

class Item;
class ItemsManager;

struct CurrencyRatio
{
    Currency curr1;
    Currency curr2;
    double value1;
    double value2;
    CurrencyRatio()
        : curr1(Currency::CURRENCY_NONE)
        , curr2(Currency::CURRENCY_NONE)
        , value1(0)
        , value2(0)
    {}
    CurrencyRatio(Currency c1, Currency c2, double v1, double v2)
        : curr1(c1)
        , curr2(c2)
        , value1(v1)
        , value2(v2)
    {}
};

struct CurrencyItem
{
    int count;
    Currency currency;
    QString name;
    // What one Divine Orb buys of this currency. Until #38 the anchor was
    // the Exalted Orb.
    CurrencyRatio divine;
    CurrencyRatio chaos;
    CurrencyItem(int co, Currency curr, double chaos_ratio, double divine_ratio)
        : count(co)
        , currency(curr)
        , name(curr.AsString())
        , divine(curr, Currency::CURRENCY_DIVINE_ORB, divine_ratio, 1)
        , chaos(curr, Currency::CURRENCY_CHAOS_ORB, chaos_ratio, 1) {};
};

class CurrencyManager : public QObject
{
    Q_OBJECT
public:
    explicit CurrencyManager(QSettings &settings, DataStore &datastore, ItemsManager &items_manager);
    ~CurrencyManager();
    void ClearCurrency();
    // Called from CurrencyManager::Update.
    void ParseSingleItem(const Item &item);
    const std::vector<std::shared_ptr<CurrencyItem>> &currencies() const { return m_currencies; }
    double TotalDivineValue();
    double TotalChaosValue();
    int TotalWisdomValue();
    void Update();
    void ExportCurrency(const QString &file_name);

signals:
    void Updated();

public slots:
    void SaveCurrencyValue();
    // Saves the ratios, which the currency dialog edits in place (#285).
    void SaveCurrencyItems();

private:
    QSettings &m_settings;
    DataStore &m_data;
    ItemsManager &m_items_manager;

    // One per choosable currency, in table order: the dialog's rows. The
    // history string and its CSV columns are positional over them.
    std::vector<std::shared_ptr<CurrencyItem>> m_currencies;
    // The currencies counted in the wisdom total, from the table.
    struct WisdomCount
    {
        QString name;
        int wisdom;
        int count;
    };
    std::vector<WisdomCount> m_wisdom;
    // The ratios, and what earlier versions stored: the ratios against the
    // Exalted Orb (#38) and the list from before the JSON format, either of
    // which a first run with this version converts. When any could not be
    // read (#295), the defaults stand in for this session and none is
    // written.
    StoredSettings m_stored;
    // A new item per choosable currency, with the default ratios.
    void MakeCurrencyItems();
    // Used only the first time we launch the app
    void FirstInitCurrency();
    //Migrate from old storage (csv-like serializing) to new one (using json)
    void MigrateCurrency(const QString &currency_base);
    void InitCurrency(const QString &currency_items);
    // Reads the ratios stored against the Exalted Orb as Divine ratios (#38).
    void ConvertExaltedRatios(const QString &currency_items);
    // Restates every row's ratio, held against the Exalted Orb, against the
    // Divine Orb. False, with the rows at their defaults, when the rows give
    // no Exalted-to-Divine rate.
    bool RestateAgainstDivine();
    QString Serialize(const std::vector<std::shared_ptr<CurrencyItem>> &currencies);
    void Deserialize(const QString &data, std::vector<std::shared_ptr<CurrencyItem>> &currencies);
    void Save();
};
