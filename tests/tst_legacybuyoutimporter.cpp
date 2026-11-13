// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <xlsxcellrange.h>
#include <xlsxdocument.h>

#include "legacy/legacybuyoutimporter.h"
#include "legacy/legacyitem.h"
#include "testfixtures.h"

namespace {

    void createLegacyDatabase(const QString &filename, const QString &db_version)
    {
        LegacyItem stash_item;
        stash_item.name = "";
        stash_item.typeLine = "Chaos Orb";
        stash_item._tab_label = "Priced";
        const QString stash_hash = stash_item.hash();

        LegacyItem character_item;
        character_item.name = "";
        character_item.typeLine = "Divine Orb";
        character_item._character = "Bob";
        const QString character_hash = character_item.hash();

        const QString buyout_json = QString(R"json({
                "%1":{"value":5,"last_update":1700000000,"type":"b/o","currency":"chaos","source":"manual","inherited":false},
                "%2":{"value":6,"last_update":1700000001,"type":"price","currency":"divine","source":"manual","inherited":false},
                "orphan-hash":{"value":7,"last_update":1700000002,"type":"b/o","currency":"chaos","source":"manual","inherited":false}
            })json")
                                        .arg(stash_hash, character_hash);
        const QString tab_buyout_json = R"json({
            "stash:Priced":{"value":8,"last_update":1700000003,"type":"b/o","currency":"chaos","source":"manual","inherited":false},
            "character:Bob":{"value":9,"last_update":1700000004,"type":"price","currency":"divine","source":"manual","inherited":false}
        })json";
        const QString stashes_json = R"json([
            {"id":"stash-a","name":"Priced","type":"PremiumStash","index":0,"metadata":{"colour":"7f7f7f"}},
            {"id":"stash-b","name":"Priced","type":"PremiumStash","index":1,"metadata":{"colour":"7f7f7f"}}
        ])json";
        const QString characters_json = R"json([
            {"id":"character-id-bob","name":"Bob","realm":"pc","class":"Marauder","league":"Standard","level":90,"experience":1}
        ])json";
        const QString stash_items_json = R"json([
            {"id":"item-a","name":"","typeLine":"Chaos Orb","_tab_label":"Priced","ilvl":0,"frameType":5},
            {"id":"item-b","name":"","typeLine":"Chaos Orb","_tab_label":"Priced","ilvl":0,"frameType":5}
        ])json";
        const QString character_items_json = R"json([
            {"id":"item-character","name":"","typeLine":"Divine Orb","_character":"Bob","inventoryId":"MainInventory"}
        ])json";

        const QString connection_name = "legacy-import-source:"
                                        + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
            db.setDatabaseName(filename);
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TABLE data (key TEXT PRIMARY KEY, value BLOB)"));
            QVERIFY(query.exec("CREATE TABLE tabs (type INTEGER PRIMARY KEY, value BLOB)"));
            QVERIFY(query.exec("CREATE TABLE items (loc TEXT PRIMARY KEY, value BLOB)"));

            query.prepare("INSERT INTO data (key, value) VALUES (?, ?)");
            const auto insert_data = [&query](const QString &key, const QString &value) {
                query.bindValue(0, key);
                query.bindValue(1, value);
                return query.exec();
            };
            QVERIFY(insert_data("db_version", db_version));
            QVERIFY(insert_data("version", "0.15.0"));
            QVERIFY(insert_data("buyouts", buyout_json));
            QVERIFY(insert_data("tab_buyouts", tab_buyout_json));

            query.prepare("INSERT INTO tabs (type, value) VALUES (?, ?)");
            query.bindValue(0, 0);
            query.bindValue(1, stashes_json);
            QVERIFY(query.exec());
            query.bindValue(0, 1);
            query.bindValue(1, characters_json);
            QVERIFY(query.exec());

            query.prepare("INSERT INTO items (loc, value) VALUES (?, ?)");
            query.bindValue(0, "stash-a");
            query.bindValue(1, stash_items_json);
            QVERIFY(query.exec());
            query.bindValue(0, "Bob");
            query.bindValue(1, character_items_json);
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
    }

    void reviseLegacyDatabaseForPlan(const QString &filename)
    {
        LegacyItem stash_item;
        stash_item.name = "";
        stash_item.typeLine = "Chaos Orb";
        stash_item._tab_label = "Old Priced";
        const QString stash_hash = stash_item.hash();

        LegacyItem character_item;
        character_item.name = "";
        character_item.typeLine = "Divine Orb";
        character_item._character = "Bob";
        const QString character_hash = character_item.hash();

        const QString buyout_json = QString(R"json({
                "%1":{"value":5,"last_update":1700000000,"type":"b/o","currency":"chaos","source":"manual","inherited":false},
                "%2":{"value":6,"last_update":1700000001,"type":"price","currency":"divine","source":"manual","inherited":true},
                "orphan-hash":{"value":7,"last_update":1700000002,"type":"b/o","currency":"chaos","source":"manual","inherited":false}
            })json")
                                        .arg(stash_hash, character_hash);
        const QString tab_buyout_json = R"json({
            "stash:Old Priced":{"value":8,"last_update":1700000003,"type":"b/o","currency":"chaos","source":"manual","inherited":false},
            "character:Bob":{"value":9,"last_update":1700000004,"type":"price","currency":"divine","source":"manual","inherited":false}
        })json";
        const QString long_stash_a = "0123456789" + QString(54, 'a');
        const QString long_stash_b = "fedcba9876" + QString(54, 'b');
        const QString stashes_json = QString(R"json([
                {"id":"%1","n":"Old Priced","type":"PremiumStash","index":0,"metadata":{"colour":"7f7f7f"}},
                {"id":"%2","n":"Old Priced","type":"PremiumStash","index":1,"metadata":{"colour":"7f7f7f"}}
            ])json")
                                         .arg(long_stash_a, long_stash_b);
        const QString characters_json = R"json([
            {"name":"Bob","realm":"pc","class":"Marauder","league":"Standard","level":90,"experience":1}
        ])json";
        const QString stash_items_json = R"json([
            {"id":"broken","name":"","typeLine":"Broken","_tab_label":"Old Priced","sockets":"not-an-array"},
            {"id":"item-a","name":"","typeLine":"Chaos Orb","_tab_label":"Old Priced","ilvl":0,"frameType":5},
            {"id":"item-b","name":"","typeLine":"Chaos Orb","_tab_label":"Old Priced","ilvl":0,"frameType":5}
        ])json";

        const QString connection_name = "legacy-plan-update:"
                                        + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
            db.setDatabaseName(filename);
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery query(db);
            query.prepare("UPDATE data SET value = ? WHERE key = ?");
            const auto update_data = [&query](const QString &key, const QString &value) {
                query.bindValue(0, value);
                query.bindValue(1, key);
                return query.exec();
            };
            QVERIFY(update_data("buyouts", buyout_json));
            QVERIFY(update_data("tab_buyouts", tab_buyout_json));

            query.prepare("UPDATE tabs SET value = ? WHERE type = ?");
            query.bindValue(0, stashes_json);
            query.bindValue(1, 0);
            QVERIFY(query.exec());
            query.bindValue(0, characters_json);
            query.bindValue(1, 1);
            QVERIFY(query.exec());

            QVERIFY(query.exec("DELETE FROM items WHERE loc = 'stash-a'"));
            query.prepare("INSERT INTO items (loc, value) VALUES (?, ?)");
            query.bindValue(0, "0123456789");
            query.bindValue(1, stash_items_json);
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
    }

    // Marks the revised source's item price for the "Old Priced" chaos orbs
    // and its "stash:Old Priced" tab price as read from the game, as a pre-
    // 0.16 build stored a note's or a tab name's price.
    void markLegacyPricesFromTheGame(const QString &filename)
    {
        const QString connection_name = "legacy-game-rows:"
                                        + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
            db.setDatabaseName(filename);
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery query(db);
            const auto mark = [&query](const QString &key, const QString &price) {
                QVERIFY(
                    query.prepare("UPDATE data SET value = replace(value, ?, ?) WHERE key = ?"));
                query.bindValue(0, price + R"("source":"manual")");
                query.bindValue(1, price + R"("source":"game")");
                query.bindValue(2, key);
                QVERIFY(query.exec());
                QCOMPARE(query.numRowsAffected(), 1);
            };
            mark("buyouts",
                 R"("value":5,"last_update":1700000000,"type":"b/o","currency":"chaos",)");
            mark("tab_buyouts",
                 R"("value":8,"last_update":1700000003,"type":"b/o","currency":"chaos",)");
            db.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
    }

    int gameRows(QSqlDatabase &db)
    {
        QSqlQuery query(db);
        if (!query.exec("SELECT (SELECT COUNT(*) FROM item_buyouts WHERE source = 'game')"
                        " + (SELECT COUNT(*) FROM location_buyouts WHERE source = 'game')")
            || !query.next()) {
            qFatal("Could not count game rows");
        }
        return query.value(0).toInt();
    }

    struct PlanningFixture
    {
        PlanningFixture()
            : stashes(std::make_unique<StashRepo>(*buyouts.db))
            , characters(std::make_unique<CharacterRepo>(*buyouts.db))
        {
            if (!stashes->ensureSchema() || !characters->ensureSchema()) {
                qFatal("Failed to create planning fixture schema");
            }
        }

        void seedStash(const QString &id, const QString &name, unsigned index)
        {
            poe::StashTab stash;
            stash.id = id;
            stash.name = name;
            stash.type = "PremiumStash";
            stash.index = index;
            if (!saveStashFixture(*stashes, stash, "pc", "Standard")) {
                qFatal("Failed to seed planning stash");
            }
        }

        void seedCharacter(const QString &id,
                           const QString &name,
                           const std::optional<QString> equipped_item_id = {},
                           const QString &league = "Standard")
        {
            poe::Character character{};
            character.id = id;
            character.name = name;
            character.realm = "pc";
            character.class_ = "Marauder";
            character.league = league;
            character.level = 90;
            character.experience = 1;
            if (equipped_item_id) {
                poe::Item item{};
                item.id = *equipped_item_id;
                item.name = "";
                item.typeLine = "Divine Orb";
                item.baseType = "Divine Orb";
                character.equipment = std::vector{item};
            }
            if (!characters->saveCharacterList({character})
                || !saveCharacterFixture(*characters, character)) {
                qFatal("Failed to seed planning character");
            }
        }

        BuyoutManagerFixture buyouts;
        std::unique_ptr<StashRepo> stashes;
        std::unique_ptr<CharacterRepo> characters;
    };

    using PlanRows = QList<QHash<QString, QVariant>>;

    PlanRows readPlanRows(const QString &filename)
    {
        QXlsx::Document document(filename);
        if (!document.load() || !document.selectSheet("plan")) {
            return {};
        }
        QHash<int, QString> headers;
        const QXlsx::CellRange dimension = document.dimension();
        for (int column = 1; column <= dimension.lastColumn(); ++column) {
            headers[column] = document.read(1, column).toString();
        }

        PlanRows rows;
        for (int row = 2; row <= dimension.lastRow(); ++row) {
            QHash<QString, QVariant> values;
            for (int column = 1; column <= dimension.lastColumn(); ++column) {
                values[headers[column]] = document.read(row, column);
            }
            rows.push_back(std::move(values));
        }
        return rows;
    }

    // Returns a copy (QHash is implicitly shared, so this is cheap): the
    // column and value arguments are usually temporaries, and a returned
    // reference would leave GCC unable to tell that it points into rows.
    QHash<QString, QVariant> findPlanRow(const PlanRows &rows,
                                         const QString &column,
                                         const QString &value)
    {
        const auto found = std::ranges::find_if(rows, [&](const auto &row) {
            return row.value(column).toString() == value;
        });
        if (found == rows.end()) {
            qFatal("Plan row not found: %s=%s", qPrintable(column), qPrintable(value));
        }
        return *found;
    }

    void editPlanRow(const QString &filename,
                     const QString &match_column,
                     const QString &match_value,
                     const QHash<QString, QVariant> &changes)
    {
        QXlsx::Document document(filename);
        QVERIFY(document.load());
        QVERIFY(document.selectSheet("plan"));
        QHash<QString, int> columns;
        const QXlsx::CellRange dimension = document.dimension();
        for (int column = 1; column <= dimension.lastColumn(); ++column) {
            columns[document.read(1, column).toString()] = column;
        }
        QVERIFY(columns.contains(match_column));
        int matching_row = 0;
        for (int row = 2; row <= dimension.lastRow(); ++row) {
            if (document.read(row, columns.value(match_column)).toString() == match_value) {
                matching_row = row;
                break;
            }
        }
        QVERIFY(matching_row > 0);
        for (auto change = changes.constBegin(); change != changes.constEnd(); ++change) {
            QVERIFY(columns.contains(change.key()));
            QVERIFY(document.write(matching_row, columns.value(change.key()), change.value()));
        }
        QVERIFY(document.save());
    }

} // namespace

