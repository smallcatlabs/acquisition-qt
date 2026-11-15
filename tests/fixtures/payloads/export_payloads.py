#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Tom Holz
"""Export a small sample of real API replies from Acquisition user stores (#246).

The user store keeps the `stash` / `character` object of every reply exactly
as the API sent it (F62), so a store that has been refreshed is a capture.
This script reads the stash and character tables of one or more stores, picks
a small sample, replaces character and account names, and writes one JSON
file per reply into the directory it lives in, with README.md listing them.

    python3 -I export_payloads.py <userstore.db> [<userstore.db> ...]

Stores are read in the order given and earlier ones are preferred, so name
the store with the newest league first. Only stores copied out of, or
created outside, the platform's application-data location: never point this
at a real Acquisition data directory.

What is kept (maintainer's ruling, 2026-11-14):
  - one stash tab of each type, and of each role a type appears in (a parent
    with children, a child, a tab inside a folder, a plain tab);
  - items so that every frame type appears FRAME_QUOTA times, every flag that
    is set on some item FLAG_QUOTA times, every key an item carries, at any
    depth, once, and CURRENCY_QUOTA different
    currencies; an item stays in the tab it was found in;
  - characters: one from the first store, and the one elsewhere that adds
    the most frame types, features and sections not covered yet; their
    inventories are cut to INVENTORY_ITEMS items plus any that add coverage.
What is changed: character names and account names, and nothing else, except
that a reply is written back out by a JSON library after items are removed
from its lists, so the spelling of a number can differ (20.00 becomes 20.0).
"""

import collections
import json
import pathlib
import re
import sqlite3
import sys

FRAME_QUOTA = 3
FLAG_QUOTA = 2
CURRENCY_QUOTA = 8
CURRENCY_FRAME = 5
HOME_ITEMS = 2  # items kept in a tab chosen for its type, whatever they add
CHILD_DESCRIPTORS = 3  # entries kept in a parent tab's `children` list
INVENTORY_ITEMS = 3  # items kept from a character's inventory
ITEM_SECTIONS = ("equipment", "inventory", "rucksack", "jewels", "guardian", "wickerman", "skills")

OUT = pathlib.Path(__file__).resolve().parent


def open_store(path):
    # '#' in an account name would start a URI fragment.
    return sqlite3.connect("file:" + str(path).replace("#", "%23") + "?mode=ro", uri=True)


def role_of(reply):
    if "parent" in reply:
        return "child"
    if "children" in reply:
        return "parent"
    if "folder" in reply:
        return "in a folder"
    return "plain"


def key_paths(value, prefix):
    """Every key below `value`, as a path; list positions and numeric keys
    (indexes, not names) are not part of it."""
    if isinstance(value, list):
        for member in value:
            yield from key_paths(member, prefix)
    elif isinstance(value, dict):
        for key, member in value.items():
            if key == "socketedItems":
                continue  # items in their own right
            path = prefix + ("#" if key.isdigit() else key)
            yield path
            yield from key_paths(member, path + ".")


def features(item):
    """What an item contributes to coverage."""
    found = [("frame", item.get("frameType"))]
    found += [("flag", key) for key, value in item.items() if value is True]
    found += [("key", path) for path in set(key_paths(item, ""))]
    if item.get("frameType") == CURRENCY_FRAME:
        found.append(("currency", item.get("typeLine")))
    return found


class Coverage:
    def __init__(self):
        self.count = collections.Counter()
        self.currencies = set()

    def wants(self, item):
        for kind, value in features(item):
            if kind == "frame" and self.count[(kind, value)] < FRAME_QUOTA:
                return True
            if kind == "flag" and self.count[(kind, value)] < FLAG_QUOTA:
                return True
            if kind == "key" and self.count[(kind, value)] < 1:
                return True
            if (
                kind == "currency"
                and value not in self.currencies
                and len(self.currencies) < CURRENCY_QUOTA
            ):
                return True
        return any(self.wants(child) for child in item.get("socketedItems", []))

    def gain(self, items):
        """The features these items would cover for the first time."""
        new = set()
        for item in items:
            new |= {feature for feature in features(item) if not self.count[feature]}
            new |= self.gain(item.get("socketedItems", []))
        return new

    def take(self, item):
        for kind, value in features(item):
            self.count[(kind, value)] += 1
            if kind == "currency":
                self.currencies.add(value)
        for child in item.get("socketedItems", []):
            self.take(child)


