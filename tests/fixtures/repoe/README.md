# RePoE excerpts

Real game data for tests, taken from the public RePoE files the application
downloads from `https://repoe-fork.github.io` (game version 3.29.3.6, as
given by that site's `version.txt` when the files were fetched on
2026-11-12).

## `stat_translations.json`

Whole entries of `stat_translations.min.json`, copied byte for byte. The
file is a JSON array with one source entry per line; nothing inside an
entry is edited. Entries were cut by their position in the source array
with a script that slices each top-level element's raw text out of the
file and checks that it parses to the same object.

Source positions, in file order: 1593, 1674, 2857.
