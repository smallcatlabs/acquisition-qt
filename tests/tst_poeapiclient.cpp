// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QNetworkRequest>
#include <QUrlQuery>

#include <optional>

#include "fakenetwork.h"
#include "poe/poeapiclient.h"
#include "ratelimit/fetcherror.h"
#include "replytimeout.h"
#include "testsuite.h"
#include "util/networkmanager.h"

// The typed facade (network-redesign spec, the facade section): request
// shapes, the transfer-timeout invariant, and the parse chain. The limiter is
// faked at the future boundary, so nothing here touches the network.
//
// The transfer-timeout pin is the point of this file's existence as much as
// the parsing is: the gate's liveness invariant (D5) depends on every request
// carrying a timeout, and the legacy stash-index request never did (F60).

class PoeApiClientTest : public QObject
{
    Q_OBJECT

private slots:
    void everyRequestCarriesTheTransferTimeout();
    void requestShapesAndEndpointLabels();
    void legacyStashIndexCarriesQueryAndStableEndpoint();
    void successParsesPayload();
    void stashPayloadCarriesTheWireBytes();
    void characterPayloadCarriesTheWireBytes();
    void missingStashSubObjectIsAParseError();
    void missingCharacterSubObjectIsAParseError();
    void fetchErrorsPassThroughUnchanged();
    void garbageBodyBecomesParseError();
};

namespace {

    // A caller of the facade: records the one settlement of its future.
    template<typename T>
    struct Consumer
    {
        int completions = 0;
        std::optional<T> payload;
        std::optional<RateLimit::FetchError> error;

        void attach(QFuture<std::expected<T, RateLimit::FetchError>> future)
        {
            future.then([this](const std::expected<T, RateLimit::FetchError> &result) {
                ++completions;
                if (result) {
                    payload = *result;
                    error.reset();
                } else {
                    payload.reset();
                    error = result.error();
                }
            });
        }

        std::optional<RateLimit::FetchError::Kind> kind() const
        {
            return error ? std::optional(error->kind) : std::nullopt;
        }
    };

    struct Rig
    {
        NetworkManager network;
        FakeRateLimiter limiter{network};
        PoeApiClient api{limiter};
    };