class LegacyBuyoutImporterTest : public QObject
{
    Q_OBJECT

private slots:
    void createsEditablePlanWithRevisedMatchingDefaults();
    void matchesRenamedCharacterByEquippedItems();
    void characterMatchingIsLeagueScoped();
    void formulaLikeLabelsStayText();
    void importsVersion5StampedFiles();
    void appliesEditedPlanTransactionallyAndIsIdempotent();
    void manualPricesSetAfterPlanningAreProtected();
    void inheritedCopyDoesNotBlockTheImport();
    void gamePricesAreNotImported();
    void importedInheritedBuyoutsLoseTheFlag();
    void skipRowEditsDoNotAbortTheImport();
    void blankPlanRowsAreIgnored();
    void rejectsInvalidPlanBeforeWriting();
    void rollsBackAndReportsDatabaseErrors();
    void refusesPreVersion4Files();
    void planFailsWhenCurrentBuyoutsCannotBeRead();
};

void LegacyBuyoutImporterTest::createsEditablePlanWithRevisedMatchingDefaults()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");

    Buyout manual = makeChaosBuyout(99.0);
    manual.source = Buyout::BUYOUT_SOURCE_MANUAL;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(manual, "item-a", "0123456789", ItemLocationType::STASH, false),
             BuyoutSaveResult::Saved);
    Buyout automatic = makeChaosBuyout(98.0);
    automatic.source = Buyout::BUYOUT_SOURCE_AUTO;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(automatic, "item-b", "0123456789", ItemLocationType::STASH, false),
             BuyoutSaveResult::Saved);

    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path, plan_path);

    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.total, 5);
    QCOMPARE(report.matched, 4);
    QCOMPARE(report.ambiguous, 2);
    QCOMPARE(report.orphaned, 1);
    QCOMPARE(report.skipped, 1); // one malformed item, not the whole stash row (R1-6)
    QCOMPARE(report.rows, 7);

    const PlanRows rows = readPlanRows(plan_path);
    QCOMPARE(rows.size(), 7);

    const auto &manual_row = findPlanRow(rows, "item_id", "item-a");
    QCOMPARE(manual_row.value("action").toString(), QString("skip"));
    QCOMPARE(manual_row.value("reason").toString(), QString("existing-manual"));
    QCOMPARE(manual_row.value("location_id").toString(), QString("0123456789"));
    QCOMPARE(manual_row.value("old_tab_label").toString(), QString("Old Priced"));
    QCOMPARE(manual_row.value("current_tab_name").toString(), QString("Renamed Priced"));
    QCOMPARE(manual_row.value("value").toDouble(), 5.0);
    QCOMPARE(manual_row.value("item_id").metaType(), QMetaType::fromType<QString>());

    const auto &automatic_row = findPlanRow(rows, "item_id", "item-b");
    QCOMPARE(automatic_row.value("action").toString(), QString("import"));
    QVERIFY(automatic_row.value("reason").toString().startsWith("ambiguous-"));
    QCOMPARE(automatic_row.value("existing_source").toString(), QString("auto"));

    const auto &character_row = findPlanRow(rows, "item_id", "item-character");
    QCOMPARE(character_row.value("action").toString(), QString("skip"));
    QCOMPARE(character_row.value("reason").toString(), QString("inherited"));
    QCOMPARE(character_row.value("location_id").toString(), QString("character-id-bob"));

    const auto &orphan_row = findPlanRow(rows, "legacy_hash", "orphan-hash");
    QCOMPARE(orphan_row.value("action").toString(), QString("skip"));
    QCOMPARE(orphan_row.value("reason").toString(), QString("orphaned"));
    QVERIFY(orphan_row.value("item_id").toString().isEmpty());

    const auto &second_stash_row = findPlanRow(rows, "location_id", "fedcba9876");
    QCOMPARE(second_stash_row.value("action").toString(), QString("skip"));
    QCOMPARE(second_stash_row.value("reason").toString(), QString("needs-attention"));
    QCOMPARE(second_stash_row.value("old_tab_label").toString(), QString("Old Priced"));
    QVERIFY(second_stash_row.value("current_tab_name").toString().isEmpty());

    QXlsx::Document document(plan_path);
    QVERIFY(document.load());
    QVERIFY(document.sheetNames().contains("plan"));
    QVERIFY(document.selectSheet("meta"));
    QCOMPARE(document.read(1, 1).toString(), QString("format_version"));
    QCOMPARE(document.read(1, 2).toString(), QString("1"));
    QCOMPARE(document.read(4, 2).toString(), QString("4"));
}

