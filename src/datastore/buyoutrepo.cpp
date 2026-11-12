// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "buyoutrepo.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include "currency.h"
#include "datastore/datastore_utils.h"
#include "item.h"
#include "itemlocation.h"

constexpr const char *CREATE_ITEM_BUYOUT_TABLE{R"(
CREATE TABLE IF NOT EXISTS item_buyouts (
    item_id         TEXT PRIMARY KEY,
    location_id     TEXT NOT NULL,
    location_type   TEXT NOT NULL CHECK (location_type IN ('character', 'stash')),
    currency        TEXT NOT NULL,
    inherited       INTEGER NOT NULL CHECK (inherited IN (0,1)),
    last_update     INTEGER NOT NULL,
    source          TEXT NOT NULL,
    type            TEXT NOT NULL,
    value           REAL NOT NULL
)
)"};

constexpr const char *CREATE_LOCATION_BUYOUT_TABLE{R"(
CREATE TABLE IF NOT EXISTS location_buyouts (
    location_id     TEXT PRIMARY KEY,
    location_type   TEXT NOT NULL CHECK (location_type IN ('character', 'stash')),
    currency        TEXT NOT NULL,
    inherited       INTEGER NOT NULL CHECK (inherited IN (0,1)),
    last_update     INTEGER NOT NULL,
    source          TEXT NOT NULL,
    type            TEXT NOT NULL,
    value           REAL NOT NULL
)
)"};

