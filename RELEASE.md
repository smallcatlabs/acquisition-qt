# Releasing Acquisition

How a version goes from `master` to a published GitHub release. Building and
the CI checks themselves are described in `BUILD.md`.

## The version cycle

The version has one source: `project(VERSION ...)` plus `app_version_suffix`
in `CMakeLists.txt`.

1. Between releases `master` carries `-dev` (for example `0.18.10-dev`).
2. When a train is in, one commit changes the suffix to `-beta.1`; tag it. A
   fix found while it soaks becomes `-beta.2`, and so on.
3. Promote by clearing the suffix and nothing else; tag it.
4. The first change of the next train sets the next version with `-dev`.

The suffix change can ride in the last pull request of a train to avoid a
separate CI round. Sentry reports `-dev` builds as `development`, other
suffixes as `pre-release`, and no suffix as `production`.

## Cutting a release

1. Merge the commit that sets the version (`-beta.N`, or no suffix for a
   final release).
2. Tag that commit on `master` with a signed tag named `v` plus the version,
   and push the tag:

   ```sh
   git tag -s v0.18.10-beta.1 -m "v0.18.10-beta.1"
   git push origin v0.18.10-beta.1
   ```

3. The tag starts three workflows, independently:
   - **Release** (`release.yml`) checks the tag against the version in
     `CMakeLists.txt` and fails within a minute if they differ, or if the
     version ends in `-dev`. It then builds, tests and packages Linux, macOS
     and Windows in parallel (`build-{linux,macos,windows}.yml`, each also
     uploading its debug symbols to Sentry). When all three have succeeded,
     its last job creates the Sentry release and one **draft** GitHub release
     with generated notes and these files attached: the AppImage, the DMG,
     `acquisition_setup_<version>.exe` and `acquisition-<version>.pdb`. A
     version with a suffix is marked as a pre-release.
   - **CodeQL** (`codeql.yml`) scans the tag and fails on any open alert.
   - **Sanitizers** (`sanitizers.yml`) runs the test suite under ASan and
     UBSan, and the integration runner under LeakSanitizer.

   The release job does not wait for CodeQL or the sanitizers.

4. A final release is tagged only after its beta's CodeQL and sanitizer runs
   came back clean (`BUILD.md`, "Code Scanning"). A finding is fixed on
   `master` and gets a new beta.
5. Edit the draft's notes and publish it. Publishing is always done by hand.
   The draft carries `acquisition-<version>.pdb` for local debugging of a
   Windows crash dump; Sentry already holds the same symbols. Releases have
   been published both with and without it (0.18.9 has none): delete it from
   the draft before publishing if it should not ship.

## When something fails

- **One platform fails** (a flaky runner, a timeout): open the Release run
  and use **Re-run failed jobs**. Only the failed build runs again; the
  others keep their artifacts, and the release job runs once all three have
  passed.
- **The tag does not match the version**: delete the tag (`git push origin
  :refs/tags/<tag>`, and `git tag -d <tag>` locally), fix the version on
  `master` through a pull request, and tag again.
- **The release job fails after the draft was created** (for example while
  uploading a file): finish by hand with
  `gh release upload <tag> <files> --clobber`, or delete the draft and re-run
  the job. Re-running it while the draft exists fails at `gh release create`.
- **The release job fails before the draft exists**: re-run it. The Sentry
  step accepts an existing Sentry release.
- **Building one platform by hand**: each `build-*.yml` can be started from
  the Actions tab (`workflow_dispatch`). It builds, tests and uploads the
  same files as workflow artifacts but never creates a release; download them
  from that run and attach them with `gh release upload`.

## Trying the workflow without a tag

Running **Release** from the Actions tab on a branch (`workflow_dispatch`)
runs the version job (which only reads the version, because the ref is not a
tag) and all three builds, and skips the release job. It proves that the
workflows parse, that the three builds are called and pass with this
repository's secrets, and that each uploads its artifacts.

Only a real tag exercises the rest: the tag-against-version check, the
release job (the build versions agreeing, finding exactly one of each file in
the artifacts, the Sentry release, `gh release create --verify-tag` and the
pre-release flag) and the draft itself. Run on an existing tag with `dry_run`
left on, it also checks the tag but still creates nothing; an existing tag
only helps once it points at a commit that contains `release.yml`.