void LegacyBuyoutImporterTest::matchesRenamedCharacterByEquippedItems()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedCharacter("character-id-robert", "Robert", "item-character");

    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path, plan_path);

    QVERIFY2(report.success, qPrintable(report.error));
    const PlanRows rows = readPlanRows(plan_path);
    const auto &character_row = findPlanRow(rows, "item_id", "item-character");
    QCOMPARE(character_row.value("location_id").toString(), QString("character-id-robert"));
    QCOMPARE(character_row.value("current_character").toString(), QString("Robert"));
    QCOMPARE(character_row.value("reason").toString(), QString("inherited"));

    const auto &location_row = findPlanRow(rows, "legacy_hash", "character:Bob");
    QCOMPARE(location_row.value("location_id").toString(), QString("character-id-robert"));
    QCOMPARE(location_row.value("reason").toString(), QString("character-matched-by-items"));
}

void LegacyBuyoutImporterTest::characterMatchingIsLeagueScoped()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    // Robert holds Bob's old equipped item, but in another league — the
    // equipped-item match must not propose a character the active league
    // never surfaces (R2-6).
    PlanningFixture destination;
    destination.seedCharacter("character-id-robert", "Robert", "item-character", "Hardcore");

    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path, plan_path);

    QVERIFY2(report.success, qPrintable(report.error));
    const PlanRows rows = readPlanRows(plan_path);
    const auto &location_row = findPlanRow(rows, "legacy_hash", "character:Bob");
    QCOMPARE(location_row.value("reason").toString(), QString("orphaned"));
    QVERIFY(location_row.value("location_id").toString().isEmpty());
}

