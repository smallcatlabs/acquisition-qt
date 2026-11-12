// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <variant>
#include <vector>

#include <QFuture>
#include <QPromise>
#include <QString>
#include <QStringList>

#include "poe/poeapiclient.h"
#include "ratelimit/fetcherror.h"
#include "util/json_writers.h"
// Only for the unused base-constructor argument the fake is handed; nothing
// here calls into the limiter.
#include "ratelimit/ratelimiter.h"

// A controllable stand-in for the typed API facade (testing-plan item 5).
//
// Worker tests say "when asked for stash X, return this tab / this error"
// instead of crafting response bytes: every call records the domain arguments
// the worker asked with — realm, league, stash id, substash id, character
// name — and returns a future that stays pending until the test settles it.
// That keeps delivery order under the test's control, which is what the
// worker's sequencing pins depend on, while removing JSON and URLs from the
// worker suite entirely. Request building and endpoint labels are pinned
// against the real facade in tst_poeapiclient.
//
// Deliveries name a call by its domain identity, never a global submission
// index (phase 5): the pending-call finders below match only DELIVERABLE calls
// — unsettled and not stopped — so a test that fetches the same stash id in two
// successive updates settles each update's call in turn with no per-id
// scripting. Exactly one deliverable call may match an identity; two is a
// within-update duplicate fetch (F49 tripwire). A stopped old-update straggler
// that shares a new call's identity is a legitimate cross-update overlap, not a
// duplicate: it is excluded from delivery and settled Canceled in bulk via
// settleStoppedStragglers().
//
// Each call is settled exactly once: not because a QFuture forbids more (it is
// a multi-result container — addResult() may be called repeatedly), but because
// settling a call clears its slot and a second attempt qFatals. This mirrors
// the worker's own one-shot completion. The legacy signal boundary this
// replaced could re-emit the same reply; nothing here reproduces that,
// deliberately.
class FakePoeApiClient : public PoeApiClient
{
public:
    // The payload the base's Result<T> future carries. Named differently on
    // purpose: PoeApiClient::Result<T> is the future, Value<T> is what it
    // resolves to.
    template<typename T>
    using Value = std::expected<T, RateLimit::FetchError>;

    // The limiter is never touched: every method below is overridden. It
    // exists only to satisfy the base constructor.
    explicit FakePoeApiClient(RateLimiter &unused)
        : PoeApiClient(unused)
    {}

    struct Call
    {
        enum class Kind { ListStashes, GetStash, ListCharacters, GetCharacter, LegacyStashIndex };

        Kind kind{};
        QString account{};
        QString realm{};
        QString league{};
        QString stash_id{};
        QString substash_id{};
        QString name{};
        // The stop_token the worker passed with this call. From phase 5 the
        // worker threads one per-update token through every call in the update;
        // the identity/token finders below expose it so tests can assert token
        // sharing, first-failure stopping, and cross-update distinctness
        // (W-TOKEN/W-IDENTITY).
        std::stop_token token{};
    };

    // --- the facade ------------------------------------------------------

    PoeApiClient::Result<poe::StashListWrapper> listStashes(const QString &realm,
                                                            const QString &league,
                                                            std::stop_token token = {}) override
    {
        Call call{.kind = Call::Kind::ListStashes, .realm = realm, .league = league, .token = token};
        return record<poe::StashListWrapper>(std::move(call));
    }

    PoeApiClient::Result<poe::StashPayload> getStash(const QString &realm,
                                                     const QString &league,
                                                     const QString &stash_id,
                                                     const QString &substash_id = {},
                                                     std::stop_token token = {}) override
    {
        Call call{.kind = Call::Kind::GetStash,
                  .realm = realm,
                  .league = league,
                  .stash_id = stash_id,
                  .substash_id = substash_id,
                  .token = token};
        return record<poe::StashPayload>(std::move(call));
    }

    PoeApiClient::Result<poe::CharacterListWrapper> listCharacters(
        const QString &realm, std::stop_token token = {}) override
    {
        Call call{.kind = Call::Kind::ListCharacters, .realm = realm, .token = token};
        return record<poe::CharacterListWrapper>(std::move(call));
    }

