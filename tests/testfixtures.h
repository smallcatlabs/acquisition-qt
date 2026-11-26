// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>

#include <QSqlDatabase>
#include <QTemporaryDir>

#include "buyoutmanager.h"
#include "datastore/buyoutrepo.h"
#include "datastore/characterrepo.h"
#include "datastore/sqlitedatastore.h"
#include "datastore/stashrepo.h"
#include "item.h"
#include "poe/types/character.h"
#include "poe/types/item.h"
#include "poe/types/stashtab.h"
#include "sessionidentity.h"
#include "util/json_writers.h"

struct OAuthToken;

// The fixtures every suite shares. They are compiled once, into the test
// support library (testfixtures.cpp, acq_testsupport); this header declares
// them, and a suite that uses them does not compile them again (#476).

// A session identity for a test; every field must be non-empty.
SessionIdentity makeTestIdentity(const QString &account,
                                 const QString &realm,
                                 const QString &league);

class BuyoutManagerFixture
{
public:
    BuyoutManagerFixture();
    ~BuyoutManagerFixture();

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
bool saveStashFixture(StashRepo &repo,
                      const poe::StashTab &stash,
                      const QString &realm,
                      const QString &league);

bool saveCharacterFixture(CharacterRepo &repo, const poe::Character &character);

ItemLocation makeTestStashLocation(const QString &id = "stash00001",
                                   const QString &name = "Test Tab",
                                   unsigned index = 0);

// The wire item read from the JSON of one item, as the API sends it; a JSON
// the reader refuses ends the test run.
poe::Item makeTestWireItem(const char *json);

// A one-cell wire item with this id.
poe::Item makeTestWireItemWithId(const char *id);

// An item built from the JSON of one item, as the API sends it.
Item makeTestItem(const char *json, const ItemLocation &loc);

// A one-cell item with this id, in makeTestStashLocation().
Item makeTestItem(const char *id);

// A token as the global store holds it under "oauth_token".
QString serializeTestToken(const OAuthToken &token);

Buyout makeChaosBuyout(double value);
