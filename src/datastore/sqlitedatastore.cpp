// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "datastore/sqlitedatastore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>

#include "util/spdlog_qt.h" // IWYU pragma: keep

SqliteDataStore::SqliteDataStore(const QString &filename)
    : m_filename(filename)
{
    QDir dir(QDir::cleanPath(m_filename + "/.."));
    if (!dir.exists()) {
        QDir().mkpath(dir.path());
    }

    if (!QFile(filename).exists()) {
        // If the file doesn't exist, it's possible there's an old data file from
        // before the addition of account name discriminators. Look for one of those
        // files and rename if it found.
        const QString old_filename = m_filename.chopped(5);
        if (QFile(old_filename).exists()) {
            spdlog::warn("Renaming old data file with new account discriminator: {}", m_filename);
            if (!QFile(old_filename).rename(m_filename)) {
                spdlog::error("Unable to rename file: {}", old_filename);
            }
        }
    }

    // Open the database and make sure tables are created if they don't exist.
    // Old databases may also contain "tabs" and "items" tables from before
    // the December 2025 move to StashRepo/CharacterRepo; they are ignored,
    // and kept: such a file is valid input for the legacy buyout importer.
    // The file is not vacuumed on open (#292): that rewrote all of it, on
    // the main thread at login, and reclaimed almost nothing.
    CreateTable("data", "key TEXT PRIMARY KEY, value BLOB");
    CreateTable("currency", "timestamp INTEGER PRIMARY KEY, value TEXT");
}

SqliteDataStore::~SqliteDataStore()
{
    // Close and remove each database connection.
    QMutexLocker locker(&m_mutex);
    const auto &connections = m_connection_names;
    for (const QString &connection : connections) {
        if (QSqlDatabase::contains(connection)) {
            QSqlDatabase::database(connection).close();
            QSqlDatabase::removeDatabase(connection);
        }
    }
    m_connection_names.clear();
}

QString SqliteDataStore::getThreadLocalConnectionName() const
{
    // Create a database connection name for this thread.
    const QString file_name = QFileInfo(m_filename).fileName();
    const quintptr thread_id = reinterpret_cast<quintptr>(QThread::currentThread());
    return QString("sqlite-%1-%2").arg(file_name).arg(thread_id);
}

QSqlDatabase SqliteDataStore::getThreadLocalDatabase()
{
    QString connection = getThreadLocalConnectionName();
    if (!QSqlDatabase::contains(connection)) {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(m_filename);
        if (!db.open()) {
            spdlog::error("Failed to open database {}: {}", m_filename, db.lastError().text());
        } else {
            // Overwrite deleted and replaced content instead of leaving it in
            // free space: the global file holds the OAuth token, and a
            // cleared or rotated token must not stay recoverable from the
            // file or its backups. It costs extra writes on each delete,
            // which this small key-value store does rarely.
            QSqlQuery pragma(db);
            if (!pragma.exec("PRAGMA secure_delete=ON")) {
                spdlog::error("Failed to enable secure_delete on {}: {}",
                              m_filename,
                              pragma.lastError().text());
            }
            // Add the connection to the list so we can close it later.
            QMutexLocker locker(&m_mutex);
            m_connection_names.insert(connection);
        }
    }
    return QSqlDatabase::database(connection);
}

void SqliteDataStore::CreateTable(const QString &name, const QString &fields)
{
    QSqlDatabase db = getThreadLocalDatabase();
    QSqlQuery query(db);
    query.prepare("CREATE TABLE IF NOT EXISTS " + name + "(" + fields + ")");
    if (query.exec() == false) {
        spdlog::error("CreateTable(): failed to create {}: {}", name, query.lastError().text());
    }
}

QString SqliteDataStore::Get(const QString &key, const QString &default_value)
{
    QSqlDatabase db = getThreadLocalDatabase();
    QSqlQuery query(db);
    query.prepare("SELECT value FROM data WHERE key = ?");
    query.bindValue(0, key);
    if (query.exec() == false) {
        spdlog::error("Error getting data for {}: {}", key, query.lastError().text());
        return default_value;
    }
    if (query.next() == false) {
        if (query.isActive() == false) {
            spdlog::error("Error getting result for {}: {}", key, query.lastError().text());
        }
        return default_value;
    }
    QString result = query.value(0).toByteArray();
    return result;
}

void SqliteDataStore::Set(const QString &key, const QString &value)
{
    QSqlDatabase db = getThreadLocalDatabase();
    QSqlQuery query(db);
    query.prepare("INSERT OR REPLACE INTO data (key, value) VALUES (?, ?)");
    query.bindValue(0, key);
    query.bindValue(1, value);
    if (query.exec() == false) {
        spdlog::error("Error setting value {}", key);
    }
}

bool SqliteDataStore::InsertCurrencyUpdate(const CurrencyUpdate &update)
{
    QSqlDatabase db = getThreadLocalDatabase();
    QSqlQuery query(db);
    // The key is a time in seconds, so two updates in the same second would
    // collide; the later one replaces the earlier (#285).
    query.prepare("INSERT OR REPLACE INTO currency (timestamp, value) VALUES (?, ?)");
    query.bindValue(0, update.timestamp);
    query.bindValue(1, update.value);
    if (query.exec() == false) {
        spdlog::error("Error inserting currency update: {}", query.lastError().text());
        return false;
    }
    return true;
}

std::vector<CurrencyUpdate> SqliteDataStore::GetAllCurrency()
{
    QSqlDatabase db = getThreadLocalDatabase();
    QSqlQuery query(db);
    query.prepare("SELECT timestamp, value FROM currency ORDER BY timestamp ASC");
    std::vector<CurrencyUpdate> result;
    if (query.exec() == false) {
        spdlog::error("Error getting currency updates: {}", query.lastError().text());
        return {};
    }
    while (query.next()) {
        CurrencyUpdate update = CurrencyUpdate();
        update.timestamp = query.value(0).toLongLong();
        update.value = query.value(1).toString();
        result.push_back(update);
    }
    if (query.lastError().isValid()) {
        spdlog::error("Error getting currency.");
        return {};
    }
    return result;
}

QString SqliteDataStore::MakeFilename(const QString &username, const QString &league)
{
    // We somehow have to manage the fact that usernames now have a numeric discriminator,
    // e.g. GERWARIC#7694 instead of just GERWARIC.
    if (username.contains("#")) {
        // Build the filename as though the username did not have a discriminator,
        // then append the discriminator. This approach makes it possible to recognise
        // old data files more easily, because the discriminator is kept out of the hash.
        const QStringList parts = username.split("#");
        const QString base_username = parts[0];
        const QString discriminator = parts[1];
        const std::string key = base_username.toStdString() + "|" + league.toStdString();
        const QString datafile
            = QString(QCryptographicHash::hash(key.c_str(), QCryptographicHash::Md5).toHex()) + "-"
              + discriminator;
        return datafile;
    } else {
        const std::string key = username.toStdString() + "|" + league.toStdString();
        const QString datafile = QString(
            QCryptographicHash::hash(key.c_str(), QCryptographicHash::Md5).toHex());
        return datafile;
    }
}