    PoeApiClient::Result<poe::CharacterPayload> getCharacter(const QString &realm,
                                                             const QString &name,
                                                             std::stop_token token = {}) override
    {
        Call call{.kind = Call::Kind::GetCharacter, .realm = realm, .name = name, .token = token};
        return record<poe::CharacterPayload>(std::move(call));
    }

    PoeApiClient::Result<poe::WebStashListWrapper> getLegacyStashIndex(
        const QString &account,
        const QString &realm,
        const QString &league,
        std::stop_token token = {}) override
    {
        Call call{.kind = Call::Kind::LegacyStashIndex,
                  .account = account,
                  .realm = realm,
                  .league = league,
                  .token = token};
        return record<poe::WebStashListWrapper>(std::move(call));
    }

    // --- inspection ------------------------------------------------------

    // Total calls ever recorded, settled or not. This is a submission count,
    // not a request identity: a test may assert "N requests have been issued
    // so far" (no over-fetching, no premature next call), but it must address
    // an individual call by its domain identity below, never by this index.
    size_t callCount() const { return m_pending.size(); }
    const Call &call(size_t i) const { return m_pending.at(i).call; }

    // --- identity-based lookup (phase 5) ---------------------------------
    //
    // Tests name a call by what the worker asked for — realm/league, stash id
    // + substash id, or character name — never by a global submission index.
    // With serial submission exactly one call is pending at a time; once
    // phase-5 batching lands, several are, and the identity selects which one
    // a delivery settles. Exactly one *pending* (unsettled) call must match an
    // identity: none means the worker never issued it, more than one means two
    // identical requests are in flight at once — the F49 duplicate-fetch
    // tripwire, not something a test may silently settle.

    // Calls recorded but not yet settled — including stopped stragglers, which
    // still owe a settlement. A fixture proves this is zero at teardown.
    size_t pendingCount() const
    {
        size_t n = 0;
        for (const auto &slot : m_pending) {
            if (slot.reject) {
                ++n;
            }
        }
        return n;
    }

    // Unsettled calls whose recorded token has been stopped: old-update
    // stragglers awaiting deterministic Canceled settlement. The active update
    // never delivers to these (the identity finders exclude them).
    size_t stoppedStragglerCount() const
    {
        size_t n = 0;
        for (const auto &slot : m_pending) {
            if (slot.reject && slot.call.token.stop_requested()) {
                ++n;
            }
        }
        return n;
    }

    bool hasPendingStashList(const QString &realm, const QString &league) const
    {
        return countPending(Call::Kind::ListStashes,
                            [&](const Call &c) { return c.realm == realm && c.league == league; })
               == 1;
    }

    bool hasPendingCharacterList(const QString &realm) const
    {
        return countPending(Call::Kind::ListCharacters,
                            [&](const Call &c) { return c.realm == realm; })
               == 1;
    }

    bool hasPendingStash(const QString &stash_id, const QString &substash_id = {}) const
    {
        return countPending(Call::Kind::GetStash,
                            [&](const Call &c) {
                                return c.stash_id == stash_id && c.substash_id == substash_id;
                            })
               == 1;
    }

    bool hasPendingCharacter(const QString &name) const
    {
        return countPending(Call::Kind::GetCharacter, [&](const Call &c) { return c.name == name; })
               == 1;
    }

    // The index of the single pending call matching an identity, for the
    // delivery helpers to settle. qFatal unless exactly one matches.
    size_t pendingStashList(const QString &realm, const QString &league) const
    {
        return findPending("list-stashes", Call::Kind::ListStashes, [&](const Call &c) {
            return c.realm == realm && c.league == league;
        });
    }

    size_t pendingCharacterList(const QString &realm) const
    {
        return findPending("list-characters", Call::Kind::ListCharacters, [&](const Call &c) {
            return c.realm == realm;
        });
    }

    size_t pendingStash(const QString &stash_id, const QString &substash_id = {}) const
    {
        return findPending("get-stash", Call::Kind::GetStash, [&](const Call &c) {
            return c.stash_id == stash_id && c.substash_id == substash_id;
        });
    }

    size_t pendingCharacter(const QString &name) const
    {
        return findPending("get-character", Call::Kind::GetCharacter, [&](const Call &c) {
            return c.name == name;
        });
    }

