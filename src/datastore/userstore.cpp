// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "datastore/userstore.h"

#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/datastore_utils.h"
#include "datastore/stashrepo.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

// Shape of the tables. Bump when the DDL changes and add a ladder step in
// migrate(). This is separate from json::PAYLOAD_VERSION, which versions the
// json stored inside them: a payload change needs no schema bump, and this
// one is compared with '<' (migrations replay forward) where the payload
// version is compared with '!=' (a downgrade must not misparse newer blobs).
static constexpr int SCHEMA_VERSION = 3;

constexpr unsigned int QSQLITE_BUSY_TIMEOUT{5000};

constexpr std::array CONNECTION_PRAGMAS{
    "PRAGMA busy_timeout=5000",
    "PRAGMA temp_store=MEMORY",
    "PRAGMA journal_mode=WAL",
    "PRAGMA synchronous=NORMAL",
    "PRAGMA foreign_keys=OFF",
};

UserStore::UserStore(const QDir &dir, const QString &username)
{
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString connection = "UserStore:" + username + ":" + uuid;
    m_db = QSqlDatabase::addDatabase("QSQLITE", connection);

    m_characters = std::make_unique<CharacterRepo>(m_db);
    m_stashes = std::make_unique<StashRepo>(m_db);
    m_buyouts = std::make_unique<BuyoutRepo>(m_db);

    m_usable = open(dir, username);
    if (!m_usable) {
        spdlog::error("UserStore: the store for '{}' is not usable; prices will not be posted",
                      username);
    }
    m_buyouts->setStoreUsable(m_usable);
}

bool UserStore::open(const QDir &dir, const QString &username)
{
    if (!m_db.isValid()) {
        spdlog::error("UserStore: database is not valid: {}", m_db.lastError().text());
        return false;
    }

    QDir dataDir(dir);
    if (!dataDir.mkpath(dir.absolutePath())) {
        spdlog::error("UserStore: unable to create directory: {}", dir.absolutePath());
        return false;
    }

    const QString filename = dataDir.absoluteFilePath("userstore-" + username + ".db");
    m_db.setDatabaseName(filename);
    m_db.setConnectOptions(QString("QSQLITE_BUSY_TIMEOUT=%1").arg(QSQLITE_BUSY_TIMEOUT));
    spdlog::debug("UserStore: created database connection '{}' to '{}'",
                  m_db.connectionName(),
                  m_db.databaseName());

    if (!m_db.open()) {
        spdlog::error("UserStore: error opening database connection '{}' to '{}': {}",
                      m_db.connectionName(),
                      m_db.databaseName(),
                      m_db.lastError().text());
        return false;
    }

    QSqlQuery q(m_db);
    for (const auto &pragma : CONNECTION_PRAGMAS) {
        if (!q.exec(pragma)) {
            spdlog::warn("UserStore: pragma failed: {} ({})", pragma, q.lastError().text());
        }
    }

    const std::optional<int> version = userVersion();
    if (!version) {
        // Not readable is not "fresh": treating it as version 0 would rebuild
        // the cache tables of a database we cannot see into.
        return false;
    }
    spdlog::debug("UserStore: user_version is {}, schema version is {}", *version, SCHEMA_VERSION);
    if (*version > SCHEMA_VERSION) {
        // A newer build wrote this database (#257). Its shape is not ours to
        // migrate, and nothing here may stamp an older version over it.
        spdlog::error("UserStore: user_version {} is newer than this build's schema version {}",
                      *version,
                      SCHEMA_VERSION);
        return false;
    }
    if (*version < SCHEMA_VERSION) {
        spdlog::info("UserStore: migrating from user_version {} to {}", *version, SCHEMA_VERSION);
        return migrate();
    }
    return true;
}

UserStore::~UserStore()
{
    m_buyouts = nullptr;
    m_characters = nullptr;
    m_stashes = nullptr;

    // Close the database.
    if (m_db.isValid()) {
        m_db.close();
    }

    // Grab the connection name.
    const QString connection = m_db.connectionName();

    // Clear member variables.
    m_db = QSqlDatabase();

    // Remove the database connection.
    if (QSqlDatabase::contains(connection)) {
        QSqlDatabase::removeDatabase(connection);
    }
}

namespace {

    // True when the table exists and its primary key is exactly (id), which
    // is what the repos' ON CONFLICT(id) upserts require. Databases created
    // by v0.16.0-alpha.2 through alpha.6 have composite primary keys instead
    // — (realm, league, id) on stashes, (realm, id) on characters — which
    // SQLite rejects at prepare time with "ON CONFLICT clause does not match
    // any PRIMARY KEY or UNIQUE constraint".
    bool primaryKeyIsId(QSqlDatabase &db, const QString &table)
    {
        QSqlQuery q(db);
        if (!q.exec("PRAGMA table_info(" + table + ")")) {
            spdlog::error("UserStore: error reading table_info for {}: {}",
                          table,
                          q.lastError().text());
            return false;
        }
        QStringList pk_columns;
        while (q.next()) {
            if (q.value("pk").toInt() > 0) {
                pk_columns.append(q.value("name").toString());
            }
        }
        return (pk_columns.size() == 1) && (pk_columns.front() == "id");
    }

} // namespace

