// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Guillaume DUPUY <glorf@glorf.fr>

#include "currencymanager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTextStream>

#include <cmath>

#include "datastore/datastore.h"
#include "item.h"
#include "itemsmanager.h"
#include "util/glaze_qt.h"  // IWYU pragma: keep
#include "util/spdlog_qt.h" // IWYU pragma: keep

struct SerializedCurrency
{
    QString currency;
    int count;
    double chaos_ratio;
    double exalt_ratio;
};

CurrencyManager::CurrencyManager(QSettings &settings,
                                 DataStore &datastore,
                                 ItemsManager &items_manager)
    : QObject()
    , m_settings(settings)
    , m_data(datastore)
    , m_items_manager(items_manager)
{
    if (m_data.Get("currency_items", "").isEmpty()) {
        FirstInitCurrency();
        if (!m_data.Get("currency_base", "").isEmpty()) {
            MigrateCurrency();
            spdlog::warn("Found old currency values, migrated them to the new system");
        }
    } else {
        InitCurrency();
    }
}

CurrencyManager::~CurrencyManager()
{
    Save();
}

void CurrencyManager::Save()
{
    SaveCurrencyItems();
    SaveCurrencyValue();
}

void CurrencyManager::Update()
{
    ClearCurrency();
    for (auto &item : m_items_manager.items()) {
        ParseSingleItem(*item);
    }
    SaveCurrencyValue();
    emit Updated();
}

const double EPS = 1e-6;
double CurrencyManager::TotalExaltedValue()
{
    double out = 0;
    for (const auto &currency : m_currencies) {
        if (fabs(currency->exalt.value1) > EPS) {
            out += currency->count / currency->exalt.value1;
        }
    }
    return out;
}

double CurrencyManager::TotalChaosValue()
{
    double out = 0;
    for (const auto &currency : m_currencies) {
        if (fabs(currency->chaos.value1) > EPS) {
            out += currency->count / currency->chaos.value1;
        }
    }
    return out;
}

int CurrencyManager::TotalWisdomValue()
{
    int out = 0;
    for (const auto &currency : m_currencies) {
        if (const CurrencyInfo *info = currency->currency.Info()) {
            out += currency->count * info->wisdom;
        }
    }
    return out;
}

void CurrencyManager::ClearCurrency()
{
    for (auto &currency : m_currencies) {
        currency->count = 0;
    }
}

void CurrencyManager::MakeCurrencyItems()
{
    // The currencies the application has always had start at a ratio of 1,
    // as they always did. Those added later start at 0, which the totals
    // skip, so they count only once the user gives them a ratio.
    constexpr size_t kOriginalCurrencies = Currency::CURRENCY_SILVER_COIN;
    const auto &table = CurrencyTable::Default();
    m_currencies.clear();
    m_currencies.reserve(table.size());
    for (size_t row = 0; row < table.size(); ++row) {
        const double ratio = (row < kOriginalCurrencies) ? 1 : 0;
        m_currencies.push_back(
            std::make_shared<CurrencyItem>(0, Currency::AtRow(row), ratio, ratio));
    }
}

void CurrencyManager::InitCurrency()
{
    MakeCurrencyItems();
    Deserialize(m_data.Get("currency_items"), m_currencies);
}

void CurrencyManager::FirstInitCurrency()
{
    MakeCurrencyItems();
    // A placeholder last value: the total, then a count per currency.
    QString value = "0";
    for (size_t row = 0; row < m_currencies.size(); ++row) {
        value += ";0";
    }
    m_data.Set("currency_items", Serialize(m_currencies));
    m_data.Set("currency_last_value", value);
    m_settings.setValue("show_chaos", true);
    m_settings.setValue("show_exalt", true);
}

void CurrencyManager::MigrateCurrency()
{
    // The old list is positional over the old currency enum, whose first
    // entry was "no currency": entry i is table row i - 1.
    const QStringList list = m_data.Get("currency_base").split(';');
    for (qsizetype i = 1; (i < list.size()) && (static_cast<size_t>(i - 1) < m_currencies.size());
         ++i) {
        // We can't use the toDouble function from QT, because it encodes a double with a ","
        // Might be related to localisation issue, but anyway it's safer this way
        m_currencies[static_cast<size_t>(i - 1)]->exalt.value1 = list[i].toDouble();
    }
    //Set to empty so we won't trigger it next time !
    m_data.Set("currency_base", "");
}