    // --- stopped-straggler lookup, by identity (never a global index) --------
    //
    // The index of the unique STOPPED straggler (token stopped, still unsettled)
    // matching an identity, so a test can settle an old update's straggler with a
    // SUCCESS value — the W-IDENTITY case (verification §5) where a successful old
    // reply resumes while a subsequent update is active. This is the mirror image
    // of the deliverable finders above, which deliberately EXCLUDE stopped
    // stragglers (so a new update's same-identity call is the one delivery
    // settles). qFatal unless exactly one stopped straggler matches.
    size_t stoppedStash(const QString &stash_id, const QString &substash_id = {}) const
    {
        return findStopped("get-stash", Call::Kind::GetStash, [&](const Call &c) {
            return c.stash_id == stash_id && c.substash_id == substash_id;
        });
    }

    size_t stoppedCharacter(const QString &name) const
    {
        return findStopped("get-character", Call::Kind::GetCharacter, [&](const Call &c) {
            return c.name == name;
        });
    }

    // --- lane-local submission order, by identity (never a global index) ------
    //
    // The order in which each lane's calls were submitted, expressed as the
    // calls' own identities. A test pins D6/P-ORDER source traversal order with
    // these instead of indexing into the global call history, which would bake
    // in unrelated cross-lane positions the harness forbids as identity.

    // Every top-level GetStash (empty substash) stash id, in submission order.
    QStringList stashFetchOrder() const
    {
        QStringList ids;
        for (const auto &slot : m_pending) {
            if (slot.call.kind == Call::Kind::GetStash && slot.call.substash_id.isEmpty()) {
                ids.append(slot.call.stash_id);
            }
        }
        return ids;
    }

    // Every GetCharacter name, in submission order.
    QStringList characterFetchOrder() const
    {
        QStringList names;
        for (const auto &slot : m_pending) {
            if (slot.call.kind == Call::Kind::GetCharacter) {
                names.append(slot.call.name);
            }
        }
        return names;
    }

    // Every reply-discovered child (non-empty substash) of one parent stash id,
    // by substash id, in submission order. The child batch is its own policy
    // lane, so W-F56-ORDER's source-order guarantee applies to it too; this
    // addresses children by parent/substash identity rather than a global index.
    QStringList substashFetchOrder(const QString &parent_stash_id) const
    {
        QStringList ids;
        for (const auto &slot : m_pending) {
            if (slot.call.kind == Call::Kind::GetStash && slot.call.stash_id == parent_stash_id
                && !slot.call.substash_id.isEmpty()) {
                ids.append(slot.call.substash_id);
            }
        }
        return ids;
    }

    // --- token lookup, by identity (never a global index) --------------------
    //
    // The recorded stop_token of the single deliverable call of an identity, so a
    // test can assert token sharing/stopping while calls are still in flight
    // without addressing them by submission index. Same exactly-one-deliverable
    // rule as the pending* finders.
    std::stop_token tokenForStashList(const QString &realm, const QString &league) const
    {
        return m_pending.at(pendingStashList(realm, league)).call.token;
    }

    std::stop_token tokenForCharacterList(const QString &realm) const
    {
        return m_pending.at(pendingCharacterList(realm)).call.token;
    }

    std::stop_token tokenForStash(const QString &stash_id, const QString &substash_id = {}) const
    {
        return m_pending.at(pendingStash(stash_id, substash_id)).call.token;
    }

    std::stop_token tokenForCharacter(const QString &name) const
    {
        return m_pending.at(pendingCharacter(name)).call.token;
    }

    // --- delivery --------------------------------------------------------

    // Settle call i with a value. T must match the call's payload type; a
    // mismatch is a test bug and aborts loudly rather than silently doing
    // nothing.
    template<typename T>
    void resolve(size_t i, T value)
    {
        settle<T>(i, Value<T>(std::move(value)));
    }

    // Convenience for the two payloads the worker fetches one of. The real
    // facade returns the reply's wire bytes alongside the parse (F62); the
    // fake serializes the typed fixture instead — re-serialization is
    // harmless in tests, it is the production cache that must be faithful.
    void resolveStash(size_t i, poe::StashTab stash)
    {
        QByteArray bytes = json::writeStash(stash);
        resolve(i, poe::StashPayload{.stash = std::move(stash), .bytes = std::move(bytes)});
    }

