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
export CMAKE_PREFIX_PATH=<qt-install>/6.11.3/<compiler>   # e.g. ~/Qt/6.11.3/macos
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

Every suite is linked into one executable, `acq_tests`, and CTest runs each
suite in a process of its own under the suite's name (`tst_search`). To run
one suite, or one test function of it, name the suite first and pass Qt Test's
arguments after it; `acq_tests` with no argument lists the suites:

```sh
ctest --preset dev -R '^tst_search$'
QT_QPA_PLATFORM=offscreen ./build/dev/tests/acq_tests tst_search
QT_QPA_PLATFORM=offscreen ./build/dev/tests/acq_tests tst_search <function> -v2
```

### Adding a test

A suite is a file `tests/tst_<name>.cpp` holding one Qt Test class. It
includes `testsuite.h` and ends with one registration line where a Qt Test
program has `QTEST_MAIN`, before its `#include "tst_<name>.moc"`:
`ACQ_TEST_MAIN(Class)` when it needs a `QApplication` (widgets), otherwise
`ACQ_TEST_GUILESS_MAIN(Class)`. Then add `acq_add_test(tst_<name>)` to
`tests/CMakeLists.txt`, or `acq_add_test(tst_<name> GUILESS)` for a suite
without a `QApplication`: it is compiled against the core library alone, with
no Qt Widgets header in reach. A suite that must run one test function per
process registers with `ACQ_TEST_SCENARIO_MAIN` and `acq_add_scenario_test`,
as `tst_workerintegration` does.

