// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>

#include <vector>

namespace poe {
    struct Character;
    struct StashTab;
} // namespace poe

// Writers and readers are broken up to avoid an issue on Windows where
// too many templated functions in one file causes a build error and would
// require using /bigobj.

namespace json {

    // Format of the JSON blob the datastore stores in stashes.json_data /
    // characters.json_data.
    //
    //   1  pre-3.29
    //   2  3.29: implicitMods/explicitMods became arrays of poe::ItemMod;
    //            craftedMods/fracturedMods/mutatedMods/desecratedMods removed
    //
    // Since the F62 fix, json_data holds the RAW WIRE BYTES of the reply's
    // stash/character sub-object, so from then on this constant labels GGG's
    // wire format — which is what makes a future blob upgrader possible when
    // the wire format changes (a mechanical transform instead of emptying
    // the cache). Version-2 rows written before the fix are lossy
    // re-serializations of the same shape; the tolerant reader parses both
    // alike, so they share the label.
    //
    // Bump this whenever the blob a current-version reader would produce
    // stops meaning what the poe:: types expect: a wire-format change from
    // GGG, or a renamed/retyped/re-meaning member in the poe:: types. A new
    // field GGG adds does not need a bump — old blobs simply lack it (and
    // with raw storage, new rows keep it even before it is modeled).
    //
    // Rows carry the version they were written with, and readers compare it
    // for EQUALITY, never `<`. A blob written by a newer Acquisition must be
    // refetched rather than misparsed when a user downgrades, so "newer than
    // I understand" has to fail the same way "older" does.
    constexpr int PAYLOAD_VERSION = 2;

    // These writers no longer feed the production cache (F62: the datastore
    // stores received wire bytes, not a re-serialization). They remain for
    // test fixtures, which serialize a typed value where a real reply's
    // bytes would flow — harmless there; it is the production cache that
    // must be faithful.

    QByteArray writeCharacter(const poe::Character &character);

    QByteArray writeStash(const poe::StashTab &stash);

} // namespace json