    void resolveStashList(size_t i, std::vector<poe::StashTab> stashes)
    {
        resolve(i, poe::StashListWrapper{.stashes = std::move(stashes)});
    }

    void resolveCharacter(size_t i, poe::Character character)
    {
        QByteArray bytes = json::writeCharacter(character);
        resolve(i,
                poe::CharacterPayload{.character = std::move(character), .bytes = std::move(bytes)});
    }

    void resolveCharacterList(size_t i, std::vector<poe::Character> characters)
    {
        resolve(i, poe::CharacterListWrapper{.characters = std::move(characters)});
    }

    // Settle call i with a failure of the given kind. Every flavour the
    // network can produce reaches the worker through this one shape.
    void reject(size_t i, RateLimit::FetchError::Kind kind, const QString &message = {})
    {
        Slot &slot = m_pending.at(i);
        if (!slot.reject) {
            qFatal("FakePoeApiClient::reject: call %zu was already settled", i);
        }
        RateLimit::FetchError error;
        error.kind = kind;
        error.message = message;
        auto reject_fn = std::move(slot.reject);
        slot.resolve_type = nullptr;
        slot.reject = nullptr;
        // Clear the throw hook too: it also captures the promise, so leaving it
        // set would keep the shared future state (and any payload) alive past
        // completion, exactly the retention promise.reset() exists to avoid.
        slot.throw_exception = nullptr;
        reject_fn(std::move(error));
        // See settle(): drop the settled promise so its payload is not
        // retained past completion.
        slot.promise.reset();
    }

    // Settle call i with an EXCEPTIONAL future: its shared state carries a
    // std::exception_ptr, so a consumer's co_await (qCoro(future).takeResult())
    // rethrows at the await instead of yielding a value. This is the real
    // R5-1 shape the worker's per-fetch catch-all must contain — an IR4
    // boundary violation that produces a throwing future rather than an
    // expected value (verification §2/§3, W-THROW). Settled exactly once, like
    // every other delivery.
    void throwAt(size_t i)
    {
        Slot &slot = m_pending.at(i);
        if (!slot.reject) {
            qFatal("FakePoeApiClient::throwAt: call %zu was already settled", i);
        }
        auto throw_fn = std::move(slot.throw_exception);
        slot.resolve_type = nullptr;
        slot.reject = nullptr;
        slot.throw_exception = nullptr;
        throw_fn();
        slot.promise.reset();
    }

    // --- pre-completed (ready) futures -----------------------------------
    //
    // Arm the NEXT call of a kind to return an ALREADY-FINISHED future, so its
    // consumer's co_await does not suspend (S1-6) and runs inline during the
    // worker's launch loop. This is how the initialize-before-launch invariant
    // is exercised (IR2/W-INIT): a synchronously ready success or error must
    // not finalize early or corrupt counters that were set before the launch.
    // FIFO per registration; the call is still recorded (callCount and the
    // token are observable) but never pending.
    void preresolveStashList(std::vector<poe::StashTab> stashes)
    {
        armReady<poe::StashListWrapper>(Call::Kind::ListStashes,
                                        poe::StashListWrapper{.stashes = std::move(stashes)});
    }

    void prerejectStashList(RateLimit::FetchError::Kind kind)
    {
        armRejected<poe::StashListWrapper>(Call::Kind::ListStashes, kind);
    }

    // Arm the next stash content (or child) fetch to come back already-finished.
    // Per-kind FIFO: arming several applies to successive GetStash calls in the
    // order they are launched, so a whole ready content batch can be scripted.
    // These drive the content/child variants of the initialize-before-launch
    // invariant (W-INIT): a ready fetch runs its handler inline in the batch
    // launch loop, and must not finalize early or corrupt the counters and
    // parent bookkeeping that were set before the launch.
    void preresolveStash(poe::StashTab stash)
    {
        QByteArray bytes = json::writeStash(stash);
        armReady<poe::StashPayload>(Call::Kind::GetStash,
                                    poe::StashPayload{.stash = std::move(stash),
                                                      .bytes = std::move(bytes)});
    }

