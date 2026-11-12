// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>

#include <optional>
#include <vector>

namespace poe {
    struct Character;
    struct CharacterListWrapper;
    struct CharacterPayload;
    struct League;
    struct StashTab;
    struct StashListWrapper;
    struct StashPayload;
    struct WebStashListWrapper;
} // namespace poe

struct OAuthToken;

// Writers and readers are broken up to avoid an issue on Windows where
// too many templated functions in one file causes a build error and would
// require using /bigobj.

namespace json {

    std::optional<OAuthToken> readOAuthToken(const QByteArray &json);

    // The payload readers capture the reply's stash/character sub-object
    // losslessly and parse the typed payload from that same substring, so
    // the bytes and the parse cannot diverge (F62).
    std::optional<poe::CharacterPayload> readCharacterPayload(const QByteArray &json);
    std::optional<poe::CharacterListWrapper> readCharacterListWrapper(const QByteArray &json);
    std::optional<poe::StashPayload> readStashPayload(const QByteArray &json);
    std::optional<poe::StashListWrapper> readStashListWrapper(const QByteArray &json);

    std::optional<poe::WebStashListWrapper> readWebStashListWrapper(const QByteArray &json);

    std::optional<poe::Character> readCharacter(const QByteArray &json);
    std::optional<poe::StashTab> readStash(const QByteArray &json);

    std::optional<std::vector<poe::League>> readLeagueList(const QByteArray &json);

} // namespace json