std::optional<int> UserStore::userVersion()
{
    QSqlQuery q(m_db);
    if (!q.exec("PRAGMA user_version") || !q.next()) {
        spdlog::error("UserStore: error getting user_version: {}", q.lastError().text());
        return std::nullopt;
    }
    return q.value(0).toInt();
}

bool UserStore::migrate()
{
    QSqlQuery q(m_db);

    // Acquire a write lock so only one migrator proceeds.
    if (!q.exec("BEGIN IMMEDIATE")) {
        spdlog::error("UserStore: migration could not acquire a write lock: {}",
                      q.lastError().text());
        return false;
    }

    // Another connection might have migrated while we waited.
    const std::optional<int> read_version = userVersion();
    if (!read_version) {
        m_db.rollback();
        return false;
    }
    const int version = *read_version;
    if (version > SCHEMA_VERSION) {
        spdlog::error("UserStore: a newer build migrated to {} while waiting for the lock", version);
        m_db.rollback();
        return false;
    }
    if (version == SCHEMA_VERSION) {
        spdlog::debug("UserStore: migration occured while waiting for the lock");
        return m_db.commit();
    }

    // Two distinct cases, and they must not be mixed.
    //
    // Version 0 is a database with no schema of ours at all (a fresh file, or
    // one predating the repos). It is built directly at the current schema,
    // because the CREATE statements always carry every column — so it must
    // NOT then replay the ladder below, which would try to add columns the
    // CREATE just made (ALTER ADD COLUMN fails on a duplicate).
    //
    // Anything else is an existing database that must replay every step it
    // missed. Those steps test `version < N` rather than `version == N - 1`
    // so they compose: a database several versions behind runs all of them,
    // in order.
    if (version < 1) {
        if (!m_characters->resetRepo()) {
            m_db.rollback();
            return false;
        }

        if (!m_stashes->resetRepo()) {
            m_db.rollback();
            return false;
        }

        if (!m_buyouts->resetRepo()) {
            m_db.rollback();
            return false;
        }

    } else {
        // 1 -> 2: add the payload version column. Existing rows get NULL,
        // which never equals json::PAYLOAD_VERSION, so their cached json is
        // treated as unfetched and refetched on the next refresh. That is the
        // intended outcome here: 3.29 turned implicitMods/explicitMods into
        // objects, so every blob written before this version is unreadable
        // anyway. No bulk UPDATE is needed, and no future payload bump will
        // need a migration at all — bumping the constant is enough to strand
        // stale rows.
        if (version < 2) {
            constexpr std::array statements{
                "ALTER TABLE stashes ADD COLUMN json_version INTEGER",
                "ALTER TABLE characters ADD COLUMN json_version INTEGER",
            };
            for (const auto &sql : statements) {
                if (!q.exec(sql)) {
                    ds::logQueryError("UserStore::migrate", q);
                    m_db.rollback();
                    return false;
                }
            }
        }

        // 2 -> 3: repair databases created by v0.16.0-alpha.2 through
        // alpha.6. Those alphas built stashes and characters with composite
        // primary keys and stamped user_version 1 before alpha.7 switched to
        // id-only keys and ON CONFLICT(id) upserts without bumping the schema
        // version, so every later release rejected their upserts at prepare
        // time. They also predate BuyoutRepo, so its tables may be missing
        // entirely. The rebuild is conditional on the actual key shape:
        // stashes and characters are refetchable caches, but a healthy
        // database must not lose its rows to someone else's repair (the tab
        // and character lists drive the UI until the next refresh). Buyout
        // tables get CREATE IF NOT EXISTS only — they hold user-authored
        // data, so no path here may drop them.
        if (version < 3) {
            if (!primaryKeyIsId(m_db, "stashes")) {
                spdlog::info("UserStore: rebuilding stashes (pre-alpha.7 primary key)");
                if (!m_stashes->resetRepo()) {
                    m_db.rollback();
                    return false;
                }
            }
            if (!primaryKeyIsId(m_db, "characters")) {
                spdlog::info("UserStore: rebuilding characters (pre-alpha.7 primary key)");
                if (!m_characters->resetRepo()) {
                    m_db.rollback();
                    return false;
                }
            }
            if (!m_buyouts->ensureSchema()) {
                m_db.rollback();
                return false;
            }
        }
    }

    // Update the user_version.
    if (!q.exec(QString("PRAGMA user_version=%1").arg(SCHEMA_VERSION))) {
        spdlog::error("UserStore: error setting user_version: {}", q.lastError().text());
        m_db.rollback();
        return false;
    }

    // Commit the transaction.
    if (!m_db.commit()) {
        spdlog::error("UserStore: error committing migration: {}", m_db.lastError().text());
        return false;
    }

    spdlog::info("UserStore: migrated from version {} to {}", version, SCHEMA_VERSION);
    return true;
}
