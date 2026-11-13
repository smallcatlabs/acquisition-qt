# Building Acquisition

Acquisition is a C++23 Qt Widgets application. It can be built on Windows,
macOS, and Linux with CMake and Qt 6.

The CMake project currently requires Qt 6.11 or newer with these Qt modules:

- Core
- Gui
- Network
- Network Authorization
- Sql
- Test
- Widgets

## Compiler Requirements

Acquisition uses C++ coroutines (via QCoro), which need more than a compiler
that accepts `-std=c++23`. CMake checks these floors explicitly and fails the
configure step below them:

- GCC 13 or newer (earlier GCC miscompiles coroutines with captures in
  temporary lambdas)
- Clang 15 or newer
- AppleClang 15 or newer (Xcode 15.2)
- MSVC 19.40 or newer (Visual Studio 2022 17.10)

The Clang, AppleClang, and MSVC minimums are the ones QCoro v0.13.0 documents;
the GCC floor is stricter for the miscompilation above.

## Third-Party Dependencies

CMake fetches third-party libraries (sentry-native, glaze, cpp-semver, spdlog,
QCoro, and QXlsx) at configure time via FetchContent; the vendored qdarkstyle
lives in `deps/`. QCoro is pinned exactly at v0.13.0 — a hard floor, not a preference:
the coroutine semantics acquisition relies on are verified at that release (see
`docs/design/network-redesign.md`, "Dependency: QCoro"). Its examples and tests
are kept out of the build with `QCORO_BUILD_EXAMPLES=OFF` and
`QCORO_BUILD_TESTING=OFF`; the global `BUILD_TESTING` flag is never touched, so
acquisition's own test suite stays enabled.

For day-to-day development, Qt Creator Community is the easiest setup on all
supported platforms. Open the repository as a CMake project, configure a Qt 6.11+
desktop kit, and build the `acquisition` target.

## Command Line Build

`CMakePresets.json` defines three build trees, each under `build/<preset>`:

| Preset | Build type | Use |
|---|---|---|
| `dev` | Debug | day-to-day development |
| `release` | Release | what the pull-request test job builds |
| `asan` | RelWithDebInfo + AddressSanitizer + UndefinedBehaviorSanitizer | test suite only; see below |

CMake has to be able to find Qt. Either export `CMAKE_PREFIX_PATH` or put it in
a `CMakeUserPresets.json` (ignored by git):

```sh
export CMAKE_PREFIX_PATH=<qt-install>/6.12.0/<compiler>   # e.g. ~/Qt/6.12.0/macos
export CMAKE_GENERATOR=Ninja                              # optional, faster
```

From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev
```

Replace `dev` with `release` for an optimized build. The executable is written
to the build tree. Run it with a temporary data directory while testing:

```sh
./build/dev/acquisition.app/Contents/MacOS/acquisition --data-dir /tmp/acq-data --log-level debug   # macOS
./build/dev/acquisition --data-dir /tmp/acq-data --log-level debug                                  # Linux
```

The presets are a convenience, not a requirement: `cmake -S . -B <dir>` with
the same cache variables builds the same thing, and the release workflows
still configure that way.

## Tests

The checked-in tests use Qt Test and are registered with CTest. After building:

```sh
ctest --preset dev
```

GUI-dependent tests are configured by CMake to use Qt's offscreen platform.

`.github/workflows/tests.yml` builds the `release` preset on Linux and runs the
whole suite on every pull request and every push to `master`. Run the suite
locally before opening a pull request; a change to behaviour comes with a test
that fails without it.

## Formatting

The C++ files under `src/` and `tests/` follow `.clang-format`, and the test
workflow fails a pull request whose files differ from the output of
clang-format **23.1.5**. Other versions format some constructs differently, so
use that one; `pipx install clang-format==23.1.5` provides it. To format the
tree:

```sh
git ls-files -z 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h' | xargs -0 clang-format -i
```

`// clang-format off/on` blocks are left alone. Commits that only reformat are
listed in `.git-blame-ignore-revs`; run
`git config blame.ignoreRevsFile .git-blame-ignore-revs` once so `git blame`
skips them.

