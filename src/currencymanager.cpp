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
#include "util/json_glaze.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    // Where the ratios are stored. The key before #38, "currency_items",
    // holds ratios against the Exalted Orb; it is read once, to convert
    // them, and left as it is: a version from before #38 still reads it,
    // and would fail on an entry with a field it does not know.
    const QString kItemsKey = "currency_items_divine";
    const QString kExaltedItemsKey = "currency_items";

} // namespace

struct SerializedCurrency
{
    QString currency;
    int count;
    double chaos_ratio;
    double divine_ratio;
};

// An entry of "currency_items".
struct ExaltedCurrency
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
    , m_stored(datastore, {kItemsKey, kExaltedItemsKey, "currency_base"})
{
    for (const CurrencyInfo &row : CurrencyTable::Default().rows()) {
        if (row.wisdom > 0) {
            m_wisdom.push_back(WisdomCount{.name = row.name, .wisdom = row.wisdom, .count = 0});
        }
    }
    // A read that fails is not an empty store (#295): set up and saved as
    // one, the defaults would replace the user's ratios.
    if (!m_stored.Loaded()) {
        spdlog::error("CurrencyManager: the stored currency could not be read, so it will not be "
                      "saved this session: {}",
                      m_stored.error());
        MakeCurrencyItems();
    } else if (!m_stored.value(kItemsKey).isEmpty()) {
        InitCurrency(m_stored.value(kItemsKey));
    } else if (!m_stored.value(kExaltedItemsKey).isEmpty()) {
        ConvertExaltedRatios(m_stored.value(kExaltedItemsKey));
    } else {
        FirstInitCurrency();
        const QString base = m_stored.value("currency_base");
        if (!base.isEmpty()) {
            MigrateCurrency(base);
            spdlog::warn("Found old currency values, migrated them to the new system");
        }
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
double CurrencyManager::TotalDivineValue()
{
    double out = 0;
    for (const auto &currency : m_currencies) {
        if (fabs(currency->divine.value1) > EPS) {
            out += currency->count / currency->divine.value1;
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
    for (const WisdomCount &row : m_wisdom) {
        out += row.count * row.wisdom;
    }
    return out;
}

void CurrencyManager::ClearCurrency()
{
    for (auto &currency : m_currencies) {
        currency->count = 0;
    }
    for (WisdomCount &row : m_wisdom) {
        row.count = 0;
    }
}

void CurrencyManager::MakeCurrencyItems()
{
    const auto &table = CurrencyTable::Default();
    m_currencies.clear();
    for (size_t row = 0; row < table.size(); ++row) {
        if (table.at(row).choosable) {
            m_currencies.push_back(std::make_shared<CurrencyItem>(0, Currency::AtRow(row), 1, 1));
        }
    }
}

void CurrencyManager::InitCurrency(const QString &currency_items)
{
    MakeCurrencyItems();
    Deserialize(currency_items, m_currencies);
}

void CurrencyManager::FirstInitCurrency()
{
    MakeCurrencyItems();
    // A placeholder last value: the total, then a count per currency.
    QString value = "0";
    for (size_t row = 0; row < m_currencies.size(); ++row) {
        value += ";0";
    }
    m_stored.Set(kItemsKey, Serialize(m_currencies));
    m_data.Set("currency_last_value", value);
    m_settings.setValue("show_chaos", true);
    m_settings.setValue("show_divine", true);
}

void CurrencyManager::MigrateCurrency(const QString &currency_base)
{
    // The old list is positional over the old currency enum, whose first
    // entry was "no currency": entry i is table row i - 1.
    const QStringList list = currency_base.split(';');
    for (qsizetype i = 1; (i < list.size()) && (static_cast<size_t>(i - 1) < m_currencies.size());
         ++i) {
        // We can't use the toDouble function from QT, because it encodes a double with a ","
        // Might be related to localisation issue, but anyway it's safer this way
        m_currencies[static_cast<size_t>(i - 1)]->divine.value1 = list[i].toDouble();
    }
    // The old list is against the Exalted Orb too.
    if (!RestateAgainstDivine()) {
        spdlog::warn("The old currency values give no rate between the Exalted Orb and the "
                     "Divine Orb, so the Divine ratios start at their defaults");
    }
    m_stored.Set(kItemsKey, Serialize(m_currencies));
    //Set to empty so we won't trigger it next time !
    m_stored.Set("currency_base", "");
}

QString CurrencyManager::Serialize(const std::vector<std::shared_ptr<CurrencyItem>> &currencies)
{
    std::vector<SerializedCurrency> output;
    output.reserve(currencies.size());
    for (const auto &currency : currencies) {
        SerializedCurrency c{.currency = currency->currency.AsTag(),
                             .count = currency->count,
                             .chaos_ratio = currency->chaos.value1,
                             .divine_ratio = currency->divine.value1};
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
                                                      obj.divine_ratio);
                break;
            }
        }
    }
}

void CurrencyManager::ConvertExaltedRatios(const QString &currency_items)
{
    MakeCurrencyItems();
    const auto bytes = currency_items.toUtf8();
    const std::string_view sv{bytes.constData(), size_t(bytes.size())};
    const auto result = glz::read_json<std::vector<ExaltedCurrency>>(sv);
    if (!result) {
        // As before #38 for a list that cannot be parsed (one from before
        // version 0.16, say): the defaults replace it.
        const auto msg = glz::format_error(result.error(), sv);
        spdlog::error("Error deserializing currency: {}", msg);
    } else {
        for (const auto &obj : *result) {
            const Currency curr = Currency::FromTag(obj.currency);
            for (auto &item : m_currencies) {
                if (curr == item->currency) {
                    item = std::make_shared<CurrencyItem>(obj.count,
                                                          curr,
                                                          obj.chaos_ratio,
                                                          obj.exalt_ratio);
                    break;
                }
            }
        }
        if (RestateAgainstDivine()) {
            spdlog::info("The currency ratios against the Exalted Orb were restated against the "
                         "Divine Orb");
        } else {
            spdlog::warn("The currency ratios against the Exalted Orb give no rate between it "
                         "and the Divine Orb, so the Divine ratios start at their defaults");
        }
    }
    m_stored.Set(kItemsKey, Serialize(m_currencies));
    // The check box follows the one it replaces.
    if (!m_settings.contains("show_divine") && m_settings.contains("show_exalt")) {
        m_settings.setValue("show_divine", m_settings.value("show_exalt"));
    }
}

bool CurrencyManager::RestateAgainstDivine()
{
    // What one Exalted Orb buys in Divine Orbs.
    double rate = 0;
    for (const auto &currency : m_currencies) {
        if (currency->currency == Currency::CURRENCY_DIVINE_ORB) {
            rate = currency->divine.value1;
        }
    }
    const bool known = fabs(rate) > EPS;
    for (auto &currency : m_currencies) {
        // One Exalted Orb buys `value1` of this currency and `rate` Divine
        // Orbs, so one Divine Orb buys `value1 / rate` of it. A currency
        // that was not counted (0) stays uncounted.
        currency->divine.value1 = known ? currency->divine.value1 / rate : 1;
    }
    return known;
}

void CurrencyManager::SaveCurrencyItems()
{
    m_stored.Set(kItemsKey, Serialize(m_currencies));
}

void CurrencyManager::SaveCurrencyValue()
{
    if (!m_stored.Loaded()) {
        return;
    }
    QString value = "";
    // Useless to save if every count is 0.
    bool empty = true;
    value = QString::number(TotalDivineValue());
    for (auto &currency : m_currencies) {
        value += ";" + QString::number(currency->count);
        if (currency->count != 0) {
            empty = false;
        }
    }
    // A last value that cannot be read is not "none" (#295): taken for
    // none, this update would be recorded again even when nothing changed.
    StoredSettings last(m_data, {"currency_last_value"});
    if (!last.Loaded()) {
        return;
    }
    if (value != last.value("currency_last_value") && !empty) {
        CurrencyUpdate update = CurrencyUpdate();
        update.timestamp = QDateTime::currentSecsSinceEpoch();
        update.value = value;
        // The last value advances only with its history row (#285), so an
        // update that was not written is tried again next time.
        if (m_data.InsertCurrencyUpdate(update)) {
            last.Set("currency_last_value", value);
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
    for (WisdomCount &row : m_wisdom) {
        if (item.PrettyName() == row.name) {
            row.count += item.count();
        }
    }
}
