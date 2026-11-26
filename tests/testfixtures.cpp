// SPDX-License-Identifier: GPL-3.0-or-later

#include "testfixtures.h"

#include <string_view>

#include <QSqlError>
#include <QUuid>

#include <glaze/glaze.hpp>

#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/json_writers.h"
#include "util/oauthtoken.h"

// A session identity for a test; every field must be non-empty.
SessionIdentity makeTestIdentity(const QString &account, const QString &realm, const QString &league)
{
    auto identity = SessionIdentity::Make(account, realm, league);
    if (!identity) {
        qFatal("makeTestIdentity: every field of a session identity must be set");
    }
    return *identity;
}

BuyoutManagerFixture::BuyoutManagerFixture()
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
}

BuyoutManagerFixture::~BuyoutManagerFixture()
{
    manager.reset();
    repo.reset();
    db->close();
    db.reset();
    QSqlDatabase::removeDatabase(connectionName);
    data.reset();
}

// The repos store the exact wire bytes a fetch received (F62). Tests seed
// them from typed fixtures instead, serializing the fixture where a real
// reply's bytes would flow — re-serialization is harmless in tests; it is
// the production cache that must be faithful.
bool saveStashFixture(StashRepo &repo,
                      const poe::StashTab &stash,
                      const QString &realm,
                      const QString &league)
{
    return repo.saveStash(stash, json::writeStash(stash), realm, league);
}

bool saveCharacterFixture(CharacterRepo &repo, const poe::Character &character)
{
    return repo.saveCharacter(character, json::writeCharacter(character));
}

ItemLocation makeTestStashLocation(const QString &id, const QString &name, unsigned index)
{
    poe::StashTab stash;
    stash.id = id;
    stash.name = name;
    stash.type = "PremiumStash";
    stash.index = index;
    return ItemLocation(stash);
}

poe::Item makeTestWireItem(const char *json)
{
    const std::string_view input(json);
    auto item = glz::read_json<poe::Item>(input);
    if (!item) {
        qFatal("makeTestWireItem: could not parse the test item json: %s",
               glz::format_error(item.error(), input).c_str());
    }
    return *item;
}

poe::Item makeTestWireItemWithId(const char *id)
{
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
    return makeTestWireItem(json.constData());
}

Item makeTestItem(const char *json, const ItemLocation &loc)
{
    return Item(makeTestWireItem(json), loc);
}

Item makeTestItem(const char *id)
{
    return Item(makeTestWireItemWithId(id), makeTestStashLocation());
}

QString serializeTestToken(const OAuthToken &token)
{
    std::string serialized;
    if (glz::write_json(token, serialized)) {
        qFatal("serializeTestToken: cannot serialize the token");
    }
    return QString::fromStdString(serialized);
}

Buyout makeChaosBuyout(double value)
{
    return Buyout(value,
                  Buyout::BUYOUT_TYPE_BUYOUT,
                  Currency::CURRENCY_CHAOS_ORB,
                  QDateTime::fromSecsSinceEpoch(1));
}
