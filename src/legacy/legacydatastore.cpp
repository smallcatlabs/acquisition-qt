// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "legacy/legacydatastore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

    enum class ReadResult { Loaded, Skipped, Error };

    static ReadResult getByteArray(QSqlDatabase db, const QString &query, QByteArray &value)
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(query);
        if (!q.exec()) {
            spdlog::error("Database error calling exec(): {}: {}", query, db.lastError().text());
            return ReadResult::Error;
        }
        if (!q.next()) {
            spdlog::warn("LegacyDataStore: no row found for '{}'; skipping", query);
            return ReadResult::Skipped;
        }
        value = q.value(0).toByteArray();
        return ReadResult::Loaded;
    }

    static ReadResult getString(QSqlDatabase db, const QString &query, QString &value)
    {
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare(query);
        if (!q.exec()) {
            spdlog::error("Database error calling exec(): {}: {}", query, db.lastError().text());
            return ReadResult::Error;
        }
        if (!q.next()) {
            spdlog::warn("LegacyDataStore: no row found for '{}'; skipping", query);
            return ReadResult::Skipped;
        }
        value = q.value(0).toString();
        return ReadResult::Loaded;
    }

    template<typename T>
    static ReadResult getStruct(QSqlDatabase db, const QString &query, T &value)
    {
        QByteArray data;
        const ReadResult read_result = getByteArray(db, query, data);
        if (read_result != ReadResult::Loaded) {
            return read_result;
        }

        // Create a view over the QByteArray (it may contain '\0', so don't assume C-strings)
        const std::string_view sv{data.constData(), static_cast<std::size_t>(data.size())};

        constexpr glz::opts opts{.null_terminated = false, .error_on_unknown_keys = false};

        if (auto ec = glz::read<opts>(value, sv); ec) {
            spdlog::warn("LegacyDataStore: JSON error parsing {} from '{}'; skipping: {}",
                         typeid(T).name(),
                         query.toStdString(),
                         glz::format_error(ec, sv));
            return ReadResult::Skipped;
        }
        return ReadResult::Loaded;
    }

    template<typename T>
    static ReadResult getStructMap(QSqlDatabase db,
                                   const QString &query,
                                   std::unordered_map<QString, T> &value,
                                   qint64 &skipped_count)
    {
        QByteArray data;
        const ReadResult read_result = getByteArray(db, query, data);
        if (read_result != ReadResult::Loaded) {
            return read_result;
        }

        const std::string_view sv{data.constData(), static_cast<std::size_t>(data.size())};
        constexpr glz::opts opts{.null_terminated = false, .error_on_unknown_keys = false};

        std::unordered_map<QString, glz::raw_json> raw_values;
        if (auto ec = glz::read<opts>(raw_values, sv); ec) {
            value.clear();
            spdlog::warn("LegacyDataStore: JSON error parsing object from '{}'; skipping: {}",
                         query.toStdString(),
                         glz::format_error(ec, sv));
            return ReadResult::Skipped;
        }

        for (auto &[key, raw_value] : raw_values) {
            T parsed;
            if (auto ec = glz::read<opts>(parsed, raw_value.str); ec) {
                spdlog::warn("LegacyDataStore: JSON error parsing entry '{}' from '{}'; "
                             "skipping entry: {}",
                             key,
                             query,
                             glz::format_error(ec, raw_value.str));
                ++skipped_count;
                continue;
            }
            value.emplace(std::move(key), std::move(parsed));
        }
        return ReadResult::Loaded;
    }

} // namespace

//-------------------------------------------------------------------------------------------

LegacyDataStore::LegacyDataStore(const QString &filename)
{
    if (!QFile::exists(filename)) {
        spdlog::error("BuyoutCollection: file not found: {}", filename);
        return;
    }

    const QString connection_name = "LegacyDataStore:"
                                    + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
    const auto close_database = qScopeGuard([&db, &connection_name] {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection_name);
    });
    db.setConnectOptions("QSQLITE_OPEN_READONLY");
    db.setDatabaseName(filename);
    if (!db.open()) {
        spdlog::error("BuyoutCollection: cannot open {} due to error: {}",
                      filename,
                      db.lastError().text());
        return;
    }

    const auto load = [this](ReadResult result) {
        if (result == ReadResult::Skipped) {
            ++m_skipped_row_count;
        }
        return result != ReadResult::Error;
    };

    bool structurally_valid = true;
    structurally_valid &= load(
        getString(db, "SELECT value FROM data WHERE (key = 'db_version')", m_data.db_version));
    structurally_valid &= load(
        getString(db, "SELECT value FROM data WHERE (key = 'version')", m_data.version));
    structurally_valid &= load(getStructMap(db,
                                            "SELECT value FROM data WHERE (key = 'buyouts')",
                                            m_data.buyouts,
                                            m_skipped_row_count));
    structurally_valid &= load(getStructMap(db,
                                            "SELECT value FROM data WHERE (key = 'tab_buyouts')",
                                            m_data.tab_buyouts,
                                            m_skipped_row_count));
    structurally_valid &= load(
        getStruct(db, "SELECT value FROM tabs WHERE (type = 0)", m_tabs.stashes));
    structurally_valid &= load(
        getStruct(db, "SELECT value FROM tabs WHERE (type = 1)", m_tabs.characters));
    if (!structurally_valid) {
        spdlog::error("LegacyDataStore: required database tables could not be read from {}",
                      filename);
        return;
    }

    const QString statement = "SELECT loc, value FROM items";
    QSqlQuery query(db);
    query.setForwardOnly(true);
    query.prepare(statement);
    if (!query.exec()) {
        spdlog::error("LegacyDataStore: error executing '{}': {}",
                      statement,
                      query.lastError().text());
        return;
    }

    m_item_count = 0;
    while (query.next()) {
        const QString loc = query.value(0).toString();
        const QByteArray ba = query.value(1).toByteArray();

        // Parse from a size-aware view (don't assume null-terminated input)
        const std::string_view sv{ba.constData(), static_cast<std::size_t>(ba.size())};
        constexpr glz::opts opts{.null_terminated = false, .error_on_unknown_keys = false};

        std::vector<glz::raw_json> raw_items;
        if (auto ec = glz::read<opts>(raw_items, sv); ec) {
            spdlog::warn("LegacyDataStore: error parsing 'items' for '{}'; skipping row: {}",
                         loc.toStdString(),
                         glz::format_error(ec, sv));
            ++m_skipped_row_count;
            continue;
        }

        std::vector<LegacyItem> result;
        result.reserve(raw_items.size());
        for (const glz::raw_json &raw_item : raw_items) {
            LegacyItem item;
            if (auto ec = glz::read<opts>(item, raw_item.str); ec) {
                spdlog::warn("LegacyDataStore: error parsing item for '{}'; skipping item: {}",
                             loc,
                             glz::format_error(ec, raw_item.str));
                ++m_skipped_row_count;
                continue;
            }
            result.push_back(std::move(item));
        }

        m_item_count += static_cast<qint64>(result.size());
        m_items[loc] = std::move(result); // requires your QString adapter in glaze_qt.h
    }

    if (query.lastError().isValid()) {
        spdlog::error("LegacyDataStore: error moving to next record in 'items': {}",
                      query.lastError().text());
        return;
    }

    query.finish();

    m_valid = true;
}
