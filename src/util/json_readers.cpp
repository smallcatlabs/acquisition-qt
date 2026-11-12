// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "util/json_readers.h"

#include "poe/types/character.h"
#include "poe/types/league.h"
#include "poe/types/stashtab.h"
#include "poe/types/website/webstashtab.h"
#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/oauthtoken.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

// The wire shape of a single stash/character reply, with the payload
// captured as the raw substring (F62): glz::raw_json keeps the exact bytes
// GGG sent, including every field the poe:: types do not model. In a named
// namespace (not the anonymous one) because glaze reflection requires the
// type to have linkage.
namespace json::raw {

    struct StashWrapper
    {
        std::optional<glz::raw_json> stash;
    };

    struct CharacterWrapper
    {
        std::optional<glz::raw_json> character;
    };

} // namespace json::raw

namespace {

    enum class BufferSensitivity
    {
        ordinary,
        credentials,
    };

    template<typename T>
    std::optional<T> read_json_impl(const QByteArray &json, BufferSensitivity sensitivity)
    {
        // Tolerate unknown keys so new fields in GGG's API responses do
        // not break parsing.
        static constexpr glz::opts opts{.error_on_unknown_keys = false};
        T result;
        const std::string_view str{json.constData(), size_t(json.size())};
        const auto err = glz::read<opts>(result, str);
        if (err) {
            const auto type = typeid(T).name();
            if (sensitivity == BufferSensitivity::credentials) {
                // Log the error code and position only: format_error's
                // buffer context would echo the credential (F73).
                spdlog::error("Error reading {} from json: {} at byte {}",
                              type,
                              glz::format_error(err),
                              err.count);
            } else {
                const auto msg = glz::format_error(err, str);
                spdlog::error("Error reading {} from json: {}", type, msg);
            }
            return std::nullopt;
        }
        return result;
    }

    template<typename T>
    std::optional<T> read_json(const QByteArray &json)
    {
        return read_json_impl<T>(json, BufferSensitivity::ordinary);
    }

    template<typename T>
    std::optional<T> read_sensitive_json(const QByteArray &json)
    {
        return read_json_impl<T>(json, BufferSensitivity::credentials);
    }

    // Capture the reply's sub-object losslessly, then parse the typed
    // payload from that same substring, so the stored bytes and the parsed
    // object cannot diverge. The whole read fails — which the facade
    // classifies as FetchError{Parse} — when the sub-object is missing or
    // null (M2 D5/R2-4: a 200 without its payload is deterministic, and it
    // must arrive as a typed error, not as a success the worker has to
    // second-guess) or when the sub-object fails the typed parse.
    template<typename Payload, typename Value, typename Raw>
    std::optional<Payload> read_payload(const QByteArray &json,
                                        const char *what,
                                        std::optional<glz::raw_json> Raw::*member,
                                        Value Payload::*value)
    {
        const auto raw = read_json<Raw>(json);
        if (!raw) {
            return std::nullopt;
        }
        if (!(*raw.*member)) {
            spdlog::error("The reply has no '{}' payload", what);
            return std::nullopt;
        }
        const std::string &str = (*raw.*member)->str;
        const auto view = QByteArray::fromRawData(str.data(), qsizetype(str.size()));
        auto parsed = read_json<Value>(view);
        if (!parsed) {
            return std::nullopt;
        }
        Payload payload;
        payload.*value = std::move(*parsed);
        payload.bytes = QByteArray(str.data(), qsizetype(str.size()));
        return payload;
    }

} // namespace

std::optional<OAuthToken> json::readOAuthToken(const QByteArray &json)
{
    return read_sensitive_json<OAuthToken>(json);
}

std::optional<poe::Character> json::readCharacter(const QByteArray &json)
{
    return read_json<poe::Character>(json);
}

std::optional<poe::CharacterListWrapper> json::readCharacterListWrapper(const QByteArray &json)
{
    return read_json<poe::CharacterListWrapper>(json);
}

std::optional<poe::CharacterPayload> json::readCharacterPayload(const QByteArray &json)
{
    return read_payload(json,
                        "character",
                        &json::raw::CharacterWrapper::character,
                        &poe::CharacterPayload::character);
}

std::optional<std::vector<poe::League>> json::readLeagueList(const QByteArray &json)
{
    return read_json<std::vector<poe::League>>(json);
}

std::optional<poe::StashTab> json::readStash(const QByteArray &json)
{
    return read_json<poe::StashTab>(json);
}

std::optional<poe::StashListWrapper> json::readStashListWrapper(const QByteArray &json)
{
    return read_json<poe::StashListWrapper>(json);
}

std::optional<poe::StashPayload> json::readStashPayload(const QByteArray &json)
{
    return read_payload(json, "stash", &json::raw::StashWrapper::stash, &poe::StashPayload::stash);
}

std::optional<poe::WebStashListWrapper> json::readWebStashListWrapper(const QByteArray &json)
{
    return read_json<poe::WebStashListWrapper>(json);
}
