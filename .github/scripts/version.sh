#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Tom Holz
#
# Prints the version a build of this tree calls itself: project(VERSION)
# followed by app_version_suffix, as app_version_string in CMakeLists.txt.
# Every release workflow reads the version here and nowhere else (RELEASE.md).
#
# On a tag (GITHUB_REF_TYPE=tag) it fails unless the tag is v<version> and the
# version is not a -dev one. The update checker compares the running version
# with release tags (src/util/updatechecker.cpp), so a tag that disagrees with
# the build would tell every user to update to the version they already run.
#
# Prints version= and prerelease= lines, and appends them to $GITHUB_OUTPUT
# when that is set. Runs under macOS's bash 3.2 and BSD sed.
#
# Usage: version.sh [path/to/CMakeLists.txt]

set -euo pipefail

cmakelists=${1:-CMakeLists.txt}

base=$(sed -n '/^project(/,/)/s/^ *VERSION \([0-9][0-9.]*\) *$/\1/p' "$cmakelists")
suffix_lines=$(grep -c '^set(app_version_suffix "[^"]*")$' "$cmakelists" || true)
suffix=$(sed -n 's/^set(app_version_suffix "\([^"]*\)")$/\1/p' "$cmakelists")

if ! printf '%s' "$base" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "::error::Could not read one project(VERSION x.y.z) from $cmakelists (got '$base')"
    exit 1
fi
if [ "$suffix_lines" -ne 1 ]; then
    echo "::error::Expected one set(app_version_suffix \"...\") line in $cmakelists, found $suffix_lines"
    exit 1
fi

version="${base}${suffix}"

if [ "${GITHUB_REF_TYPE:-}" = "tag" ]; then
    if [ "${GITHUB_REF_NAME:-}" != "v$version" ]; then
        echo "::error::Tag ${GITHUB_REF_NAME:-} does not match the version in CMakeLists.txt ($version)"
        exit 1
    fi
    case "$suffix" in
        -dev*)
            echo "::error::$version is a development version; tag a -beta.N or final version (RELEASE.md)"
            exit 1 ;;
    esac
fi

if [ -n "$suffix" ]; then prerelease=true; else prerelease=false; fi

out="version=$version
prerelease=$prerelease"
echo "$out"
if [ -n "${GITHUB_OUTPUT:-}" ]; then
    echo "$out" >> "$GITHUB_OUTPUT"
fi