    void drainEvents()
    {
        for (int i = 0; i < 20; ++i) {
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

} // namespace

void PoeApiClientTest::everyRequestCarriesTheTransferTimeout()
{
    // The F60 pin, and the reason the facade owns request building at all: a
    // request with no transfer timeout can hold a gate slot indefinitely if
    // the connection stalls, which stalls the entire hub. EVERY builder is
    // checked, including the legacy one that was built bare before.
    Rig rig;

    rig.api.listStashes("pc", "Standard");
    rig.api.getStash("pc", "Standard", "abc");
    rig.api.listCharacters("pc");
    rig.api.getCharacter("pc", "Someone");
    rig.api.getLegacyStashIndex("account", "pc", "Standard");

    QCOMPARE(rig.limiter.futureCount(), size_t(5));
    for (size_t i = 0; i < rig.limiter.futureCount(); ++i) {
        const QNetworkRequest &request = rig.limiter.pendingFuture(i).request;
        QCOMPARE(request.transferTimeout(), kPoeApiTimeout);
    }
}

void PoeApiClientTest::requestShapesAndEndpointLabels()
{
    // The endpoint label is what the hub keys its policy topology on, so it
    // must be stable per API call and independent of the parameters.
    Rig rig;

    rig.api.listStashes("pc", "Standard");
    QCOMPARE(rig.limiter.pendingFuture(0).endpoint, QString("List Stashes"));
    QCOMPARE(rig.limiter.pendingFuture(0).request.url().toString(),
             QString("https://api.pathofexile.com/stash/Standard"));

    // A non-pc realm is a path segment; the endpoint label does not change.
    rig.api.listStashes("xbox", "Standard");
    QCOMPARE(rig.limiter.pendingFuture(1).endpoint, QString("List Stashes"));
    QCOMPARE(rig.limiter.pendingFuture(1).request.url().toString(),
             QString("https://api.pathofexile.com/stash/xbox/Standard"));

    rig.api.getStash("pc", "Standard", "abc", "def");
    QCOMPARE(rig.limiter.pendingFuture(2).endpoint, QString("Get Stash"));
    QCOMPARE(rig.limiter.pendingFuture(2).request.url().toString(),
             QString("https://api.pathofexile.com/stash/Standard/abc/def"));

    rig.api.listCharacters("pc");
    QCOMPARE(rig.limiter.pendingFuture(3).endpoint, QString("List Characters"));

    rig.api.getCharacter("pc", "Someone");
    QCOMPARE(rig.limiter.pendingFuture(4).endpoint, QString("Get Character"));
    QCOMPARE(rig.limiter.pendingFuture(4).request.url().toString(),
             QString("https://api.pathofexile.com/character/Someone"));
}

void PoeApiClientTest::legacyStashIndexCarriesQueryAndStableEndpoint()
{
    Rig rig;

    rig.api.getLegacyStashIndex("someone", "pc", "Standard");

    const auto &pending = rig.limiter.pendingFuture(0);
    // The endpoint label is the bare URL: the query varies per account, and
    // labelling by the full URL would fragment the hub's topology into one
    // policy per account.
    QCOMPARE(pending.endpoint,
             QString("https://www.pathofexile.com/character-window/get-stash-items"));

    const QUrlQuery query(pending.request.url());
    QCOMPARE(query.queryItemValue("accountName"), QString("someone"));
    QCOMPARE(query.queryItemValue("realm"), QString("pc"));
    QCOMPARE(query.queryItemValue("league"), QString("Standard"));
    QCOMPARE(query.queryItemValue("tabs"), QString("1"));
    QCOMPARE(query.queryItemValue("tabIndex"), QString("0"));
}

void PoeApiClientTest::successParsesPayload()
{
    Rig rig;

    Consumer<poe::WebStashListWrapper> consumer;
    consumer.attach(rig.api.getLegacyStashIndex("someone", "pc", "Standard"));
    QCOMPARE(consumer.completions, 0);

    rig.limiter.resolve(0, R"({"tabs":[{"n":"one","i":0,"id":"0123456789abcdef"}]})");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QVERIFY(consumer.payload.has_value());
    QVERIFY(consumer.payload->tabs.has_value());
    QCOMPARE(consumer.payload->tabs->size(), size_t(1));
    QCOMPARE(consumer.payload->tabs->at(0).i, unsigned(0));
}

void PoeApiClientTest::stashPayloadCarriesTheWireBytes()
{
    // The F62 pin: a stash fetch returns, alongside the parse, the EXACT wire
    // bytes of the reply's "stash" sub-object — including fields the poe::
    // types do not model — and the typed payload is parsed from that same
    // substring. The datastore persists these bytes; a re-serialization of
    // the parsed struct would silently drop every unmodeled field.
    Rig rig;

    Consumer<poe::StashPayload> consumer;
    consumer.attach(rig.api.getStash("pc", "Standard", "abc"));

    const QByteArray sub = R"({"id":"0123456789","name":"Tab A","type":"PremiumStash",)"
                           R"("unmodeledField":{"sometimes":["an","array"]},"items":[]})";
    rig.limiter.resolve(0, "{\"stash\":" + sub + "}");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QVERIFY(consumer.payload.has_value());
    QCOMPARE(consumer.payload->stash.id, QString("0123456789"));
    QCOMPARE(consumer.payload->bytes, sub);
}

void PoeApiClientTest::characterPayloadCarriesTheWireBytes()
{
    Rig rig;

    Consumer<poe::CharacterPayload> consumer;
    consumer.attach(rig.api.getCharacter("pc", "Someone"));

    const QByteArray sub = R"({"id":"charid0001","name":"Someone","realm":"pc",)"
                           R"("class":"Witch","level":1,"experience":0,"unmodeledField":true})";
    rig.limiter.resolve(0, "{\"character\":" + sub + "}");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QVERIFY(consumer.payload.has_value());
    QCOMPARE(consumer.payload->character.name, QString("Someone"));
    QCOMPARE(consumer.payload->bytes, sub);
}

void PoeApiClientTest::missingStashSubObjectIsAParseError()
{
    // A 200 whose reply lacks its "stash" sub-object is classified at the
    // facade as a Parse error (M2 D5/R2-4): it is deterministic per tab, and
    // it must arrive as a typed FetchError — never as a success the worker
    // has to second-guess. The old worker-side "stash is empty" branch held
    // no FetchError and is deleted.
    Rig rig;

    Consumer<poe::StashPayload> consumer;
    consumer.attach(rig.api.getStash("pc", "Standard", "abc"));

    rig.limiter.resolve(0, "{}");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QCOMPARE(consumer.kind(), RateLimit::FetchError::Kind::Parse);
    QCOMPARE(consumer.error->endpoint, QString("Get Stash"));
    QVERIFY(!consumer.payload.has_value());
}

void PoeApiClientTest::missingCharacterSubObjectIsAParseError()
{
    // A null sub-object fails the same way as a missing one.
    Rig rig;

    Consumer<poe::CharacterPayload> consumer;
    consumer.attach(rig.api.getCharacter("pc", "Someone"));

    rig.limiter.resolve(0, R"({"character":null})");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QCOMPARE(consumer.kind(), RateLimit::FetchError::Kind::Parse);
    QCOMPARE(consumer.error->endpoint, QString("Get Character"));
    QVERIFY(!consumer.payload.has_value());
}

void PoeApiClientTest::fetchErrorsPassThroughUnchanged()
{
    // The facade adds parsing, not reclassification: a failure the limiter
    // already described arrives at the caller exactly as it was.
    Rig rig;

    Consumer<poe::StashListWrapper> consumer;
    consumer.attach(rig.api.listStashes("pc", "Standard"));

    rig.limiter.reject(0, RateLimit::FetchError::Kind::RateLimited, "too many requests");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QCOMPARE(consumer.kind(), RateLimit::FetchError::Kind::RateLimited);
    QCOMPARE(consumer.error->message, QString("too many requests"));
    QCOMPARE(consumer.error->endpoint, QString("List Stashes"));
}

void PoeApiClientTest::garbageBodyBecomesParseError()
{
    // A body that cannot be read is a Parse error VALUE, never an exception
    // and never an exceptional future — an exceptional future would rethrow
    // out of the caller's co_await, crossing the value-only boundary (IR4).
    Rig rig;

    Consumer<poe::StashListWrapper> consumer;
    consumer.attach(rig.api.listStashes("pc", "Standard"));

    rig.limiter.resolve(0, "this is not json at all");
    drainEvents();

    QCOMPARE(consumer.completions, 1);
    QCOMPARE(consumer.kind(), RateLimit::FetchError::Kind::Parse);
    QCOMPARE(consumer.error->endpoint, QString("List Stashes"));
    QVERIFY(!consumer.payload.has_value());
}

ACQ_TEST_GUILESS_MAIN(PoeApiClientTest)

#include "tst_poeapiclient.moc"