void LegacyBuyoutImporterTest::formulaLikeLabelsStayText()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");

    // Rename the priced tab to a separator-style label; QXlsx's generic
    // write() would turn the leading '=' into a formula and the plan
    // would show #NAME? instead of the label (R2-7).
    {
        const QString connection_name = "legacy-formula-label:"
                                        + QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connection_name);
            db.setDatabaseName(source_path);
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));
            QSqlQuery query(db);
            query.prepare("UPDATE data SET value = ? WHERE key = 'tab_buyouts'");
            query.bindValue(0,
                            QString(
                                R"json({"stash:=== SELL ===":{"value":8,"last_update":1700000003,)json"
                                R"json("type":"b/o","currency":"chaos","source":"manual",)json"
                                R"json("inherited":false}})json"));
            QVERIFY(query.exec());
            query.prepare("UPDATE tabs SET value = ? WHERE type = 0");
            query.bindValue(
                0,
                QString(R"json([{"id":"stash-a","name":"=== SELL ===","type":"PremiumStash",)json"
                        R"json("index":0,"metadata":{"colour":"7f7f7f"}}])json"));
            QVERIFY(query.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(connection_name);
    }

    PlanningFixture destination;
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path, plan_path);

    QVERIFY2(report.success, qPrintable(report.error));
    const PlanRows rows = readPlanRows(plan_path);
    const auto &tab_row = findPlanRow(rows, "legacy_hash", "stash:=== SELL ===");
    QCOMPARE(tab_row.value("old_tab_label").toString(), QString("=== SELL ==="));
}