What a suite includes is most of what it costs to compile (#476), so:

- Include `<QTest>` and the Qt classes the file names (`<QSignalSpy>`), not
  the module header `<QtTest>`.
- A fixture more than one suite uses is declared in a header and defined in a
  `.cpp` file of a test support library (`tests/CMakeLists.txt`):
  `acq_testsupport_core` for what needs only the core library,
  `acq_testsupport` for what builds a window. A suite then compiles the
  fixture's declaration, not its body.
- glaze is expensive, and it enters a file only through
  `util/json_glaze.h`, from a `.cpp` file (the `glaze_boundary` test). A
  suite that only needs a value parsed or written uses a helper from
  `testfixtures.h` (`makeTestWireItem`, `serializeTestToken`) or
  `util/json_readers.h`, and leaves glaze to the suites that test the JSON
  itself.

All suites share one link. Keep a file's helper classes and functions in an
anonymous namespace or `static`, and give the test class a name no other suite
uses: two different definitions of one name at namespace scope compile and
link without an error, and the program uses one of them for both. A file that
still ends with `QTEST_MAIN` fails the link with a second `main`. A
`tst_*.cpp` that `tests/CMakeLists.txt` does not name is neither built nor
run.

A struct that glaze reads or writes by reflection cannot be in an anonymous
namespace: Clang and MSVC refuse it, and GCC does not, so the Linux check
passes.
Use a standard container or a wire type; if a struct of the suite's own is
needed, put it in a namespace named after the suite.

`.github/workflows/tests.yml` builds the `release` preset on Linux and runs the
whole suite on every pull request and every push to `master`. Run the suite
locally before opening a pull request; a change to behaviour comes with a test
that fails without it.

### Window tests

`tests/viewtranscript.h` prints what a `QTreeView` shows, one line per
visible row with marks for expanded or collapsed, selected and current. A
window test takes the print before and after the operation it is about and
checks that only the lines it names changed (`viewTranscriptChanges`), so a
dropped selection or a re-opened tab fails a test written about something
else. Keep the scenes small and the expected text inline in the test.

Set `ACQ_TEST_SCREENSHOT_DIR` to a directory and a window test saves PNGs
there (`QWidget::grab()`) at the steps it names (`saveTestScreenshot`) and,
with a `ScreenshotOnFailure` in scope, when it fails; unset, nothing is
saved. The offscreen platform does not draw native dialogs. The
Tests workflow uploads the directory when a run fails.

```sh
ACQ_TEST_SCREENSHOT_DIR=$PWD/build/dev/screenshots ctest --preset dev -R tst_mainwindow
```

### Application scenarios

`tests/tst_appsmoke.cpp` runs the release smoke list on the real
`Application` (`tests/appfixture.h`): everything `main()` builds, over a
temporary data directory, with the network answered from files by
`tests/scriptednetwork.h`. The scene is three stash tabs from
`tests/fixtures/payloads`, in the user store before the application starts
and served again to a refresh. Each scenario is its own process and its own
CTest entry, `tst_appsmoke.<scenario>`; a new one is a slot in that file and
its name in `acq_add_scenario_test(tst_appsmoke ...)`.

```sh
ctest --preset dev -R tst_appsmoke
QT_QPA_PLATFORM=offscreen build/dev/tests/acq_tests tst_appsmoke refreshCheckedKeepsTheView
```

A refresh runs on the rate limiter's real timers, about a quarter of a
second for each request. A scenario fails if the application asked the
network for something the script does not answer, or opened a dialog box.

What the scenarios cover on the real application, by step of the release
smoke list: starting from cached items (2); Refresh Checked and Refresh
Selected (3); a price saved with Enter, the status line for text that is not
a price, the lock on an item priced in the game and a tab's price as one
stored row (part of 4); a note's price and a tab name's price (5, 6); the
Priced filter (7); one number field (8); a manual shop update, "Copy shop data
to clipboard" and an automatic update, against forum pages captured from the
real site (part of 9).

Not covered by them, and checked by hand before a release:

- logging in, and logging in again after a restart (1): it needs a browser;
- the other ways a typed price is saved (choosing a type or a currency,
  selecting other rows, closing) and the red field (the rest of 4), which
  `tst_mainwindow` tests on a window wired by hand, not on the real
  application;
- that the real forum accepts the post, and what the thread shows
  afterwards (the rest of 9): the scenarios answer the post with a captured
  page, whatever was sent;
- a run on Windows (10);
- anything the offscreen platform does not draw.

The scripted network stands in for `NetworkManager::createRequest`, so the
User-Agent and the bearer token it adds to a request are not exercised
either, nor is the session cookie sent with a forum request; the shop
scenario checks that the cookie is set for the site.

### Golden files

`tst_golden` turns each real reply in `tests/fixtures/payloads` into items
by the application's own walk and writes out what they become: each item's
fields, mod table, tooltip and Path of Building text, and the forum shop that
lists the items. It compares that with the file of the same name in
`tests/fixtures/payloads-golden`; `census.txt` there counts the items without
a category, the mod lines that reach no mod name, the mod lines filed under
their own text because the game data has no name for them, and the property
values that are not one number, and lists them.

A failure prints the lines that differ. If the change is intended, rewrite
the files and review the difference like any other change:

```sh
QT_QPA_PLATFORM=offscreen ACQ_UPDATE_GOLDEN=1 ./build/dev/tests/acq_tests tst_golden
```

The suite runs on an excerpt of the game data,
`tests/fixtures/payloads-repoe`; its `README.md` says what the excerpt
holds, how `corpus_gamedata` cuts it, and when to run the suite on the whole
game data instead.

### Live capture (hand-run, not a test)

`live_capture` (`tests/live_capture.cpp`) builds the real `Application` on a
data directory, logs in to one league with the token stored there, refreshes
every tab once against the real API, and exits. `ctest` never runs it. The
replies land in that directory's user store as a refresh in the application
stores them.

It needs a live directory: a data directory outside the platform's
application-data location in which you have logged in once, with
`acquisition --data-dir <dir>`. The account is a local setting; do not write
it into the repository, an issue or a pull request.

```sh
./build/dev/tests/live_capture --data-dir <dir> --account '<name#1234>' --league <league> [--out <dir>]
```

The directory and the account can come from the environment instead of the
command line: `ACQ_LIVE_DIR` and `ACQ_LIVE_ACCOUNT`. Set them once, somewhere
git does not hold, and a command no longer has to name them.

With `--out` it leaves two files in that directory when the refresh has
ended: `transcript.txt`, what the item tree shows with every tab opened
(`tests/viewtranscript.h` describes the format), and `window.png`, a picture
of the window. They name your tabs, characters and items. The directory must
exist, and it is refused inside the source tree, where the files could be
committed, and inside the platform's application-data location.

It refuses a directory that is the platform's application-data location or
inside it, a directory logged in as another account, and a session with a
forum shop thread or shop auto-update on (`tests/liveguards.h`, pinned by
`tst_liveguards`).

It also refuses a branch that changes the refresh or network code. It asks
git which files of the source tree it was built from differ from the commit
where the branch left `origin/master` (committed, staged, unstaged and not
yet added), and refuses if one of them matches a `src/` path in the `paths:`
list of `.github/workflows/sanitizers.yml`. Whatever it cannot tell it
refuses: no git, no `origin/master`, no such list. It reads the tree as it is
when it runs, not as it was when it was built, so build before you run. The
comparison is with `origin/master` as last fetched: if that is stale, what
`master` has changed since can look like the branch's, so fetch first.

It never authenticates: when the stored token is not
usable it stops and says so. It does not start Sentry. Do not run it while
another Acquisition is using the same account: the rate-limit counters are
per account (N37).

### Windows from another machine

The pull-request job shows whether Windows builds and passes. To work on a
failure that only happens there, an agent session on another machine (or
anyone) can build and test on a Windows machine over SSH (#379). The machine
is reached by a host alias, `acq-win`, in the user's own `~/.ssh/config`. The
address, the user and the key are local settings: they stay in that file and
out of the repository, issues, pull requests and commits.

One-time setup of the Windows machine, the same tools as the pull-request job:

- OpenSSH Server, with key login and PowerShell as the shell SSH starts;
- Visual Studio 2022 Build Tools with "Desktop development with C++" (it
  brings the compiler, CMake and Ninja);
- Qt 6.11.3, the `msvc2022_64` kit with Qt Network Authorization, under
  `C:\Qt`;
- Git, and a clone of this repository at `C:\src\acquisition-qt`.

Build and test a branch that has been pushed. PowerShell goes on after a
program fails, hence the checks:

```sh
ssh acq-win '
  $vs = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
  Set-Location C:\src\acquisition-qt
  git fetch origin; if ($LASTEXITCODE) { exit 1 }
  git checkout --detach origin/<branch>; if ($LASTEXITCODE) { exit 1 }
  cmake --preset release -G Ninja "-DCMAKE_PREFIX_PATH=C:\Qt\6.11.3\msvc2022_64"; if ($LASTEXITCODE) { exit 1 }
  cmake --build --preset release --parallel 4; if ($LASTEXITCODE) { exit 1 }
  ctest --preset release
'
```

This is the `release` preset without `ACQ_WERROR`, as in the pull-request
job. Give a long run `-o ServerAliveInterval=30`. To run one suite, add
`-R '^tst_search$'` to `ctest`, or run the executable, which on Windows is
`build\release\acq_tests.exe` and needs Qt on the path:

```sh
ssh acq-win '
  Set-Location C:\src\acquisition-qt
  $env:PATH = "C:\Qt\6.11.3\msvc2022_64\bin;$env:PATH"
  $env:QT_QPA_PLATFORM = "offscreen"
  .\build\release\acq_tests.exe tst_search <function> -v2
'
```

Screenshots: on Windows the offscreen platform finds no fonts by itself, and
every letter in a picture is drawn as an empty box. Point Qt at the system's
fonts with `QT_QPA_FONTDIR`, as the pull-request job does, then copy the
pictures back:

```sh
ssh acq-win '
  ...the Visual Studio lines and Set-Location from above...
  $env:QT_QPA_FONTDIR = "C:\Windows\Fonts"
  $env:ACQ_TEST_SCREENSHOT_DIR = "C:\src\shots"
  ctest --preset release -R tst_appsmoke
'
scp "acq-win:C:/src/shots/*.png" <a directory outside the repository>/
```

Leave the clone as `git status` found it: change the code on the branch and
push, not on the Windows machine. There is no live directory on it, so
`live_capture` is not run there.

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

The build preset builds only `acq_tests`, the executable that holds every
suite, and leaves out the app. The
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

The same workflow (`.github/workflows/tests.yml`) builds and tests on Windows
as `Build + CTest (Windows, advisory)`. The ruleset does not require that job:
it shows a broken Windows build on the pull request that breaks it, and blocks
nothing.

## Where CI runs

`Build + CTest (Linux)` and the sanitizer job run either on the project's own
runner pool or on GitHub's `ubuntu-latest`. A `Pick Linux runner` job in each
workflow decides, by the same rule, and the first line of every run's summary
says which and why. The pool is a set of
throwaway virtual machines, each taking one job, running Ubuntu with Qt,
the apt packages, the locale and clang-format already installed.

- A pull request from a fork, or from Dependabot, always builds on
  `ubuntu-latest`.
- Everything else builds on the pool while the pool is up. The pool writes
  the time into the organization variable `SMALLCAT_LINUX_POOL` every minute,
  and `off` when it is stopped. A missing value, `off`, or a time more than
  five minutes old sends the build to `ubuntu-latest`.
- To build on `ubuntu-latest` on purpose, stop the pool's service on its
  host; stopping it sets `SMALLCAT_LINUX_POOL` to `off`. (Editing the variable
  by hand lasts only until the running pool's next beat.) Then use "Re-run
  all jobs": "Re-run failed jobs" keeps the earlier choice.
- On the pool, `Configure` builds the FetchContent dependencies from copies
  in the runner image instead of cloning them, when the image holds the
  exact repository and tag `CMakeLists.txt` asks for; a bumped or new
  dependency downloads as usual.
- On the pool, the steps that install Qt and packages are skipped, and
  `Check runner image` fails at once if the image's Qt or clang-format does not
  match the versions pinned in `tests.yml`. Refresh the image, or stop the
  pool.
- Both jobs build through ccache. On the pool the runner image carries warm
  caches, built from `master` when the image was last refreshed (weekly):
  `~/.cache/ccache` for the `release` preset and `~/.cache/ccache-asan` for
  the `asan` preset. On `ubuntu-latest` the caches are GitHub's, saved after
  each run; a sanitizer run there restores only one saved on its own branch or
  tag, so a pull request's first sanitizer run there is a cold build.

`Build + CTest (Windows, advisory)` always runs on GitHub's `windows-latest`.
It builds through ccache too, with the cache saved by GitHub after each run,
and configures with `-DACQ_MSVC_EMBEDDED_DEBUG_INFO=ON`: MSVC's `/Zi` writes
all debug information into one shared file, which a compiler cache cannot
store, so this job compiles with `/Z7` instead. Release builds keep `/Zi`.

Every other workflow runs on GitHub-hosted runners.

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

A tag matching `v*` starts `.github/workflows/release.yml`, which calls the
three and, when all have passed, creates one draft GitHub release with their
packages. Each can also be run by hand; it then only builds. How a release is
cut, and recovered when it fails, is in `RELEASE.md`. Every
workflow, these three and the test, CodeQL and sanitizer workflows, installs
the same Qt, 6.11.3, with the `qtnetworkauth` module. A Windows package built
with 6.12.0 failed at start-up with "SSL support is missing" (#370). The
Windows workflow runs the packaged binary with `--check-tls` and fails when it
finds no TLS backend that supports SSL.

The Windows installer removes what older packages left in the install folder
before it copies the new files (`[InstallDelete]` in `installer.iss`). A Qt
plugin left behind by an older package can stop the application: v0.18.10
reported "SSL support is missing" when installed over a folder that still held
a Qt 6.5 `tls\qopensslbackend.dll` (#440). The workflow installs the finished
installer over a folder seeded with such files, and fails if TLS does not work
afterwards or if any of them remain.

## Platform Notes

Windows release packaging uses MSVC 2022, `windeployqt`, the Visual C++
Redistributable, and Inno Setup via `installer.iss`.

macOS release packaging uses `macdeployqt` to produce a DMG. The current workflow
runs on `macos-latest` and names the uploaded artifact as an arm64 DMG.

GitHub sometimes cannot allocate an arm64 Mac and cancels the job after 15
minutes ("not acquired by Runner of type hosted"). The fallback is a run on the
maintainer's Mac: cancel the stuck run, start `release.yml` by hand on the
same tag with `dry_run` off and `macos_runner: laptop` (`RELEASE.md`), and
start a one-job VM there. That run rebuilds only macOS and takes the Linux and
Windows packages from the tag's own run. The job then
needs the label `smallcat-macos`. That VM is a throwaway copy of a macOS 26
image with Xcode 26.6 and Qt 6.11.3 at `QT_ROOT_DIR`, matching GitHub's
`macos-26-arm64` image, and runs the job as a user without admin rights. Tag
pushes never go there; only a hand-started run with `laptop` chosen does.

Linux release packaging uses `linuxdeploy` and `linuxdeploy-plugin-qt` to produce
an AppImage.