Each of those files, and `tests/CMakeLists.txt`, starts with an
`SPDX-License-Identifier: GPL-3.0-or-later` comment line; the test workflow
fails a pull request that adds one without it and names the file.

## Sanitizer Builds (test/CI only)

The `ACQ_SANITIZE` CMake option builds the checked-in test suite with a runtime
sanitizer. It is a test/CI-only knob: it is OFF by default and release packaging
never sets it, so shipped binaries are never sanitized. Use a throwaway build
tree so the normal results stay untouched; the `asan` preset is that tree
(`build/asan`):

```sh
cmake --preset asan
cmake --build --preset asan
ASAN_OPTIONS=abort_on_error=1 ctest --preset asan
```

The build preset builds only `acq_tests`, every executable `ctest` runs, and
leaves out the app. The
preset sets `ACQ_SANITIZE=address,undefined`, the same as CI, and the knob
builds with `-fno-sanitize-recover=all`, so an UndefinedBehaviorSanitizer
report aborts its test and fails it; no `UBSAN_OPTIONS` setting is needed for
that.

This gives the phase-5 shutdown pins (`I-SHUT-*`, `I-LEAK-BOUND` in
`tst_workerintegration.cpp`) use-after-free teeth: if any event-loop turn ran
after owner destruction, a detached QCoro frame would resume into a freed
worker/hub and AddressSanitizer would report heap-use-after-free.

Do **not** build or run the `acquisition` app target under a sanitizer. The app
links crashpad, whose signal handlers are installed only by `sentry_init()` in
`src/main.cpp` (never in the test binaries), so a sanitized app can conflict with
the sanitizer's own handlers. The test binaries do not run crashpad and are safe.

On macOS, ASAN performs no leak detection (LSAN is unavailable there), so this
knob covers use-after-free and undefined behaviour only. On Linux, ASAN turns
LeakSanitizer on by default; only the integration runner's leaks are bounded,
so run the rest of the suite with `ASAN_OPTIONS=abort_on_error=1:detect_leaks=0`
as CI does. Leak-boundedness (`I-LEAK-BOUND`, see
`docs/design/network-redesign-phase5-verification.md` §6) is enforced on Linux CI
by `.github/workflows/sanitizers.yml`, which builds `acq_tests` with
`-DACQ_SANITIZE=address,undefined`, runs the whole suite with leak detection
off, and runs each `tst_workerintegration` scenario as its own process under
LeakSanitizer. The accepted detached-QCoro-frame leaks of the shutdown
scenarios are matched by a tight, CI-tuned suppression in `tests/lsan.supp` that
names only those known detached roots; any leak with a stack outside that closure
is a real defect and fails the job.

## Code Scanning

Static analysis is run by `.github/workflows/codeql.yml` using GitHub CodeQL.
The workflow performs a manual Linux CMake build so CodeQL sees the same Qt,
compiler, generated sources, and include paths used by the application build.

When each check runs:

| Workflow | Pull request | Push to `master` | Release tag (`v*`) | Other |
|---|---|---|---|---|
| Tests (`tests.yml`) | always | yes | no | on demand |
| Sanitizers (`sanitizers.yml`) | only when the change touches the worker, the API client, `src/ratelimit/`, the network manager, or the runner and its build; not a required check | no | yes | on demand |
| CodeQL (`codeql.yml`) | no | no | yes | weekly on `master`, on demand |

A pull request waits on the test suite and nothing else. CodeQL and the
sanitizer run are slow (17 to 83 minutes from start to result in November
2026) and rarely find anything, so they run when a release is tagged, not when
a change merges.

A final release is always preceded by a beta or release-candidate tag
(`v0.18.10-beta.1`). Tag the final release only after that tag's CodeQL and
sanitizer runs have come back clean; a finding is fixed on `master` and gets a
new beta tag. Nothing is released unscanned, at the price that a finding
points at the changes since the previous tag, not at one merge.

CodeQL files alerts against the ref it scanned and does not itself fail the
job, so the workflow's last step does: it fails when that ref has an open
alert (a dismissed alert does not count) and prints the alerts. A green CodeQL
job on a tag therefore means a clean scan. The weekly run keeps the alerts for
`master` current between releases.