void LegacyBuyoutImporterTest::importsVersion5StampedFiles()
{
    // Earlier releases' MigrateBuyouts (removed in #290) stamped
    // db_version 5 into upgraded legacy files while leaving the
    // v4-generation hash keys intact (R1-1), so a 5-stamped file must
    // match and import exactly like a 4-stamped one — this is the file
    // shape most <=0.15 upgraders actually have. Asserting
    // matched/orphaned counts (not just totals) keeps a regression in
    // LegacyItem::hash() or the version gate visible (R2-8).
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v5.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "5");

    PlanningFixture destination;
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport plan = importer.createPlan(source_path, plan_path);

    QVERIFY2(plan.success, qPrintable(plan.error));
    QCOMPARE(plan.total, 5);
    QCOMPARE(plan.matched, 4);
    QCOMPARE(plan.ambiguous, 2);
    QCOMPARE(plan.orphaned, 1);
    QCOMPARE(plan.rows, 7);
    QVERIFY(QFileInfo::exists(plan_path));

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.imported, 6);
    QCOMPARE(report.errors, 0);
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().size(), std::size_t(3));
    QCOMPARE(destination.buyouts.repo->loadLocationBuyouts().value().size(), std::size_t(3));
}

void LegacyBuyoutImporterTest::appliesEditedPlanTransactionallyAndIsIdempotent()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    // An orphan becomes importable when the user supplies the target ids.
    editPlanRow(plan_path,
                "legacy_hash",
                "orphan-hash",
                {{"action", "import"},
                 {"item_id", "recovered-item"},
                 {"location_id", "0123456789"},
                 {"location_type", "stash"}});

    const LegacyBuyoutApplyReport first = importer.applyPlan(plan_path);
    QVERIFY2(first.success, qPrintable(first.error));
    QCOMPARE(first.imported, 5);
    QCOMPARE(first.already_present, 0);
    QCOMPARE(first.skipped, 2);
    QCOMPARE(first.errors, 0);
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().size(), std::size_t(3));
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().at("recovered-item").value, 7.0);
    QCOMPARE(destination.buyouts.repo->loadLocationBuyouts().value().size(), std::size_t(2));
    QVERIFY(!destination.buyouts.repo->loadLocationBuyouts().value().contains("fedcba9876"));

    const PlanRows applied_rows = readPlanRows(plan_path);
    QCOMPARE(findPlanRow(applied_rows, "item_id", "recovered-item").value("outcome").toString(),
             QString("imported"));

    const LegacyBuyoutApplyReport second = importer.applyPlan(plan_path);
    QVERIFY2(second.success, qPrintable(second.error));
    QCOMPARE(second.imported, 0);
    QCOMPARE(second.already_present, 5);
    QCOMPARE(second.skipped, 2);
    QCOMPARE(second.errors, 0);
}