constexpr const char *UPSERT_ITEM_BUYOUT{R"(
INSERT INTO item_buyouts (
    item_id, location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :item_id, :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(item_id) DO UPDATE SET
    location_id     = excluded.location_id,
    location_type   = excluded.location_type,
    currency        = excluded.currency,
    inherited       = excluded.inherited,
    last_update     = excluded.last_update,
    source          = excluded.source,
    type            = excluded.type,
    value           = excluded.value
)"};

constexpr const char *UPSERT_LOCATION_BUYOUT{R"(
INSERT INTO location_buyouts (
    location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(location_id) DO UPDATE SET
    location_type   = excluded.location_type,
    currency        = excluded.currency,
    inherited       = excluded.inherited,
    last_update     = excluded.last_update,
    source          = excluded.source,
    type            = excluded.type,
    value           = excluded.value
)"};

constexpr const char *IMPORT_ITEM_BUYOUT{R"(
INSERT INTO item_buyouts (
    item_id, location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :item_id, :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(item_id) DO UPDATE SET
    location_id     = excluded.location_id,
    location_type   = excluded.location_type,
    currency        = excluded.currency,
    inherited       = excluded.inherited,
    last_update     = excluded.last_update,
    source          = excluded.source,
    type            = excluded.type,
    value           = excluded.value
WHERE item_buyouts.location_id IS NOT excluded.location_id
   OR item_buyouts.location_type IS NOT excluded.location_type
   OR item_buyouts.currency IS NOT excluded.currency
   OR item_buyouts.inherited IS NOT excluded.inherited
   OR item_buyouts.last_update IS NOT excluded.last_update
   OR item_buyouts.source IS NOT excluded.source
   OR item_buyouts.type IS NOT excluded.type
   OR item_buyouts.value IS NOT excluded.value
)"};

constexpr const char *IMPORT_LOCATION_BUYOUT{R"(
INSERT INTO location_buyouts (
    location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(location_id) DO UPDATE SET
    location_type   = excluded.location_type,
    currency        = excluded.currency,
    inherited       = excluded.inherited,
    last_update     = excluded.last_update,
    source          = excluded.source,
    type            = excluded.type,
    value           = excluded.value
WHERE location_buyouts.location_type IS NOT excluded.location_type
   OR location_buyouts.currency IS NOT excluded.currency
   OR location_buyouts.inherited IS NOT excluded.inherited
   OR location_buyouts.last_update IS NOT excluded.last_update
   OR location_buyouts.source IS NOT excluded.source
   OR location_buyouts.type IS NOT excluded.type
   OR location_buyouts.value IS NOT excluded.value
)"};

constexpr const char *INSERT_ITEM_BUYOUT{R"(
INSERT INTO item_buyouts (
    item_id, location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :item_id, :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(item_id) DO NOTHING
)"};

constexpr const char *INSERT_LOCATION_BUYOUT{R"(
INSERT INTO location_buyouts (
    location_id, location_type, currency, inherited, last_update, source, type, value
) VALUES (
    :location_id, :location_type, :currency, :inherited, :last_update, :source, :type, :value
)
ON CONFLICT(location_id) DO NOTHING
)"};

namespace {

    // The rows a user authored (#293): not a copy of a tab's price that an
    // older build stored on each item, marked inherited, and not a price
    // an older build read from a note or a tab's name, sourced game. Every
    // read that decides whether a price exists, or is manual, reads only
    // these; other rows are left in the tables and ignored. The same rule
    // is applied to both tables.
    QString selectAuthored(const char *select_from, const char *condition = nullptr)
    {
        QString sql = QString::fromLatin1(select_from)
                      + QStringLiteral(" WHERE inherited = 0 AND source <> 'game'");
        if (condition) {
            sql += QStringLiteral(" AND ") + QString::fromLatin1(condition);
        }
        return sql;
    }

    QString locationTypeTag(ItemLocationType type)
    {
        switch (type) {
        case ItemLocationType::STASH:
            return "stash";
        case ItemLocationType::CHARACTER:
            return "character";
        }
        return {};
    }

    void bindBuyout(QSqlQuery &query, const Buyout &buyout)
    {
        query.bindValue(":currency", buyout.CurrencyAsTag());
        query.bindValue(":inherited", buyout.inherited);
        query.bindValue(":last_update", buyout.last_update);
        query.bindValue(":source", buyout.BuyoutSourceAsTag());
        query.bindValue(":type", buyout.BuyoutTypeAsTag());
        query.bindValue(":value", buyout.value);
    }

} // namespace

BuyoutRepo::BuyoutRepo(QSqlDatabase &db)
    : m_db(db) {};

bool BuyoutRepo::resetRepo()
{
    // Deliberately does NOT drop its tables, unlike StashRepo and
    // CharacterRepo. Those hold caches that a refresh rebuilds from the API;
    // buyouts are authored by the user and cannot be recovered from anywhere,
    // so a schema migration must never destroy them as a side effect. (This
    // replaces a `DROP TABLE IF EXISTS buyouts` that named a table which has
    // never existed — the real tables are item_buyouts and location_buyouts —
    // and so had always been a no-op. Making that drop *work* would have
    // armed exactly the footgun this comment exists to prevent.)
    return ensureSchema();
}

bool BuyoutRepo::ensureSchema()
{
    constexpr std::array statements{
        CREATE_ITEM_BUYOUT_TABLE,
        CREATE_LOCATION_BUYOUT_TABLE,
    };

    QSqlQuery q(m_db);

    // Run setup statements.
    for (const auto &sql : statements) {
        if (!q.exec(sql)) {
            ds::logQueryError("BuyoutRepo::ensureSchema", q);
            return false;
        }
    }

    return true;
}

std::optional<std::unordered_map<QString, Buyout>> BuyoutRepo::loadItemBuyouts()
{
    spdlog::debug("BuyoutRepo: getting item buyouts");

    QSqlQuery q(m_db);
    if (!q.prepare(selectAuthored("SELECT"
                                  "    item_id, location_id, location_type,"
                                  "    currency, inherited, last_update, source, type, value"
                                  " FROM item_buyouts"))) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return std::nullopt;
    }

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::loadItemBuyouts()", q);
        return std::nullopt;
    }

    std::unordered_map<QString, Buyout> buyouts;

    while (q.next()) {
        const QString item_id = q.value(0).toString();
        Buyout buyout;
        buyout.currency = Currency::FromTag(q.value(3).toString());
        buyout.inherited = q.value(4).toBool();
        buyout.last_update = q.value(5).toDateTime();
        buyout.source = Buyout::TagAsBuyoutSource(q.value(6).toString());
        buyout.type = Buyout::TagAsBuyoutType(q.value(7).toString());
        buyout.value = q.value(8).toDouble();
        buyouts[item_id] = buyout;
    }
    if (q.lastError().isValid()) {
        ds::logQueryError("BuyoutRepo::loadItemBuyouts()", q);
        return std::nullopt;
    }
    return buyouts;
}

std::optional<std::unordered_map<QString, Buyout>> BuyoutRepo::loadLocationBuyouts()
{
    spdlog::debug("BuyoutRepo: getting location buyouts");

    QSqlQuery q(m_db);
    if (!q.prepare(selectAuthored("SELECT"
                                  "    location_id, location_type,"
                                  "    currency, inherited, last_update, source, type, value"
                                  " FROM location_buyouts"))) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return std::nullopt;
    }

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::loadLocationBuyouts()", q);
        return std::nullopt;
    }

    std::unordered_map<QString, Buyout> buyouts;

    while (q.next()) {
        const QString location_id = q.value(0).toString();
        Buyout buyout;
        buyout.currency = Currency::FromTag(q.value(2).toString());
        buyout.inherited = q.value(3).toBool();
        buyout.last_update = q.value(4).toDateTime();
        buyout.source = Buyout::TagAsBuyoutSource(q.value(5).toString());
        buyout.type = Buyout::TagAsBuyoutType(q.value(6).toString());
        buyout.value = q.value(7).toDouble();
        buyouts[location_id] = buyout;
    }
    if (q.lastError().isValid()) {
        ds::logQueryError("BuyoutRepo::loadLocationBuyouts()", q);
        return std::nullopt;
    }
    return buyouts;
}

bool BuyoutRepo::saveItemBuyout(const Buyout &buyout, const Item &item)
{
    spdlog::debug("BuyoutRepo: saving item buyout: PrettyName='{}' ({}), buyout='{}'",
                  item.PrettyName(),
                  item.id(),
                  buyout.AsText());

    const ItemLocation &location = item.location();
    return saveItemBuyout(buyout, item.id(), location.id(), location.type(), true)
           == BuyoutSaveResult::Saved;
}

BuyoutSaveResult BuyoutRepo::saveItemBuyout(const Buyout &buyout,
                                            const QString &item_id,
                                            const QString &location_id,
                                            ItemLocationType location_type,
                                            bool overwrite_existing)
{
    const QString location_type_tag = locationTypeTag(location_type);
    if (location_type_tag.isEmpty()) {
        spdlog::error("BuyoutRepo::saveItemBuyout: invalid item location type: {}", location_type);
        return BuyoutSaveResult::Error;
    }

    QSqlQuery q(m_db);
    if (!q.prepare(overwrite_existing ? UPSERT_ITEM_BUYOUT : INSERT_ITEM_BUYOUT)) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return BuyoutSaveResult::Error;
    }

    q.bindValue(":item_id", item_id);
    q.bindValue(":location_id", location_id);
    q.bindValue(":location_type", location_type_tag);
    q.bindValue(":currency", buyout.CurrencyAsTag());
    q.bindValue(":inherited", buyout.inherited);
    q.bindValue(":last_update", buyout.last_update);
    q.bindValue(":source", buyout.BuyoutSourceAsTag());
    q.bindValue(":type", buyout.BuyoutTypeAsTag());
    q.bindValue(":value", buyout.value);

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::saveItemBuyout()", q);
        return BuyoutSaveResult::Error;
    }
    return q.numRowsAffected() == 0 ? BuyoutSaveResult::Existing : BuyoutSaveResult::Saved;
}

