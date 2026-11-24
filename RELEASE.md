# Releasing Acquisition

How a version goes from `master` to a published GitHub release. Building and
the CI checks themselves are in `BUILD.md`.

## The version

The version has one source: `project(VERSION ...)` plus `app_version_suffix`
in `CMakeLists.txt`. `.github/scripts/version.sh` reads it for every release
workflow.

1. Between releases `master` carries `-dev` (for example `0.18.12-dev`).
2. When a train is in, one commit changes the suffix to `-beta.1`; tag it. A
   fix found while it soaks becomes `-beta.2`, and so on.
3. Promote by clearing the suffix and nothing else; tag it.
4. The first change of the next train sets the next version with `-dev`.

Each step is a pull request titled `chore: set the version to <version>` that
changes only those lines (#415, #428, #449, #451). The suffix change can ride
in the last pull request of a train instead, to save a CI round (0.18.10's
`-beta.2` and `-beta.3` did). Sentry reports `-dev` builds as `development`,
other suffixes as `pre-release`, and no suffix as `production`.

## Cutting a beta or a final release

1. Merge the version pull request, after everything meant to ride in it.
2. Tag a `master` commit whose `CMakeLists.txt` reads exactly that version
   (normally the version pull request's merge commit), with a signed tag, and
   push it:

   ```sh
   git tag -s v0.18.12-beta.1 -m "Acquisition v0.18.12-beta.1"
   git push origin v0.18.12-beta.1
   ```

3. The tag starts three workflows:
   - **Release** (`release.yml`). Its first job reads the version and fails
     within a minute if the tag is not `v<version>` or the version ends in
     `-dev`. Then Linux, macOS and Windows build, test and package in
     parallel (`build-{linux,macos,windows}.yml`, each also uploading its
     debug symbols to Sentry). When all three have passed, the last job
     checks they report the same version, creates the Sentry release, and
     creates one **draft** release with generated notes and three files: the
     AppImage, the DMG and `acquisition_setup_<version>.exe`. A version with
     a suffix is marked as a pre-release.
   - **CodeQL** (`codeql.yml`), which fails on any open alert.
   - **Sanitizers** (`sanitizers.yml`).

   The release job does not wait for CodeQL or the sanitizers.
4. **A final release is always preceded by a beta.** Tag the final only
   after the beta's CodeQL and sanitizer runs came back clean (`BUILD.md`,
   "Code Scanning") and the maintainer has tried the beta's packages. A
   finding is fixed on `master` and gets a new beta. The final's pull request
   clears the suffix and nothing else, so the final's tree is the beta's.
5. **Publish by hand.** Read and edit the draft's notes, then publish. A
   published pre-release is offered to users by the update checker
   (`src/util/updatechecker.cpp`); a beta's draft may also be left
   unpublished (the 0.18.10 betas were). A final is published as the latest
   release.
6. The next train starts by setting the next version with `-dev`.

**The `.pdb`.** Releases carry the three packages and nothing else: 0.18.9,
0.18.10, 0.18.11-beta.1 and 0.18.11 were published without
`acquisition-<version>.pdb` (it was deleted from each draft by hand; 0.18.7
was the last to ship it). The release job therefore does not attach it. The
Windows symbols are in Sentry, and the `.pdb` is in the Windows build's
workflow artifact for 30 days.

## When something fails

- **The version job fails**: nothing was built. Delete the tag
  (`git push origin :refs/tags/<tag>` and `git tag -d <tag>`), fix the
  version on `master` through a pull request, and tag again.
- **One platform fails for a passing reason** (a runner, a download, a
  timeout): open the Release run and use **Re-run failed jobs**. Only the
  failed build runs again, and the release job runs once it passes.
- **A platform fails for a real reason** (a test, packaging): nothing was
  released. The fix goes into `master` with the next `-beta.N`, which is
  tagged.
- **GitHub cannot allocate a Mac** (the macOS job is cancelled after 15
  minutes, "not acquired by Runner of type hosted"): try **Re-run failed
  jobs** once. If it is still stuck, cancel the run, then start Release by
  hand (Actions, Release, Run workflow) on the tag, with `dry_run` off and
  `macos_runner: laptop`, and start the one-job VM on the maintainer's Mac
  (`BUILD.md`, "Platform Notes"). That run builds all three platforms again
  and creates the draft. Only a run started by hand, which needs write
  access to the repository, can send a job to that machine; a tag push never
  does.
- **The release job fails before the draft exists**: re-run it. The error
  names what it expected and found. The Sentry step accepts a Sentry release
  that already exists.
- **The release job fails after the draft exists**: attach what is missing
  with `gh release upload <tag> <files> --clobber`, or delete the draft
  (`gh release delete <tag>`, which keeps the tag) and re-run the job.
- **The workflow cannot work at all**: download the run's artifacts
  (`gh run download <run-id> -D dist`) and create the draft by hand:
  `gh release create <tag> --draft --generate-notes --verify-tag`, with
  `--prerelease` for a suffix, and the three packages.

A single platform can also be built from its own workflow (Actions, Build &
Package, Run workflow). That run uploads its symbols to Sentry and its files
as workflow artifacts; it never creates or changes a release.

## Trying the workflow without a tag

Starting Release by hand on a branch, with `dry_run` on (the default), runs
the version job (which only reads the version, because the ref is not a tag)
and all three builds, and skips the release job. Note that the builds upload
symbols to Sentry. Only a tag exercises the tag check, the release job and
the draft.