void LegacyBuyoutImporterTest::manualPricesSetAfterPlanningAreProtected()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    // A manual price written between plan and apply (a hand edit, or the
    // auto-refresh timer) must be protected at write time — the plan's
    // prefill could not have known about it (R2-1).
    Buyout manual = makeChaosBuyout(99.0);
    manual.source = Buyout::BUYOUT_SOURCE_MANUAL;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(manual, "item-a", "0123456789", ItemLocationType::STASH, false),
             BuyoutSaveResult::Saved);

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.protected_manual, 1);
    QCOMPARE(report.imported, 3);
    QCOMPARE(report.errors, 0);
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().at("item-a").value, 99.0);

    const PlanRows rows = readPlanRows(plan_path);
    QCOMPARE(findPlanRow(rows, "item_id", "item-a").value("outcome").toString(),
             QString("skipped-existing-manual"));

    // A plan generated with the manual row visible (existing_source =
    // manual, prefilled skip) may overwrite it once the user explicitly
    // flips the row to import.
    const QString second_plan = source_dir.filePath("buyout-plan-2.xlsx");
    QVERIFY(importer.createPlan(source_path, second_plan).success);
    const PlanRows second_rows = readPlanRows(second_plan);
    QCOMPARE(findPlanRow(second_rows, "item_id", "item-a").value("reason").toString(),
             QString("existing-manual"));
    editPlanRow(second_plan, "item_id", "item-a", {{"action", "import"}});

    const LegacyBuyoutApplyReport second = importer.applyPlan(second_plan);
    QVERIFY2(second.success, qPrintable(second.error));
    QCOMPARE(second.protected_manual, 0);
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().at("item-a").value, 5.0);
}

// #293: a copy of a tab's price that an older build stored on an item,
// with a manual source, is not a price the user set on the item. The plan
// does not count it as an existing manual price and proposes the import,
// and applying the plan writes the legacy price over it; a real manual
// price written after planning is still protected.
void LegacyBuyoutImporterTest::inheritedCopyDoesNotBlockTheImport()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    Buyout copy = makeChaosBuyout(42.0);
    copy.source = Buyout::BUYOUT_SOURCE_MANUAL;
    copy.inherited = true;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(copy, "item-a", "0123456789", ItemLocationType::STASH, true),
             BuyoutSaveResult::Saved);

    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);
    const PlanRows rows = readPlanRows(plan_path);
    const auto &row = findPlanRow(rows, "item_id", "item-a");
    QCOMPARE(row.value("action").toString(), QString("import"));
    QVERIFY(row.value("existing_source").toString().isEmpty());

    // A real manual price on another matched item, set after planning, is
    // still protected at write time.
    Buyout manual = makeChaosBuyout(99.0);
    manual.source = Buyout::BUYOUT_SOURCE_MANUAL;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(manual, "item-b", "0123456789", ItemLocationType::STASH, true),
             BuyoutSaveResult::Saved);

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.protected_manual, 1);
    const auto stored = destination.buyouts.repo->loadItemBuyouts().value();
    QCOMPARE(stored.at("item-a").value, 5.0);
    QVERIFY(!stored.at("item-a").inherited);
    QCOMPARE(stored.at("item-b").value, 99.0);
}