QString CurrencyManager::Serialize(const std::vector<std::shared_ptr<CurrencyItem>> &currencies)
{
    std::vector<SerializedCurrency> output;
    output.reserve(currencies.size());
    for (const auto &currency : currencies) {
        SerializedCurrency c{.currency = currency->currency.AsTag(),
                             .count = currency->count,
                             .chaos_ratio = currency->chaos.value1,
                             .exalt_ratio = currency->exalt.value1};
        output.push_back(c);
    }

    const auto result = glz::write_json(output);
    if (!result) {
        const auto msg = glz::format_error(result.error());
        spdlog::error("Error serializing currency: {}", msg);
        return QString();
    }
    return QString::fromStdString(*result);
}

void CurrencyManager::Deserialize(const QString &string_data,
                                  std::vector<std::shared_ptr<CurrencyItem>> &currencies)
{
    // Maybe clear something would be good
    if (string_data.isEmpty()) {
        return;
    }

    const auto bytes = string_data.toUtf8();
    const std::string_view sv{bytes.constData(), size_t(bytes.size())};
    const auto result = glz::read_json<std::vector<SerializedCurrency>>(sv);
    if (!result) {
        const auto msg = glz::format_error(result.error(), sv);
        spdlog::error("Error deserializing currency: {}", msg);
        return;
    }

    for (const auto &obj : *result) {
        Currency curr = Currency::FromTag(obj.currency);
        for (auto &item : currencies) {
            if (curr == item->currency) {
                item = std::make_shared<CurrencyItem>(obj.count,
                                                      curr,
                                                      obj.chaos_ratio,
                                                      obj.exalt_ratio);
                break;
            }
        }
    }
}

void CurrencyManager::SaveCurrencyItems()
{
    m_data.Set("currency_items", Serialize(m_currencies));
}

void CurrencyManager::SaveCurrencyValue()
{
    QString value = "";
    // Useless to save if every count is 0.
    bool empty = true;
    value = QString::number(TotalExaltedValue());
    for (auto &currency : m_currencies) {
        value += ";" + QString::number(currency->count);
        if (currency->count != 0) {
            empty = false;
        }
    }
    QString old_value = m_data.Get("currency_last_value", "");
    if (value != old_value && !empty) {
        CurrencyUpdate update = CurrencyUpdate();
        update.timestamp = QDateTime::currentSecsSinceEpoch();
        update.value = value;
        // The last value advances only with its history row (#285), so an
        // update that was not written is tried again next time.
        if (m_data.InsertCurrencyUpdate(update)) {
            m_data.Set("currency_last_value", value);
        }
    }
}

void CurrencyManager::ExportCurrency(const QString &file_name)
{
    if (file_name.isEmpty()) {
        return;
    }
    QString header_csv = "Date,Total value";
    for (const auto &item : m_currencies) {
        header_csv += "," + item->currency.AsString();
    }
    std::vector<CurrencyUpdate> result = m_data.GetAllCurrency();
    QFile file(QDir::toNativeSeparators(file_name));
    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        spdlog::warn("CurrencyManager::ExportCurrency : couldn't open CSV export file ");
        return;
    }
    QTextStream out(&file);
    out << header_csv << "\n";
    for (auto &update : result) {
        const QDateTime timestamp = QDateTime::fromSecsSinceEpoch(update.timestamp).toLocalTime();
        const QString value = update.value;
        out << timestamp.toString("yyyy-MM-dd hh:mm") << ",";
        out << value.split(";").join(",") << "\n";
    }
}

void CurrencyManager::ParseSingleItem(const Item &item)
{
    for (auto &currency : m_currencies) {
        if (item.PrettyName() == currency->name) {
            currency->count += item.count();
        }
    }
}