def main(store_paths):
    names = set()  # every character and account name seen, for the scrub
    stashes = []  # (store index, league, reply)
    characters = []  # (store index, reply)
    for index, path in enumerate(store_paths):
        account = re.fullmatch(r"userstore-(.*)\.db", pathlib.Path(path).name)
        if not account:
            sys.exit(f"not a user store file name: {pathlib.Path(path).name}")
        names.add(account.group(1))
        names.add(account.group(1).split("#")[0])
        db = open_store(path)
        for (name,) in db.execute("select name from characters"):
            names.add(name)
        for league, text in db.execute(
            "select league, json_data from stashes where json_data is not null"
            " order by league, stash_index, id"
        ):
            stashes.append((index, league, json.loads(text)))
        for (text,) in db.execute(
            "select json_data from characters where json_data is not null order by id"
        ):
            characters.append((index, json.loads(text)))

    coverage = Coverage()
    kept = {}  # id(reply) -> list of kept item positions
    replies = {}  # id(reply) -> (league, reply)

    def keep(league, reply, position):
        replies[id(reply)] = (league, reply)
        positions = kept.setdefault(id(reply), [])
        if position is not None and position not in positions:
            positions.append(position)
            coverage.take(reply["items"][position])

    # 1. One tab for each type and role, the first non-empty one found.
    homes = {}
    for index, league, reply in stashes:
        slot = (reply["type"], role_of(reply))
        best = homes.get(slot)
        if best is None or (not best[1].get("items") and reply.get("items")):
            homes[slot] = (league, reply)
    for league, reply in homes.values():
        keep(league, reply, None)
        for position in range(min(HOME_ITEMS, len(reply.get("items", [])))):
            keep(league, reply, position)

    # 2. Items that add coverage: first from the tabs already chosen, then
    #    from any tab, so that as few tabs as possible are added.
    chosen = [value for value in replies.values()]
    for candidates in (chosen, [(league, reply) for _, league, reply in stashes]):
        for league, reply in candidates:
            for position, item in enumerate(reply.get("items", [])):
                if coverage.wants(item):
                    keep(league, reply, position)

    # 3. Characters, each trimmed and counted as it is picked: the first
    #    store's first, then the one that adds the most frame types, and
    #    after that the most features and sections, not covered yet.
    def character_items(reply):
        return [item for section in ITEM_SECTIONS for item in reply.get(section) or []]

    def trimmed(reply, number):
        out = dict(reply)
        out["name"] = f"ExampleCharacter{number}"
        for item in character_items({**out, "inventory": []}):
            coverage.take(item)
        inventory = []
        for position, item in enumerate(out.get("inventory") or []):
            if position < INVENTORY_ITEMS or coverage.wants(item):
                inventory.append(item)
                coverage.take(item)
        if "inventory" in out:
            out["inventory"] = inventory
        return out

    picked = []
    sections = set()
    remaining = [reply for _, reply in characters]
    first_store = [reply for index, reply in characters if index == 0]
    if first_store:
        picked.append(trimmed(first_store[0], 1))
        sections = {key for key in first_store[0] if first_store[0][key]}
        remaining = [reply for reply in remaining if reply is not first_store[0]]
    if remaining:

        def adds(reply):
            gain = coverage.gain(character_items(reply))
            # A frame type seen nowhere else outweighs everything.
            return (
                sum(1 for kind, _ in gain if kind == "frame"),
                len(gain) + len({key for key in reply if reply[key]} - sections),
            )

        best = max(remaining, key=adds)
        picked.append(trimmed(best, len(picked) + 1))

    for old in OUT.glob("*.json"):
        old.unlink()
    rows = []
    serial = collections.Counter()

    def write(name, reply):
        text = json.dumps(reply, ensure_ascii=False, indent=1) + "\n"
        (OUT / name).write_text(text, encoding="utf-8")
        return len(text.encode("utf-8"))

    kept_ids = {reply["id"] for _, reply in replies.values()}
    for league, reply in replies.values():
        out = dict(reply)
        total = len(reply.get("items", []))
        if "items" in reply:
            out["items"] = [reply["items"][p] for p in sorted(kept[id(reply)])]
        if "children" in reply:
            mine = [c for c in reply["children"] if c["id"] in kept_ids]
            others = [c for c in reply["children"] if c["id"] not in kept_ids]
            out["children"] = (mine + others)[: max(CHILD_DESCRIPTORS, len(mine))]
        slot = f"{league}-{reply['type']}".replace(" ", "")
        serial[slot] += 1
        name = f"stash-{slot}-{serial[slot]}.json"
        size = write(name, out)
        rows.append((name, league, reply["type"], role_of(reply), len(out.get("items", [])), total, size))

    for number, out in enumerate(picked, start=1):
        name = f"character-{number}.json"
        size = write(name, out)
        rows.append((name, out.get("league", ""), "character", "", len(character_items(out)), "", size))

    # The scrub is checked, not assumed: no name may survive anywhere.
    leaks = []
    for path in sorted(OUT.glob("*.json")):
        text = path.read_text(encoding="utf-8").lower()
        for name in names:
            if name and name.lower() in text:
                leaks.append(path.name)
    if leaks:
        sys.exit(
            "a character or account name survives in: "
            + ", ".join(sorted(set(leaks)))
            + " (the files were written; do not commit them)"
        )

    # Measured, not assumed: what the stores hold that the sample does not.
    def all_items(items):
        for item in items:
            yield item
            yield from all_items(item.get("socketedItems", []))

    everything = [item for _, _, reply in stashes for item in all_items(reply.get("items", []))]
    everything += [item for _, reply in characters for item in all_items(character_items(reply))]
    missing = sorted(
        {
            f"{kind} {value}"
            for item in everything
            for kind, value in features(item)
            if kind != "currency" and not coverage.count[(kind, value)]
        }
    )

    rows.sort()
    lines = [
        "# Real API replies",
        "",
        "Written by `export_payloads.py` in this directory; do not edit by hand.",
        "The script's header says what is kept and what is changed. Each file is the",
        "`stash` or `character` object of one reply, with items removed from its lists.",
        "",
        "| File | League | Type | Role | Items kept | Items in the reply | Bytes |",
        "|---|---|---|---|---|---|---|",
    ]
    lines += ["| " + " | ".join(str(cell) for cell in row) + " |" for row in rows]
    lines += [
        "",
        "Coverage of the items kept:",
        "",
        "- frame types: "
        + ", ".join(
            f"{value} ×{n}"
            for (kind, value), n in sorted(coverage.count.items(), key=lambda kv: str(kv[0]))
            if kind == "frame"
        ),
        "- flags: "
        + ", ".join(
            f"{value} ×{n}" for (kind, value), n in sorted(coverage.count.items()) if kind == "flag"
        ),
        "- currencies: " + ", ".join(sorted(coverage.currencies)),
        "- frame types, flags and keys found on some item in the stores and on none in the "
        f"sample: {', '.join(missing) if missing else 'none'}",
        f"- distinct item keys, at any depth: "
        f"{sum(1 for kind, _ in coverage.count if kind == 'key')}",
        "",
    ]
    (OUT / "README.md").write_text("\n".join(lines), encoding="utf-8")
    total = sum(row[-1] for row in rows)
    print(f"{len(rows)} files, {total} bytes, {len(names)} names checked, {len(missing)} features missing")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