    void prerejectStash(RateLimit::FetchError::Kind kind)
    {
        armRejected<poe::StashPayload>(Call::Kind::GetStash, kind);
    }

    // Settle every stopped straggler Canceled, exactly once, and return how
    // many were settled. This is the deterministic straggler path the worker's
    // shared-token cancellation needs: it scans the LIVE slots (never call
    // history by index — a settled slot still carries the now-stopped token and
    // rejecting it again would qFatal), so it is safe to call after any number
    // of updates have shared and stopped their tokens.
    size_t settleStoppedStragglers()
    {
        size_t settled = 0;
        for (size_t i = 0; i < m_pending.size(); ++i) {
            Slot &slot = m_pending[i];
            if (slot.reject && slot.call.token.stop_requested()) {
                reject(i, RateLimit::FetchError::Kind::Canceled);
                ++settled;
            }
        }
        return settled;
    }

private:
    // Type-erased settle hooks, so calls of different payload types can live
    // in one indexed list.
    struct Slot
    {
        Call call;
        // Identifies the payload type, so resolve<T> can reject a mismatch.
        const void *resolve_type{nullptr};
        std::function<void(RateLimit::FetchError)> reject;
        // Settle this call with an exceptional future (throwAt). Typed at
        // record() time, like reject.
        std::function<void()> throw_exception;
        // Holds the typed promise; erased behind the hooks above.
        std::shared_ptr<void> promise;
    };

    // A pre-armed ready outcome for the next call of a kind (preresolve* /
    // prereject*). Applied inside record() to a freshly created promise,
    // finishing it before the future is handed back so the consumer's co_await
    // does not suspend.
    struct Canned
    {
        Call::Kind kind;
        std::function<void(const std::shared_ptr<void> &)> apply;
    };

    template<typename T>
    static const void *typeTag()
    {
        static const char tag{};
        return &tag;
    }

    // A slot is deliverable if it is unsettled AND its recorded token is not
    // stopped. A stopped-but-unsettled slot is an old update's straggler: it
    // still owes a settlement (so it counts as pending), but the active update
    // never delivers to it and it must not collide with a new call that shares
    // its domain identity. Every identity finder below matches deliverable
    // slots only; settleStoppedStragglers() handles the stopped ones.
    static bool deliverable(const Slot &slot)
    {
        return slot.reject && !slot.call.token.stop_requested();
    }

    // Count deliverable calls of a kind whose recorded arguments match.
    template<typename Pred>
    size_t countPending(Call::Kind kind, Pred pred) const
    {
        size_t n = 0;
        for (const auto &slot : m_pending) {
            if (deliverable(slot) && slot.call.kind == kind && pred(slot.call)) {
                ++n;
            }
        }
        return n;
    }

    // The index of the unique deliverable call matching an identity, or a fatal
    // test bug if the match is not exactly one. Two deliverable matches is a
    // genuine within-update duplicate fetch (F49 tripwire); a stopped
    // old-update straggler with the same identity is excluded by deliverable()
    // and so is correctly NOT counted as a duplicate.
    template<typename Pred>
    size_t findPending(const char *what, Call::Kind kind, Pred pred) const
    {
        std::optional<size_t> found;
        for (size_t i = 0; i < m_pending.size(); ++i) {
            const Slot &slot = m_pending[i];
            if (deliverable(slot) && slot.call.kind == kind && pred(slot.call)) {
                if (found) {
                    qFatal("FakePoeApiClient: two deliverable %s calls share an identity — a "
                           "duplicate fetch is in flight within one update (F49 tripwire)",
                           what);
                }
                found = i;
            }
        }
        if (!found) {
            qFatal("FakePoeApiClient: no deliverable %s call matches the requested identity", what);
        }
        return *found;
    }

