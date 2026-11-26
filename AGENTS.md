# Repository Guidelines

Acquisition is a C++23 Qt Widgets application for Path of Exile stash,
inventory, and forum shop management. This repository is the Qt application,
maintained indefinitely. A Rust implementation is developed separately in
`smallcatlabs/acquisition` (`docs/adr/0003-rewrite-vs-evolve.md`); nothing
there binds work here.

## Build and Run

Read `BUILD.md` before changing build, packaging, or platform setup.

```sh
export CMAKE_PREFIX_PATH=<qt-install>/6.11.3/<compiler>   # e.g. ~/Qt/6.11.3/macos
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
- A source file here can need more than a gigabyte to compile. With Ninja the
  first-party sources compile in a job pool sized from memory, and
  configuring prints the size (`First-party sources compile N at a time`).
  Check that line against the memory you really have: if you share the
  machine with other builds or agents, or run under a limit CMake cannot see,
  configure with `-DACQ_COMPILE_JOBS=<n>`, about one job for every 2 GB that
  is yours to use (`BUILD.md`, "Memory and parallel jobs").
- Never read or write a real Acquisition data directory (the platform's
  application-data location), in the app, in tests, or in investigation
  scripts. It holds the user's tokens, prices and account data.
- Run the `acquisition` executable only with `--data-dir <temporary
  directory>`. To exercise the application, use the app fixture below.
- Enable the commit hook once per clone: `git config core.hooksPath .githooks`.

## Test

`BUILD.md`, "Tests", has the detail. Use the smallest of these that can fail
for the change in hand:

- **A suite.** `tests/tst_<name>.cpp`. All suites link into one executable:
  `acq_tests <suite> [function]` runs one, `ctest --preset dev` runs all.
  `BUILD.md`, "Adding a test", says what to include and where a shared
  fixture goes, so that a suite stays cheap to compile.
- **What the window shows.** `tests/viewtranscript.h` prints the item tree as
  text, with its selection, current row and collapsed tabs. A test compares
  the print before and after and names the lines allowed to change.
  `ACQ_TEST_SCREENSHOT_DIR=<dir>` saves PNGs.
- **The whole application.** `tests/appfixture.h` builds the real
  `Application` over a temporary data directory, with the network answered
  from files. `tests/tst_appsmoke.cpp` runs the release smoke list this way.
- **The live API.** `live_capture` (`BUILD.md`, "Live capture") refreshes once
  against the real API, and with `--out` leaves what the window then shows as
  text and as a picture. Run it only with a live directory and an account the
  user names for it, and never on a branch that changes the refresh or network
  code (the `src/` paths in `.github/workflows/sanitizers.yml`; it checks, and
  refuses). The directory and the account are local settings: they stay out
  of the repository, issues, pull requests and commits.
- **Windows.** The pull-request job builds and tests there. For a failure
  that only happens on Windows, `BUILD.md`, "Windows from another machine":
  build and test over SSH on the user's host alias, if they have set one up.
  Its address, user and key are local settings too.

## Working Rules

- **Work comes from GitHub issues.** Milestones group them; `area:` labels say
  where. `needs-decision` means the maintainer has to rule before code is
  written. `docs/cleanup/findings.md` holds standing constraints and
  design-level findings (F-numbers); check both before fixing something that
  looks broken.
- **A change in behaviour comes with a test that fails without it.** Run the
  test and see it fail before writing the fix. A test that passes at once has
  proved nothing.
- **Fixtures use what the game actually sends.** Take them from
  `tests/fixtures/payloads`, real replies with character and account names
  scrubbed. Property values look like
  `8.00%` and `20 (Max)`; item classes are named `rings` and
  `one hand swords`; requirements arrive as both `Int` and `Intelligence`.
  Tests built on invented formats have hidden real defects here.
- **Pull requests** must pass the Linux Tests check; no other check gates a
  merge. The Windows job beside it does not gate; a pull request says so if
  it is red. CodeQL and the sanitizer run happen on release tags (the
  sanitizer job also runs, without gating, on a pull request that touches the
  refresh or network code); a finding on a beta tag is fixed before the final
  release. Split work into separate pull requests only where each can merge
  and be reverted without the others. Work that cannot be separated that way
  goes in one pull request, one commit per step, each commit building and
  passing its tests. Unrelated changes never share a pull request. The
  maintainer merges and decides which release a change rides in; do not bump
  the version unless asked.
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
  version has one source: `project(VERSION ...)` plus `app_version_suffix` in
  `CMakeLists.txt`. How a release is tagged and published is in `RELEASE.md`.

## Where To Look

| Area | Code | Read first | Must not break |
|---|---|---|---|
| Refresh pipeline | `src/itemsmanagerworker.*`, `src/itemsmanager.*` | `docs/design/items-pipeline.md`; D1–D8 and D10 in `docs/design/items-pipeline-m2.md` | An update's identity is its `std::stop_token`, checked after every await. A reply replaces its source atomically. Nothing the worker discards is persisted. |
| Rate limiting and network | `src/ratelimit/`, `src/poe/poeapiclient.*`, `src/util/networkmanager.*` | `docs/design/network-redesign.md` (frozen); `docs/design/network-ground-truth.md` (N-claims) | One HEAD in flight at a time (F5). The hub lives on the main thread. Every API request carries the transfer timeout. Owners are destroyed only after the event loop stops. |
| Search and model | `src/search.*`, `src/bucket.*`, `src/items_model.*`, `src/column.*`, `src/filters/` | `docs/design/items-pipeline-m3.md`; `docs/design/folder-tree.md` | Refresh deltas reach the model as row operations, never a reset. `src/filters/` stays free of widgets (the `filters_boundary` test). |
| Items | `src/item.*`, `src/modlist.*`, `src/pseudomods.*`, `src/repoe/`, `src/poe/types/` | Issues labelled `area:items` | Game data (RePoE) is loaded before any `Item` is built. glaze is included only through `util/json_glaze.h`, from `.cpp` files; what glaze must know about a type (a renamed or skipped key, a custom reader) is declared in a `glaze_meta.h`, never beside the struct (the `glaze_boundary` test). |
| Pricing and shop | `src/buyout*`, `src/shop.*`, `src/currency*` | `docs/user/pricing.md`, `docs/user/forum-shop.md`; `docs/design/trade-ground-truth.md` (T-claims) | Prices set in the game are never posted to the forum. Automatic shop submission happens only after a clean completed refresh. User-authored prices are never dropped. |
| Storage | `src/datastore/`, `src/legacy/` | F22, F64 and F66 in `findings.md` | Any change to a repo table's DDL bumps `SCHEMA_VERSION` in the same commit. Buyout tables are never dropped or rebuilt. |
| Shell | `src/main.cpp`, `src/application.*`, `src/ui/logindialog.*`, `src/util/` | "Standing constraints" in `findings.md` | Logging teardown comes last (F29). Never touch `logger->sinks()` outside `logging::init` (F42). One user session per process. Credentials never reach the log. |
| UI | `src/ui/` | `docs/user/` | — |
| Build, CI, release | `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/`, `installer.iss` | `BUILD.md` | `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` stays a global definition ahead of `FetchContent` (F78). `acquisition_core` links no Qt Widgets; widgets and `Application` live in `acquisition_gui` (the `core_boundary` test). |

`docs/README.md` maps every document. The frozen specs remain full authority
for the areas they cover; cite their decision numbers, the F-numbers, and the
N- and T-claims instead of restating them.

`assets/` holds the bundled icons and UI images. `deps/qdarkstyle/` is
vendored; other third-party libraries are fetched by CMake.
