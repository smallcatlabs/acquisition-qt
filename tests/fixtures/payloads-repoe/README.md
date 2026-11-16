# Game data for the payload corpus

An excerpt of the RePoE files the application downloads from
`https://repoe-fork.github.io`, laid out like the `repoe` directory of a data
directory. `tst_golden` loads it before it builds the items of `../payloads`.
`version.txt` is the game version the files were cut from.

Written by `corpus_gamedata` (`tests/corpus_gamedata.cpp`); do not edit by
hand. Entries are copied byte for byte.

- `item_classes.min.json`: the whole file.
- `base_items.min.json`: the entries named like the base type of an item in
  the corpus.
- `stat_translations.min.json`, `stat_translations/necropolis.min.json`: the
  entries that give a mod name in an item's mod table, and the entries whose
  text is the text of a mod line in the corpus once numbers and signs are
  left out of both and the game's `[id|text]` markup is read as its text.

After cutting, the tool builds the corpus's items with the whole game data
and with the excerpt and compares every line `tests/corpusdump.h` writes.
The excerpt was kept only because they were the same.

## One class for a name

The game data gives some base item names entries of several item classes. The
application keeps the entry whose key comes last (#390), whatever else is
loaded, so the excerpt holds every entry of such a name and resolves it as
the whole game data does. In the corpus these are Polaric Invitation, Potent
Chaotic Resonator and Powerful Alchemical Resonator.

## When the excerpt is not enough

A change to how a mod line is matched can make a line reach a translation
that is not in the excerpt, if it matches more loosely than "the same text,
numbers and signs aside". The excerpt cannot show that. With the whole game
data at hand (the `repoe` directory of any data directory the application
has started in):

```sh
ACQ_GOLDEN_GAMEDATA=<repoe-dir> QT_QPA_PLATFORM=offscreen ./build/dev/tests/acq_tests tst_golden
```

## Cutting again

After new payloads, a new game version, or a change that should use more of
the game data:

```sh
./build/dev/tests/corpus_gamedata <repoe-dir> tests/fixtures/payloads tests/fixtures/payloads-repoe
ACQ_UPDATE_GOLDEN=1 QT_QPA_PLATFORM=offscreen ./build/dev/tests/acq_tests tst_golden
```

The first command also prints the census for the whole game data; it exits
with 1, and says where, if the excerpt gives a different result.
