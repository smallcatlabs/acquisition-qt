#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Tom Holz
#
# Finds the files a release carries in the downloaded build artifacts: exactly
# one AppImage, one DMG and one installer (.exe), each named for the version.
# The names themselves are set by the build workflows (and installer.iss);
# this only checks there is one of each. Debug symbols are not artifacts; the
# builds upload them to Sentry.
#
# Prints the paths, one per line.
#
# Usage: collect-release-files.sh <artifacts-dir> <version>

set -euo pipefail

dir=$1
version=$2

for ext in AppImage dmg exe; do
    found=$(find "$dir" -type f -name "*.$ext")
    count=$(printf '%s' "$found" | grep -c '^' || true)
    if [ "$count" -ne 1 ]; then
        echo "::error::Expected exactly one .$ext in the artifacts, found $count:${found:+ $(echo "$found" | tr '\n' ' ')}" >&2
        exit 1
    fi
    case "$(basename "$found")" in
        *"$version"*) ;;
        *)
            echo "::error::$found is not named for version $version" >&2
            exit 1 ;;
    esac
    echo "$found"
done