// #293: a price an old build read from a note or a tab's name is read
// again from the game at the next refresh; it is never stored. The plan
// does not offer such a row (skip, reason "game"), and a row the user flips
// to import is still not written, not even over a manual price. Rows set
// in Acquisition are planned and imported as before.
void LegacyBuyoutImporterTest::gamePricesAreNotImported()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);
    markLegacyPricesFromTheGame(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    Buyout manual = makeChaosBuyout(99.0);
    manual.source = Buyout::BUYOUT_SOURCE_MANUAL;
    QCOMPARE(destination.buyouts.repo
                 ->saveItemBuyout(manual, "item-b", "0123456789", ItemLocationType::STASH, false),
             BuyoutSaveResult::Saved);

    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);
    const PlanRows rows = readPlanRows(plan_path);
    const auto tabRow = [](const PlanRows &rows, const QString &location_id) {
        const auto found = std::ranges::find_if(rows, [&](const auto &row) {
            return row.value("target_type").toString() == "location"
                   && row.value("location_id").toString() == location_id;
        });
        if (found == rows.end()) {
            qFatal("Tab row not found: %s", qPrintable(location_id));
        }
        return *found;
    };
    for (const QString &item_id : {QString("item-a"), QString("item-b")}) {
        const auto row = findPlanRow(rows, "item_id", item_id);
        QCOMPARE(row.value("action").toString(), QString("skip"));
        QCOMPARE(row.value("reason").toString(), QString("game"));
    }
    QCOMPARE(tabRow(rows, "0123456789").value("action").toString(), QString("skip"));
    QCOMPARE(tabRow(rows, "0123456789").value("reason").toString(), QString("game"));
    // The manual rows are planned as before.
    QCOMPARE(tabRow(rows, "character-id-bob").value("action").toString(), QString("import"));
    QCOMPARE(findPlanRow(rows, "item_id", "item-character").value("reason").toString(),
             QString("inherited"));

    // The user flips every game row to import, item-b's over its manual
    // price as well.
    editPlanRow(plan_path, "item_id", "item-a", {{"action", "import"}});
    editPlanRow(plan_path,
                "item_id",
                "item-b",
                {{"action", "import"}, {"existing_source", "manual"}});
    editPlanRow(plan_path, "legacy_hash", "stash:Old Priced", {{"action", "import"}});

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.errors, 0);
    QCOMPARE(gameRows(*destination.buyouts.db), 0);
    QCOMPARE(destination.buyouts.repo->loadItemBuyouts().value().at("item-b").value, 99.0);
    QVERIFY(!destination.buyouts.repo->loadItemBuyouts().value().contains("item-a"));
    QCOMPARE(destination.buyouts.repo->loadLocationBuyouts().value().at("character-id-bob").value,
             9.0);
    QVERIFY(!destination.buyouts.repo->loadLocationBuyouts().value().contains("0123456789"));
    QCOMPARE(report.imported, 1);

    const PlanRows applied = readPlanRows(plan_path);
    // Each flipped row is refused with the reason, not skipped as planned.
    for (const auto &row : {findPlanRow(applied, "item_id", "item-a"),
                            findPlanRow(applied, "item_id", "item-b"),
                            tabRow(applied, "0123456789")}) {
        QCOMPARE(row.value("action").toString(), QString("import"));
        QCOMPARE(row.value("outcome").toString(), QString("skipped"));
        QVERIFY(row.value("error").toString().contains("read again at the next refresh"));
    }
}

void LegacyBuyoutImporterTest::importedInheritedBuyoutsLoseTheFlag()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    // The character item's legacy buyout is inherited and prefilled skip;
    // flipping it to import must strip the flag so PropagateTabBuyouts
    // cannot destroy the recovered price on the next refresh (R2-2).
    editPlanRow(plan_path, "item_id", "item-character", {{"action", "import"}});

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));

    const auto buyouts = destination.buyouts.repo->loadItemBuyouts().value();
    QCOMPARE(buyouts.at("item-character").value, 6.0);
    QCOMPARE(buyouts.at("item-character").inherited, false);
}

void LegacyBuyoutImporterTest::skipRowEditsDoNotAbortTheImport()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    // Annotating the value cell of a row the user opted out of must not
    // fail validation for the whole plan (R2-4).
    editPlanRow(plan_path, "legacy_hash", "orphan-hash", {{"value", "n/a"}});

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.imported, 4);
    QCOMPARE(report.skipped, 3);
    QCOMPARE(report.errors, 0);
}

void LegacyBuyoutImporterTest::blankPlanRowsAreIgnored()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    // A row whose contents were cleared in a spreadsheet editor stays
    // inside the sheet dimension; it must be ignored, not treated as a
    // hard error that aborts the plan (R2-5).
    editPlanRow(plan_path,
                "legacy_hash",
                "orphan-hash",
                {{"action", ""},
                 {"target_type", ""},
                 {"item_id", ""},
                 {"location_id", ""},
                 {"value", ""}});

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY2(report.success, qPrintable(report.error));
    QCOMPARE(report.imported, 4);
    QCOMPARE(report.skipped, 2);
    QCOMPARE(report.errors, 0);
}

