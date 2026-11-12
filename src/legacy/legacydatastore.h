// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QHashFunctions> // Needed to avoid obscure errors in std::unordered_map with QString keys.
#include <QSqlDatabase>
#include <QString>

#include <unordered_map>
#include <vector>

#include "legacy/legacybuyout.h"
#include "legacy/legacycharacter.h"
#include "legacy/legacyitem.h"
#include "legacy/legacystash.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

struct LegacyDataStore
{
    struct DataTable
    {
        QString db_version;
        QString version;
        std::unordered_map<QString, LegacyBuyout> buyouts;
        std::unordered_map<QString, LegacyBuyout> tab_buyouts;
        // TODO: IGNORE: std::unordered_set<QString> refresh_checked_state;
        // TODO: IGNORE: QString currency_last_value;
        // TODO: IGNORE: LegacyCurrencyMap currency_items;
    };

    struct TabsTable
    {
        std::vector<LegacyStash> stashes;
        std::vector<LegacyCharacter> characters;
    };

    using ItemsTable = std::unordered_map<QString, std::vector<LegacyItem>>;
    // IGNORE: using CurrencyTable = std::unordered_map<unsigned long long, QString>;

    LegacyDataStore(const QString &filename);

    bool isValid() const { return m_valid; }
    qint64 itemCount() const { return m_item_count; }
    qint64 skippedRowCount() const { return m_skipped_row_count; }
    const LegacyDataStore::DataTable &data() const { return m_data; }
    const LegacyDataStore::TabsTable &tabs() const { return m_tabs; }
    const LegacyDataStore::ItemsTable &items() const { return m_items; }

private:
    LegacyDataStore::DataTable m_data;
    LegacyDataStore::TabsTable m_tabs;
    LegacyDataStore::ItemsTable m_items;
    // TODO: IGNORE: LegacyDataStore::CurrencyTable currency;

    bool m_valid{false};
    qint64 m_item_count{0};
    qint64 m_skipped_row_count{0};
};
