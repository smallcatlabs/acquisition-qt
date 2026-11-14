# Repository Guidelines

Acquisition is a C++23 Qt Widgets application for Path of Exile stash,
inventory, and forum shop management. This repository is the Qt application,
maintained indefinitely. A Rust implementation is developed separately in
`smallcatlabs/acquisition` (`docs/adr/0003-rewrite-vs-evolve.md`); nothing
there binds work here.

## Build, Run, Test

Read `BUILD.md` before changing build, packaging, or platform setup.

```sh
export CMAKE_PREFIX_PATH=<qt-install>/6.12.0/<compiler>   # e.g. ~/Qt/6.12.0/macos
export CMAKE_GENERATOR=Ninja                              # optional
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

- Presets (`dev`, `release`, `asan`) build under `build/<preset>`. Use them;
  do not create other build trees in the repository root.
- CMake requires Qt 6.11+, the Qt Network Authorization module, and the
  private Qt Gui module that the fetched QXlsx dependency links against. A
  distribution's Qt usually ships neither, and configuring fails in
  `_deps/qxlsx-src` with `Failed to find required Qt component "GuiPrivate"`.
  Point `CMAKE_PREFIX_PATH` at an official Qt kit. Linux also requires OpenSSL.
- Run the app only with `--data-dir <temporary directory>`. Never read or
  write a real Acquisition data directory (the platform's application-data
  location), in the app, in tests, or in investigation scripts. It holds the
  user's tokens, prices and account data.
- Enable the commit hook once per clone: `git config core.hooksPath .githooks`.

## Working Rules

- **Work comes from GitHub issues.** Milestones group them; `area:` labels say
  where. `needs-decision` means the maintainer has to rule before code is
  written. `docs/cleanup/findings.md` holds standing constraints and
  design-level findings (F-numbers); check both before fixing something that
  looks broken.
- **A change in behaviour comes with a test that fails without it.** Run the
  test and see it fail before writing the fix. A test that passes at once has
  proved nothing.
- **Fixtures use what the game actually sends.** Property values look like
  `8.00%` and `20 (Max)`; item classes are named `rings` and
  `one hand swords`; requirements arrive as both `Int` and `Intelligence`.
  Tests built on invented formats have hidden real defects here.
- **Pull requests** must pass the Tests check, and nothing else gates a merge.
  CodeQL and the sanitizer run happen on release tags (the sanitizer job also
  runs, without gating, on a pull request that touches the refresh or network
  code); a finding on a beta tag is fixed before the final release. Keep one
  topic per pull request. The maintainer merges and decides which release a
  change rides in; do not bump the version unless asked.
- When UI behaviour changes, update `docs/user/`.
- The C++ tree is checked against `.clang-format` in CI with a pinned
  clang-format version (`BUILD.md`, "Formatting"); format with that
  version, respect existing `// clang-format off/on` blocks, and keep
  nearby naming and organization.
- Prefer existing Qt and local helper patterns over new abstractions.
- Keep generated logs, `settings.ini`, OAuth tokens, and other user-local
  state out of commits.
- For release changes, compare `CMakeLists.txt`, `installer.iss`, and the
  relevant workflow so version and packaging assumptions stay aligned. The
  version has one source: `project(VERSION ...)` in `CMakeLists.txt`. The
  release procedure is in `RELEASE.md`.

## Where To Look

| Area | Code | Read first | Must not break |
|---|---|---|---|
| Refresh pipeline | `src/itemsmanagerworker.*`, `src/itemsmanager.*` | `docs/design/items-pipeline.md`; the M2 spec's D1–D8 and D10 | An update's identity is its `std::stop_token`, checked after every await. A reply replaces its source atomically. Nothing the worker discards is persisted. |
| Rate limiting and network | `src/ratelimit/`, `src/poe/poeapiclient.*`, `src/util/networkmanager.*` | `docs/design/network-redesign.md` (frozen); `docs/design/network-ground-truth.md` (N-claims) | One HEAD in flight at a time (F5). The hub lives on the main thread. Every API request carries the transfer timeout. Owners are destroyed only after the event loop stops. |
| Search and model | `src/search.*`, `src/bucket.*`, `src/items_model.*`, `src/column.*`, `src/filters/` | `docs/design/items-pipeline-m3.md`; `docs/design/folder-tree.md` | Refresh deltas reach the model as row operations, never a reset. `src/filters/` stays free of widgets (the `filters_boundary` test). |
| Items | `src/item.*`, `src/modlist.*`, `src/pseudomods.*`, `src/repoe/`, `src/poe/types/` | Issues labelled `area:items` | Game data (RePoE) is loaded before any `Item` is built. |
| Pricing and shop | `src/buyout*`, `src/shop.*`, `src/currency*` | `docs/user/pricing.md`, `docs/user/forum-shop.md`; `docs/design/trade-ground-truth.md` (T-claims) | Prices set in the game are never posted to the forum. Automatic shop submission happens only after a clean completed refresh. User-authored prices are never dropped. |
| Storage | `src/datastore/`, `src/legacy/` | F22, F64 and F66 in `findings.md` | Any change to a repo table's DDL bumps `SCHEMA_VERSION` in the same commit. Buyout tables are never dropped or rebuilt. |
| Shell | `src/main.cpp`, `src/application.*`, `src/ui/logindialog.*`, `src/util/` | "Standing constraints" in `findings.md` | Logging teardown comes last (F29). Never touch `logger->sinks()` outside `logging::init` (F42). One user session per process. Credentials never reach the log. |
| UI | `src/ui/` | `docs/user/` | — |
| Build, CI, release | `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/`, `installer.iss` | `BUILD.md` | `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` stays a global definition ahead of `FetchContent` (F78). |

`docs/README.md` maps every document. The frozen specs remain full authority
for the areas they cover; cite their decision numbers, the F-numbers, and the
N- and T-claims instead of restating them.

## Project Map

- `src/` is the application. `src/ui/` holds the widgets; `src/filters/` is a
  separate library with no UI dependency.
- `tests/` is the Qt Test suite, with fakes for the network (`fake*.h`) and a
  main-window fixture.
- `assets/` holds bundled icons and Path of Exile-style UI images.
- `docs/` holds the user guide, design specs, ADRs, and the findings register.
- `deps/qdarkstyle/` is vendored. Other third-party libraries are fetched by
  CMake.
- Release packaging is defined in `.github/workflows/`.