bool BuyoutRepo::saveLocationBuyout(const Buyout &buyout, const ItemLocation &location)
{
    spdlog::debug("BuyoutRepo: saving location buyout: location='{}' ({}), buyout='{}'",
                  location.GetHeader(),
                  location.id(),
                  buyout.AsText());

    return saveLocationBuyout(buyout, location.id(), location.type(), true)
           == BuyoutSaveResult::Saved;
}

BuyoutSaveResult BuyoutRepo::saveLocationBuyout(const Buyout &buyout,
                                                const QString &location_id,
                                                ItemLocationType location_type,
                                                bool overwrite_existing)
{
    const QString location_type_tag = locationTypeTag(location_type);
    if (location_type_tag.isEmpty()) {
        spdlog::error("BuyoutRepo::saveLocationBuyout: invalid item location type: {}",
                      location_type);
        return BuyoutSaveResult::Error;
    }

    QSqlQuery q(m_db);
    if (!q.prepare(overwrite_existing ? UPSERT_LOCATION_BUYOUT : INSERT_LOCATION_BUYOUT)) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return BuyoutSaveResult::Error;
    }

    q.bindValue(":location_id", location_id);
    q.bindValue(":location_type", location_type_tag);
    q.bindValue(":currency", buyout.CurrencyAsTag());
    q.bindValue(":inherited", buyout.inherited);
    q.bindValue(":last_update", buyout.last_update);
    q.bindValue(":source", buyout.BuyoutSourceAsTag());
    q.bindValue(":type", buyout.BuyoutTypeAsTag());
    q.bindValue(":value", buyout.value);

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::saveLocationBuyout()", q);
        return BuyoutSaveResult::Error;
    }
    return q.numRowsAffected() == 0 ? BuyoutSaveResult::Existing : BuyoutSaveResult::Saved;
}