## Protected Branch

The repository's `Protect default branch` ruleset applies to `master`. Changes
must arrive through a pull request, the `Build + CTest (Linux)` check must pass,
and all review conversations must be resolved. No approval is
required while the repository has a single maintainer, and a branch need not be
rebased onto the latest `master` before merging. Force pushes and deletion of
the default branch are blocked.

## Linux

Linux builds require OpenSSL 3.x support at build and runtime. On Ubuntu-like
systems, the GitHub Actions workflow installs these packages:

```sh
sudo apt-get install -y \
  patchelf openssl libfuse2 \
  libcurl4-openssl-dev \
  libgl-dev \
  libssl-dev \
  libvulkan-dev \
  libxcb-cursor0 \
  libxcb-cursor-dev \
  zlib1g-dev
```

Other distributions may need equivalent packages. If Qt cannot find OpenSSL, set
`OPENSSL_ROOT_DIR` or adjust your library path for your local installation.

### Desktop Integration and LD_LIBRARY_PATH

When Acquisition is run with `LD_LIBRARY_PATH` pointing at an official Qt
installation, anything that opens an external URL or file can fail silently.
This affects OAuth login, the Help menu links, the update download, and the
crash-report links. The window appears and the rest of the application works,
so the only sign is a message on stderr such as:

```
version `Qt_6.11_PRIVATE_API' not found (required by /lib64/libKF6WindowSystem.so.6)
```

Those paths depend on the distribution and desktop environment; the shape of
the message is the part that matters.

The cause is environment inheritance, not a bug in the application.
`QDesktopServices::openUrl` hands the URL to `xdg-open`, which delegates to a
desktop-specific helper, and that helper is a distribution binary linked
against the distribution's Qt. The helper inherits `LD_LIBRARY_PATH` from
Acquisition, so it loads the official kit's Qt libraries instead of the system
ones. Distributions and the official Qt installers do not always tag private
symbols the same way, so the helper's own dependencies then fail to resolve.
Most helpers still exit successfully after this failure, which is why the
application reports nothing and the browser simply never opens.

Do not export `LD_LIBRARY_PATH` when running a local build. The binary's
`RUNPATH` already points at the Qt kit, and the Qt libraries locate each other
through `$ORIGIN`, so the application resolves its dependencies without it.
Confirm this for a given build before changing anything else; the following
should print nothing:

```sh
env -u LD_LIBRARY_PATH ldd ./build/acquisition | grep "not found"
```

In Qt Creator, both sources of the variable are in Projects -> Run:

- Clear any `LD_LIBRARY_PATH` entry under Environment.
- Uncheck "Add build library search path to LD_LIBRARY_PATH".

Qt Creator rewrites `CMakeLists.txt.user` when it exits, so edit that file only
while Qt Creator is closed, or make the change through the user interface.

## Release Packaging

Release artifacts are built by GitHub Actions:

- `.github/workflows/build-linux.yml` builds the Linux AppImage.
- `.github/workflows/build-macos.yml` builds the macOS DMG.
- `.github/workflows/build-windows.yml` builds the Windows installer.

The workflows run on `workflow_dispatch` and on tags matching `v*`. Tag builds
create draft GitHub releases and attach the platform artifacts. They install
Qt with the `qtnetworkauth` module: 6.12.0 on macOS and Linux, and 6.11.1 on
Windows, where a 6.12.0 package failed at start-up with "SSL support is
missing" (#370). The Windows workflow runs the packaged binary with
`--check-tls` and fails when it finds no TLS backend that supports SSL.

## Platform Notes

Windows release packaging uses MSVC 2022, `windeployqt`, the Visual C++
Redistributable, and Inno Setup via `installer.iss`.

macOS release packaging uses `macdeployqt` to produce a DMG. The current workflow
runs on `macos-latest` and names the uploaded artifact as an arm64 DMG.

Linux release packaging uses `linuxdeploy` and `linuxdeploy-plugin-qt` to produce
an AppImage.
