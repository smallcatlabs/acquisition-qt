# ADR 0003: Rewrite or Evolve the Core and UI

## Status

Accepted (October 2026). The question this ADR posed in August is
decided; the decision and its consequences are below. The living
exploration that fed it (`docs/redesign/` on the `redesign` branch, the
Rust spike on `spikes/rust-playground`) is closed: the Rust spike became
its own repository, and the `redesign` branch is kept as history, never
merged.

## Context

ADR 0002 deferred the QML migration and chose an interior design
cleanup instead. That cleanup and the milestone work that followed
(items pipeline M1–M3, network redesign phases 1–5) had a side effect
that changed the option space: most of Acquisition's hard-won knowledge
came to live outside the code, in citable documents —

- `docs/design/network-ground-truth.md`: numbered, evidence-cited
  claims about how the Path of Exile API actually limits requests;
- `docs/cleanup/findings.md`: the project-wide register of design and
  correctness findings, open and resolved;
- the frozen specs and their review histories, which record accepted
  decisions alongside rejected alternatives and why.

The classic argument against rewrites — that the old code encodes
years of invisible fixes a new implementation would rediscover the
hard way (ADR 0001 made exactly this argument against a scratch QML
rebuild) — was therefore weaker here than it usually is. A new
implementation in any language inherits every ground-truth claim and
finding unchanged.

Two further inputs reopened the question ADR 0002 deferred: PR #192
(August 2026), which proposed `acquisitionctl`, a versioned local
control plane that treats agent access as a first-class surface; and
the maintainer's agreement with its author that evolving the C++/Qt
codebase and replacing it outright were both acceptable outcomes,
provided the externalized knowledge carried forward.

The question, as posed: what architecture, and consequently what
stack, should carry Acquisition forward — the existing C++23/Qt
Widgets application evolved in place, or a new implementation that
inherits the documented knowledge but not the code? Three options were
explored: evolve in place; a ground-up rewrite of core and UI on a new
stack; or a new core exposing an agent-shaped contract, with the UI
decision made separately against it.

What decided it was the Rust spike cut from this question in August
(`spikes/rust-playground`): by October it was a daemon, a CLI and an
MCP server running live against the Path of Exile API under the
existing OAuth registration, with its own decision registry, its
network, refresh, pricing, daemon-split and item-search slices closed,
and a test suite of several hundred tests. The rate-limit-core spike on
the `redesign` branch (PR #199) had already shown the header-driven
limiter could be rebuilt from the ground-truth claims alone.

## Decision

The maintainer's ruling, 2026-10-11, verbatim:

> The Rust build ships as Acquisition's agent surface by PoE2 1.0: the
> CLI, the MCP server and the daemon on Windows, macOS and Linux; GUI
> and other features follow.

Two companion rulings from the same conversation, verbatim:

- On coexistence: "Rust sits beside c++ now and replaces later". The
  replacement is a later ruling, not dated here.
- On repositories: two, not one. The C++ application is this
  repository, renamed `acquisition-qt` ("I like acquisition-qt for the
  legacy"); the Rust implementation is `smallcatlabs/acquisition`,
  carrying the spike branch's whole history ("i'm with you on clone").

In the terms of the question: the third option, a new core first with
the UI decision made separately against it. The GUI is the slice after
the agent release, and whether it is native or webview is that
slice's first question, not this ADR's.

## Consequences

- **This repository is the Qt application, maintained.** Releases of
  the C++ app continue on their own trains. The frozen specs in
  `docs/design/` remain full authority for all work here. Nothing in
  the Rust repository binds work on `master`.
- **The Rust implementation lives at
  <https://github.com/smallcatlabs/acquisition>**, branch `main`,
  licensed MIT OR Apache-2.0 (its C110). Its history before the root
  swap is this repository's `spikes/rust-playground` branch, which is
  deleted here and tagged `playground-end` there; the citations in
  its documents that point at this repository's commits are spelled
  `acquisition-qt@<hash>`.
- **Facts about GGG are authored there now.** The two ground-truth
  ledgers (`docs/design/network-ground-truth.md`,
  `docs/design/trade-ground-truth.md`) have copies in both
  repositories; new claims are written in the Rust repository and this
  repository takes copies, the reverse of the spike-era flow.
- **The `redesign` branch is history.** Its topic documents and the
  rate-limit-core spike result stay on the branch, cited as
  `redesign@<hash>` where a finding here still needs them (F74 cites
  the credential-custody topic). Nothing is squash-merged to `master`:
  the analysis was superseded by the implementation, and every finding
  that outlived it already graduated to an F-number.
- **The dual-track cost named in August is now the operating state.**
  The Rust repository's `brainstorming-notes/36-the-road-to-the-agent-release.md`
  holds the sequence to the agent release and the parks it fires;
  this repository holds none of that.

## Follow-Up

- The Rust repository: <https://github.com/smallcatlabs/acquisition>
  (`README.md` is the charter and crate index, `CONTEXT.md` the
  decision registry).
- Control-plane contract input: PR #192, whose contract the Rust
  daemon's protocol answers in its own terms.
- Predecessor decisions: ADR 0001 (superseded), ADR 0002 (accepted;
  its cleanup outputs are what made this ADR's option space viable).