BuyoutBatchSaveResult BuyoutRepo::saveImportBatch(const std::vector<ItemBuyoutWrite> &items,
                                                  const std::vector<LocationBuyoutWrite> &locations)
{
    BuyoutBatchSaveResult result;
    if (!m_db.transaction()) {
        result.error = QString("Could not start buyout import transaction: %1")
                           .arg(m_db.lastError().text());
        return result;
    }

    const auto rollback = [this, &result](const QString &error) {
        result.error = error;
        if (!m_db.rollback()) {
            result.error += QString("; rollback also failed: %1").arg(m_db.lastError().text());
        }
    };

    // The manual guard reads the target row inside the same transaction,
    // so a manual price written between plan and apply (a later hand
    // edit, or the auto-refresh timer) is seen and protected here rather
    // than trusted to the plan-time prefill. A manual row identical to
    // the incoming write is this plan's own earlier import, so a re-run
    // still reports it already-present instead of protected.
    QSqlQuery item_source_query(m_db);
    QSqlQuery item_query(m_db);
    if (!items.empty()
        && (!item_source_query.prepare(
                selectAuthored("SELECT source,"
                               "    (location_id = :location_id AND location_type = :location_type"
                               "     AND currency = :currency AND inherited = :inherited"
                               "     AND last_update = :last_update AND source = :source"
                               "     AND type = :type AND value = :value) AS identical"
                               " FROM item_buyouts",
                               "item_id = :item_id"))
            || !item_query.prepare(IMPORT_ITEM_BUYOUT))) {
        rollback(
            QString("Could not prepare item buyout import: %1").arg(item_query.lastError().text()));
        return result;
    }
    result.item_results.reserve(items.size());
    for (const ItemBuyoutWrite &item : items) {
        if (!item.allow_manual_overwrite) {
            item_source_query.bindValue(":item_id", item.item_id);
            item_source_query.bindValue(":location_id", item.location_id);
            item_source_query.bindValue(":location_type", locationTypeTag(item.location_type));
            bindBuyout(item_source_query, item.buyout);
            if (!item_source_query.exec()) {
                ds::logQueryError("BuyoutRepo::saveImportBatch(item source)", item_source_query);
                rollback(QString("Could not check item '%1': %2")
                             .arg(item.item_id, item_source_query.lastError().text()));
                return result;
            }
            if (item_source_query.next() && item_source_query.value(0).toString() == "manual") {
                result.item_results.push_back(item_source_query.value(1).toBool()
                                                  ? BuyoutSaveResult::Existing
                                                  : BuyoutSaveResult::ProtectedManual);
                continue;
            }
        }
        item_query.bindValue(":item_id", item.item_id);
        item_query.bindValue(":location_id", item.location_id);
        item_query.bindValue(":location_type", locationTypeTag(item.location_type));
        bindBuyout(item_query, item.buyout);
        if (!item_query.exec()) {
            ds::logQueryError("BuyoutRepo::saveImportBatch(item)", item_query);
            rollback(QString("Could not save item '%1': %2")
                         .arg(item.item_id, item_query.lastError().text()));
            return result;
        }
        result.item_results.push_back(item_query.numRowsAffected() == 0 ? BuyoutSaveResult::Existing
                                                                        : BuyoutSaveResult::Saved);
    }

    QSqlQuery location_source_query(m_db);
    QSqlQuery location_query(m_db);
    if (!locations.empty()
        && (!location_source_query.prepare(
                selectAuthored("SELECT source,"
                               "    (location_type = :location_type AND currency = :currency"
                               "     AND inherited = :inherited AND last_update = :last_update"
                               "     AND source = :source AND type = :type AND value = :value)"
                               " AS identical"
                               " FROM location_buyouts",
                               "location_id = :location_id"))
            || !location_query.prepare(IMPORT_LOCATION_BUYOUT))) {
        rollback(QString("Could not prepare location buyout import: %1")
                     .arg(location_query.lastError().text()));
        return result;
    }
    result.location_results.reserve(locations.size());
    for (const LocationBuyoutWrite &location : locations) {
        if (!location.allow_manual_overwrite) {
            location_source_query.bindValue(":location_id", location.location_id);
            location_source_query.bindValue(":location_type",
                                            locationTypeTag(location.location_type));
            bindBuyout(location_source_query, location.buyout);
            if (!location_source_query.exec()) {
                ds::logQueryError("BuyoutRepo::saveImportBatch(location source)",
                                  location_source_query);
                rollback(QString("Could not check location '%1': %2")
                             .arg(location.location_id, location_source_query.lastError().text()));
                return result;
            }
            if (location_source_query.next()
                && location_source_query.value(0).toString() == "manual") {
                result.location_results.push_back(location_source_query.value(1).toBool()
                                                      ? BuyoutSaveResult::Existing
                                                      : BuyoutSaveResult::ProtectedManual);
                continue;
            }
        }
        location_query.bindValue(":location_id", location.location_id);
        location_query.bindValue(":location_type", locationTypeTag(location.location_type));
        bindBuyout(location_query, location.buyout);
        if (!location_query.exec()) {
            ds::logQueryError("BuyoutRepo::saveImportBatch(location)", location_query);
            rollback(QString("Could not save location '%1': %2")
                         .arg(location.location_id, location_query.lastError().text()));
            return result;
        }
        result.location_results.push_back(location_query.numRowsAffected() == 0
                                              ? BuyoutSaveResult::Existing
                                              : BuyoutSaveResult::Saved);
    }

    if (!m_db.commit()) {
        rollback(
            QString("Could not commit buyout import transaction: %1").arg(m_db.lastError().text()));
        return result;
    }

    result.success = true;
    return result;
}

bool BuyoutRepo::removeItemBuyout(const Item &item)
{
    spdlog::debug("BuyoutRepo: removing item buyout: PrettyName='{}' ({})",
                  item.PrettyName(),
                  item.id());

    QSqlQuery q(m_db);
    if (!q.prepare("DELETE FROM item_buyouts WHERE item_id = :item_id")) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return false;
    }

    q.bindValue(":item_id", item.id());

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::removeItemBuyout()", q);
        return false;
    }
    return true;
}

bool BuyoutRepo::removeLocationBuyout(const ItemLocation &location)
{
    spdlog::debug("BuyoutRepo: removing location buyout: '{}' ({})",
                  location.GetHeader(),
                  location.id());

    QSqlQuery q(m_db);
    if (!q.prepare("DELETE FROM location_buyouts WHERE location_id = :location_id")) {
        spdlog::error("BuyoutRepo: prepare() failed: {}", q.lastError().text());
        return false;
    }

    q.bindValue(":location_id", location.id());

    if (!q.exec()) {
        ds::logQueryError("BuyoutRepo::removeLocationBuyout()", q);
        return false;
    }
    return true;
}