    // The index of the unique STOPPED straggler (unsettled, token stopped)
    // matching an identity. Unlike findPending it matches non-deliverable
    // stopped slots — an old update's straggler awaiting settlement — so a test
    // can resolve one with a success value (W-IDENTITY). qFatal unless the match
    // is exactly one; a deliverable (unstopped) slot of the same identity is a
    // live call, not a straggler, and is excluded here.
    template<typename Pred>
    size_t findStopped(const char *what, Call::Kind kind, Pred pred) const
    {
        std::optional<size_t> found;
        for (size_t i = 0; i < m_pending.size(); ++i) {
            const Slot &slot = m_pending[i];
            if (slot.reject && slot.call.token.stop_requested() && slot.call.kind == kind
                && pred(slot.call)) {
                if (found) {
                    qFatal("FakePoeApiClient: two stopped %s stragglers share an identity", what);
                }
                found = i;
            }
        }
        if (!found) {
            qFatal("FakePoeApiClient: no stopped %s straggler matches the requested identity", what);
        }
        return *found;
    }

    // Arm the next call of a kind with an already-finished successful value.
    template<typename T>
    void armReady(Call::Kind kind, T value)
    {
        m_canned.push_back(
            {kind, [value = std::move(value)](const std::shared_ptr<void> &erased) mutable {
                 auto promise = std::static_pointer_cast<QPromise<Value<T>>>(erased);
                 promise->addResult(Value<T>(std::move(value)));
                 promise->finish();
             }});
    }

    // Arm the next call of a kind with an already-finished failure.
    template<typename T>
    void armRejected(Call::Kind kind, RateLimit::FetchError::Kind error_kind)
    {
        m_canned.push_back({kind, [error_kind](const std::shared_ptr<void> &erased) {
                                auto promise = std::static_pointer_cast<QPromise<Value<T>>>(erased);
                                RateLimit::FetchError error;
                                error.kind = error_kind;
                                promise->addResult(Value<T>(std::unexpected(std::move(error))));
                                promise->finish();
                            }});
    }

    template<typename T>
    PoeApiClient::Result<T> record(Call call)
    {
        auto promise = std::make_shared<QPromise<Value<T>>>();
        promise->start();
        QFuture<Value<T>> future = promise->future();

        Slot slot;
        slot.call = std::move(call);
        slot.resolve_type = typeTag<T>();
        slot.promise = promise;
        slot.reject = [promise](RateLimit::FetchError error) {
            promise->addResult(Value<T>(std::unexpected(std::move(error))));
            promise->finish();
        };
        slot.throw_exception = [promise]() {
            promise->setException(std::make_exception_ptr(
                std::runtime_error("FakePoeApiClient: exceptional future")));
            promise->finish();
        };

        // A pre-armed ready outcome finishes the promise now, so the returned
        // future is already complete and the call is recorded but never
        // pending (the token stays observable via call()). Match the FIRST
        // canned entry of THIS call's kind — per-kind FIFO, not head-of-line:
        // arming a character outcome must not block a stash call from matching
        // its own stash outcome (needed once 5C arms multiple lanes' ready
        // paths at once).
        for (auto it = m_canned.begin(); it != m_canned.end(); ++it) {
            if (it->kind == slot.call.kind) {
                Canned canned = std::move(*it);
                m_canned.erase(it);
                canned.apply(slot.promise);
                slot.resolve_type = nullptr;
                slot.reject = nullptr;
                slot.throw_exception = nullptr;
                slot.promise.reset();
                break;
            }
        }

        m_pending.push_back(std::move(slot));
        return future;
    }

    template<typename T>
    void settle(size_t i, Value<T> result)
    {
        Slot &slot = m_pending.at(i);
        if (!slot.reject) {
            qFatal("FakePoeApiClient: call %zu was already settled", i);
        }
        if (slot.resolve_type != typeTag<T>()) {
            qFatal("FakePoeApiClient: call %zu was settled with the wrong payload type", i);
        }
        auto promise = std::static_pointer_cast<QPromise<Value<T>>>(slot.promise);
        slot.resolve_type = nullptr;
        slot.reject = nullptr;
        // Clear the throw hook too: it captures the promise, so leaving it set
        // would retain the shared future state (and payload) past completion.
        slot.throw_exception = nullptr;
        promise->addResult(std::move(result));
        promise->finish();
        // Drop the settled promise: the returned QFuture keeps the result
        // alive for as long as a consumer holds it, so nothing here needs to.
        // Holding it would retain every settled payload (whole stash bodies)
        // until fixture teardown and skew phase-5 batch/memory tests.
        slot.promise.reset();
    }

    std::vector<Slot> m_pending;
    std::deque<Canned> m_canned;
};
