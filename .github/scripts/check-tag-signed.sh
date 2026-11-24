#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Tom Holz
#
# Fails unless the tag is an annotated tag whose signature GitHub verifies,
# that is, made with `git tag -s` by a key registered as a signing key on the
# tagger's GitHub account (RELEASE.md). Asks GitHub rather than the checkout:
# actions/checkout does not keep a tag's annotation.
#
# Usage: GH_TOKEN=... check-tag-signed.sh <owner/repo> <tag>

set -euo pipefail

repo=$1
tag=$2

if ! ref=$(gh api "repos/$repo/git/ref/tags/$tag" -q '.object.type + " " + .object.sha'); then
    echo "::error::Could not read tag $tag from GitHub"
    exit 1
fi
read -r type sha <<<"$ref"
if [ "$type" != "tag" ]; then
    echo "::error::$tag is a lightweight tag; make a signed one with git tag -s (RELEASE.md)"
    exit 1
fi

if ! verification=$(gh api "repos/$repo/git/tags/$sha" -q '"\(.verification.verified) \(.verification.reason)"'); then
    echo "::error::Could not read the signature of tag $tag from GitHub"
    exit 1
fi
read -r verified reason <<<"$verification"
if [ "$verified" != "true" ]; then
    echo "::error::$tag is not signed by a key GitHub can verify ($reason); make it with git tag -s (RELEASE.md)"
    exit 1
fi
echo "$tag: signature verified by GitHub"