void LegacyBuyoutImporterTest::rejectsInvalidPlanBeforeWriting()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);
    editPlanRow(plan_path, "item_id", "item-a", {{"currency", "not-a-currency"}});

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY(!report.success);
    QVERIFY(report.error.contains("validation"));
    QCOMPARE(report.imported, 0);
    QCOMPARE(report.errors, 1);
    QVERIFY(destination.buyouts.repo->loadItemBuyouts().value().empty());
    QVERIFY(destination.buyouts.repo->loadLocationBuyouts().value().empty());

    const PlanRows rows = readPlanRows(plan_path);
    const auto &invalid = findPlanRow(rows, "item_id", "item-a");
    QCOMPARE(invalid.value("outcome").toString(), QString("error"));
    QVERIFY(invalid.value("error").toString().contains("invalid"));
    const auto &valid = findPlanRow(rows, "item_id", "item-b");
    QCOMPARE(valid.value("outcome").toString(), QString("not-applied"));
}

void LegacyBuyoutImporterTest::rollsBackAndReportsDatabaseErrors()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");
    reviseLegacyDatabaseForPlan(source_path);

    PlanningFixture destination;
    destination.seedStash("0123456789", "Renamed Priced", 0);
    destination.seedCharacter("character-id-bob", "Bob");
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    QVERIFY(importer.createPlan(source_path, plan_path).success);

    QSqlQuery trigger(*destination.buyouts.db);
    QVERIFY(trigger.exec(R"(
        CREATE TRIGGER fail_item_b
        BEFORE INSERT ON item_buyouts
        WHEN NEW.item_id = 'item-b'
        BEGIN
            SELECT RAISE(ABORT, 'forced import failure');
        END
    )"));

    const LegacyBuyoutApplyReport report = importer.applyPlan(plan_path);
    QVERIFY(!report.success);
    QVERIFY(report.error.contains("forced import failure"));
    QCOMPARE(report.imported, 0);
    QCOMPARE(report.errors, 4);
    QVERIFY(destination.buyouts.repo->loadItemBuyouts().value().empty());
    QVERIFY(destination.buyouts.repo->loadLocationBuyouts().value().empty());

    const PlanRows rows = readPlanRows(plan_path);
    const auto &rolled_back = findPlanRow(rows, "item_id", "item-a");
    QCOMPARE(rolled_back.value("outcome").toString(), QString("error"));
    QVERIFY(rolled_back.value("error").toString().contains("rolled back"));
}

void LegacyBuyoutImporterTest::refusesPreVersion4Files()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v3.db");
    createLegacyDatabase(source_path, "3");

    PlanningFixture destination;
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path,
                                                              source_dir.filePath("plan.xlsx"));

    QVERIFY(!report.success);
    QVERIFY(report.error.contains("db_version 4"));
    QVERIFY(destination.buyouts.repo->loadItemBuyouts().value().empty());
    QVERIFY(destination.buyouts.repo->loadLocationBuyouts().value().empty());
}

// #295: the plan compares each legacy price with the current one, so a
// failed read of the current buyouts must fail the plan instead of
// reading as "no prices yet".
void LegacyBuyoutImporterTest::planFailsWhenCurrentBuyoutsCannotBeRead()
{
    QTemporaryDir source_dir;
    QVERIFY(source_dir.isValid());
    const QString source_path = source_dir.filePath("legacy-v4.db");
    const QString plan_path = source_dir.filePath("buyout-plan.xlsx");
    createLegacyDatabase(source_path, "4");

    PlanningFixture destination;
    QSqlQuery q(*destination.buyouts.db);
    QVERIFY(q.exec("ALTER TABLE item_buyouts RENAME TO item_buyouts_hidden"));
    LegacyBuyoutImporter importer(*destination.buyouts.repo,
                                  *destination.stashes,
                                  *destination.characters,
                                  "pc",
                                  "Standard");
    const LegacyBuyoutPlanReport report = importer.createPlan(source_path, plan_path);

    QVERIFY(!report.success);
    QVERIFY2(report.error.contains("current buyouts"), qPrintable(report.error));
    QVERIFY(!QFileInfo::exists(plan_path));
}

QTEST_GUILESS_MAIN(LegacyBuyoutImporterTest)

#include "tst_legacybuyoutimporter.moc"
