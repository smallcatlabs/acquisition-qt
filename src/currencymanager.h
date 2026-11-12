// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Guillaume DUPUY <glorf@glorf.fr>

#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

#include "currency.h"

class QSettings;

class DataStore;
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
    CurrencyRatio exalt;
    CurrencyRatio chaos;
    CurrencyItem(int co, Currency curr, double chaos_ratio, double exalt_ratio)
        : count(co)
        , currency(curr)
        , name(curr.AsString())
        , exalt(curr, Currency::CURRENCY_EXALTED_ORB, exalt_ratio, 1)
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
    double TotalExaltedValue();
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

    // One per row of the currency table, in table order: the history string
    // and its CSV columns are positional.
    std::vector<std::shared_ptr<CurrencyItem>> m_currencies;
    // A new item per table row, with the default ratios.
    void MakeCurrencyItems();
    // Used only the first time we launch the app
    void FirstInitCurrency();
    //Migrate from old storage (csv-like serializing) to new one (using json)
    void MigrateCurrency();
    void InitCurrency();
    QString Serialize(const std::vector<std::shared_ptr<CurrencyItem>> &currencies);
    void Deserialize(const QString &data, std::vector<std::shared_ptr<CurrencyItem>> &currencies);
    void Save();
};
