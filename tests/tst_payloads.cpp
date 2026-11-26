// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// The poe:: wire types against real replies (#246). tests/fixtures/payloads
// holds the `stash` and `character` objects of replies the API actually
// sent, sampled by export_payloads.py. The application reads replies
// tolerantly, so a key GGG sends that the types do not name is silently
// dropped; here every payload is read strictly, and such a key fails the
// test and is named.

#include <QTest>

#include <QDir>
#include <QFile>

#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "testsuite.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/json_readers.h"

namespace {

    QDir payloadDir()
    {
        return QDir(QFINDTESTDATA("fixtures/payloads"));
    }

    QByteArray readFile(const QString &name)
    {
        QFile file(payloadDir().filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("cannot read payload %s", qPrintable(name));
        }
        return file.readAll();
    }

    // Empty when `json` reads as a T with no unknown key; otherwise glaze's
    // message, which shows the place.
    template<typename T>
    std::string strictReadError(const QByteArray &json)
    {
        static constexpr glz::opts strict{.error_on_unknown_keys = true};
        T value;
        const std::string text = json.toStdString();
        const auto error = glz::read<strict>(value, text);
        return error ? glz::format_error(error, text) : std::string();
    }

} // namespace

class PayloadsTest : public QObject
{
    Q_OBJECT

private slots:
    void corpusHoldsStashesAndCharacters();

    void stashPayloadHasNoUnmodelledKey_data();
    void stashPayloadHasNoUnmodelledKey();
    void characterPayloadHasNoUnmodelledKey_data();
    void characterPayloadHasNoUnmodelledKey();

    // The strict read above is only worth something if it can fail.
    void strictReadRejectsAnUnmodelledKey();
    void strictReadRejectsAnUnmodelledKeyInsideAnItem();
    void applicationReadToleratesAnUnmodelledKey();
    // A key left out of the types on purpose is not unmodelled (#422).
    void strictReadAcceptsAKeyLeftOutOnPurpose();
};

void PayloadsTest::corpusHoldsStashesAndCharacters()
{
    QVERIFY(payloadDir().exists());
    QVERIFY(!payloadDir().entryList({"stash-*.json"}, QDir::Files).isEmpty());
    QVERIFY(!payloadDir().entryList({"character-*.json"}, QDir::Files).isEmpty());
}

void PayloadsTest::stashPayloadHasNoUnmodelledKey_data()
{
    QTest::addColumn<QString>("file");
    for (const QString &name : payloadDir().entryList({"stash-*.json"}, QDir::Files, QDir::Name)) {
        QTest::newRow(qPrintable(name)) << name;
    }
}

void PayloadsTest::stashPayloadHasNoUnmodelledKey()
{
    QFETCH(QString, file);
    const QByteArray json = readFile(file);
    const std::string error = strictReadError<poe::StashTab>(json);
    QVERIFY2(error.empty(), error.c_str());
    // And the reader the application uses for a stored reply accepts it.
    QVERIFY(json::readStash(json).has_value());
}

void PayloadsTest::characterPayloadHasNoUnmodelledKey_data()
{
    QTest::addColumn<QString>("file");
    for (const QString &name :
         payloadDir().entryList({"character-*.json"}, QDir::Files, QDir::Name)) {
        QTest::newRow(qPrintable(name)) << name;
    }
}

void PayloadsTest::characterPayloadHasNoUnmodelledKey()
{
    QFETCH(QString, file);
    const QByteArray json = readFile(file);
    const std::string error = strictReadError<poe::Character>(json);
    QVERIFY2(error.empty(), error.c_str());
    QVERIFY(json::readCharacter(json).has_value());
}

// A real stash payload with one key added to it that no type names.
static QByteArray payloadWithAnUnmodelledKey()
{
    const QStringList stashes = payloadDir().entryList({"stash-*.json"}, QDir::Files, QDir::Name);
    QByteArray json = readFile(stashes.first());
    const qsizetype brace = json.indexOf('{');
    json.insert(brace + 1, R"("notInTheModel": 1,)");
    return json;
}

void PayloadsTest::strictReadRejectsAnUnmodelledKey()
{
    const std::string error = strictReadError<poe::StashTab>(payloadWithAnUnmodelledKey());
    QVERIFY(!error.empty());
    QVERIFY2(error.find("notInTheModel") != std::string::npos, error.c_str());
}

void PayloadsTest::strictReadRejectsAnUnmodelledKeyInsideAnItem()
{
    // The strictness has to reach the nested types, where most keys live.
    for (const QString &name : payloadDir().entryList({"stash-*.json"}, QDir::Files, QDir::Name)) {
        QByteArray json = readFile(name);
        const qsizetype items = json.indexOf("\"items\": [");
        const qsizetype brace = (items < 0) ? -1 : json.indexOf('{', items);
        if (brace < 0) {
            continue; // a tab with no items
        }
        json.insert(brace + 1, R"("notInTheModel": 1,)");
        const std::string error = strictReadError<poe::StashTab>(json);
        QVERIFY2(error.find("notInTheModel") != std::string::npos, error.c_str());
        return;
    }
    QFAIL("no stash payload has an item");
}

void PayloadsTest::applicationReadToleratesAnUnmodelledKey()
{
    QVERIFY(json::readStash(payloadWithAnUnmodelledKey()).has_value());
}

void PayloadsTest::strictReadAcceptsAKeyLeftOutOnPurpose()
{
    // Two keys the types name as skipped (nothing reads them), in shapes
    // their types refused when they were modeled, in a real stash's first
    // item. A key on no list beside them still fails the strict read.
    const QByteArray skipped = R"("foreseeing": "yes", "notableProperties": "text",)";
    for (const QString &name : payloadDir().entryList({"stash-*.json"}, QDir::Files, QDir::Name)) {
        QByteArray json = readFile(name);
        const qsizetype items = json.indexOf("\"items\": [");
        const qsizetype brace = (items < 0) ? -1 : json.indexOf('{', items);
        if (brace < 0) {
            continue; // a tab with no items
        }
        QByteArray unmodelled = json;
        json.insert(brace + 1, skipped);
        const std::string error = strictReadError<poe::StashTab>(json);
        QVERIFY2(error.empty(), error.c_str());

        unmodelled.insert(brace + 1, skipped + R"("notInTheModel": 1,)");
        const std::string unknown = strictReadError<poe::StashTab>(unmodelled);
        QVERIFY2(unknown.find("notInTheModel") != std::string::npos, unknown.c_str());
        return;
    }
    QFAIL("no stash payload has an item");
}

ACQ_TEST_GUILESS_MAIN(PayloadsTest)
#include "tst_payloads.moc"
