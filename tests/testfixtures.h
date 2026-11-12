// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include <QSqlDatabase>
#include <QSqlError>
#include <QTemporaryDir>
#include <QUuid>

#include <glaze/glaze.hpp>

#include "buyoutmanager.h"
#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/sqlitedatastore.h"
#include "datastore/stashrepo.h"
#include "item.h"
#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/stashtab.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/json_writers.h"

class BuyoutManagerFixture
{
public:
    BuyoutManagerFixture()
        : connectionName("buyout-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        data = std::make_unique<SqliteDataStore>(tempDir.filePath("acquisition.sqlite"));

        db.emplace(QSqlDatabase::addDatabase("QSQLITE", connectionName));
        db->setDatabaseName(":memory:");
        if (!db->open()) {
            qFatal("Failed to open test QSQLITE database: %s", qPrintable(db->lastError().text()));
        }

        repo = std::make_unique<BuyoutRepo>(*db);
        if (!repo->ensureSchema()) {
            qFatal("Failed to create test buyout schema");
        }

        manager = std::make_unique<BuyoutManager>(*data, *repo);
        QObject::connect(manager.get(),
                         &BuyoutManager::SetItemBuyout,
                         repo.get(),
                         qOverload<const Buyout &, const Item &>(&BuyoutRepo::saveItemBuyout));
        QObject::connect(manager.get(),
                         &BuyoutManager::SetLocationBuyout,
                         repo.get(),
                         qOverload<const Buyout &, const ItemLocation &>(
                             &BuyoutRepo::saveLocationBuyout));
    }

    ~BuyoutManagerFixture()
    {
        manager.reset();
        repo.reset();
        db->close();
        db.reset();
        QSqlDatabase::removeDatabase(connectionName);
        data.reset();
    }

    QTemporaryDir tempDir;
    QString connectionName;
    std::unique_ptr<SqliteDataStore> data;
    std::optional<QSqlDatabase> db;
    std::unique_ptr<BuyoutRepo> repo;
    std::unique_ptr<BuyoutManager> manager;
};

// The repos store the exact wire bytes a fetch received (F62). Tests seed
// them from typed fixtures instead, serializing the fixture where a real
// reply's bytes would flow — re-serialization is harmless in tests; it is
// the production cache that must be faithful.
inline bool saveStashFixture(StashRepo &repo,
                             const poe::StashTab &stash,
                             const QString &realm,
                             const QString &league)
{
    return repo.saveStash(stash, json::writeStash(stash), realm, league);
}

inline bool saveCharacterFixture(CharacterRepo &repo, const poe::Character &character)
{
    return repo.saveCharacter(character, json::writeCharacter(character));
}

inline ItemLocation makeTestStashLocation(const QString &id = "stash00001",
                                          const QString &name = "Test Tab",
                                          unsigned index = 0)
{
    poe::StashTab stash;
    stash.id = id;
    stash.name = name;
    stash.type = "PremiumStash";
    stash.index = index;
    return ItemLocation(stash);
}

inline Item makeTestItem(const char *json, const ItemLocation &loc)
{
    const std::string_view input(json);
    auto item = glz::read_json<poe::Item>(input);
    if (!item) {
        qFatal("makeTestItem: could not parse the test item json: %s",
               glz::format_error(item.error(), input).c_str());
    }
    return Item(*item, loc);
}

inline Item makeTestItem(const char *id)
{
    const ItemLocation loc = makeTestStashLocation();
    const QByteArray json = QString(R"({
        "verified": true,
        "w": 1,
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "%1",
        "name": "",
        "typeLine": "Test Item",
        "baseType": "Test Item",
        "identified": true,
        "ilvl": 1,
        "x": 0,
        "y": 0
    })")
                                .arg(id)
                                .toUtf8();
    return makeTestItem(json.constData(), loc);
}

inline Buyout makeChaosBuyout(double value)
{
    return Buyout(value,
                  Buyout::BUYOUT_TYPE_BUYOUT,
                  Currency::CURRENCY_CHAOS_ORB,
                  QDateTime::fromSecsSinceEpoch(1));
}
