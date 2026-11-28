# Documentation Map

One entry per document, plus the traceability rules that connect them.
Each document states its own status and provenance in its header; when
this map and a header disagree, the header wins.

## User-facing

- `user/README.md` — index of the end-user guide (`user/*.md`): login,
  searching, mods and pseudomods, pricing, forum shop, currency, settings.
  Linked from the top-level README and the application's Help menu.

## Active

Specs and registers that govern work here.

- `design/network-redesign.md` — **frozen** spec (revision 11) for
  rate-limited networking: typed facade, coroutine pumps, gate.
- `design/items-pipeline-m2.md` — **frozen** spec for the streaming
  refresh signal. D1–D8 and D10 are live for the worker, manager and
  shop; D9's consumer side was retired by M3.
- `design/items-pipeline-m3.md` — **frozen** spec for the delta-native
  items model: bucket-scoped row operations, cached sort keys, the
  By-Item merge.
- `design/folder-tree.md` — spec for the folder level in the By-Tab tree
  (#234), implemented November 11, 2026. For By-Tab only, it supersedes
  M3's "a bucket is a top-level row".
- `design/legacy-buyout-import.md` — design for importing pre-0.16
  hash-keyed buyouts through an editable XLSX plan; implemented August
  14, 2026. Its evidence: `design/legacy-buyout-import-investigation.md`
  and `design/legacy-buyout-hash-check.py`.
- `design/network-redesign-phase5-verification.md` — home of the
  phase-5 evidence IDs (`M-*`, `W-*`, `P-*`, `I-*`) and the
  shutdown/retention harness; cited by F56, `BUILD.md` and
  `tests/lsan.supp`.
- `design/network-ground-truth.md` — N-claims: how the Path of Exile API
  limits requests, each with evidence.
- `design/trade-ground-truth.md` — T-claims: the trade site, forum link
  code, and the in-game price dialog and its notes.
- `cleanup/findings.md` — the register: open findings, standing
  constraints, the resolved ledger (F-numbers). The `cleanup/` path is
  historical and kept for its git history.
- `adr/0003-rewrite-vs-evolve.md` — **accepted** (October 2026): the Rust
  implementation becomes the agent surface by PoE2 1.0, the GUI to
  follow. This repository is the Qt application, maintained; the frozen
  specs remain full authority here.
- <https://github.com/smallcatlabs/acquisition> — the Rust
  implementation. Its `CONTEXT.md` is its decision registry. Ground-truth
  claims are authored there; the two ledgers above are copies.

## Historical

Finished work. Not authority; read it to resolve a citation.

- `archive/` — measurement results (`m1-m2-result`, `m2-m2-result`,
  `m1-m3-result`, `m3-sort-profile-result`) and review histories
  (`items-pipeline-m2-reviews`, `items-pipeline-m3-reviews`,
  `network-redesign-reviews`, `legacy-buyout-import-reviews`). The specs
  cite their round IDs (ER, IR, `R6-5`, …) and gate results. `.ignore`
  keeps the directory out of default searches; name it to search it. A
  bare file name inside these documents, such as `items-pipeline-m2.md`,
  means the file in `design/`.
- `design/items-pipeline.md` — the plan for the delta-native items
  pipeline; Milestones 1–3 shipped July 2026. Its one open follow-up, the
  filter-loop optimization, is recorded in M3's D7.
- `adr/0001-qml-ui-migration-strategy.md` — superseded: incremental QML
  migration proposal (its plan remains on the unmerged `prepare-qml`
  branch).
- `adr/0002-defer-qml-migration.md` — accepted: defer the QML migration,
  do the interior design cleanup instead.

## Citation rules

- Work is tracked in GitHub issues, grouped by milestone. The findings
  register holds standing constraints, design-level findings and the
  resolved ledger; an open finding names the issue that carries its work.
- Problems get F-numbers in `cleanup/findings.md`, are never renumbered,
  and are cited by number everywhere else.
- Ground-truth claims get N-numbers in `network-ground-truth.md` and
  T-numbers in `trade-ground-truth.md`, and are never renumbered;
  designs cite claims rather than restating evidence. When a claim
  falls, every design that cites it falls with it.
- Spec review findings get round-scoped IDs (ER, IR, R4-\*, …) recorded
  in the spec's review history under `archive/`; specs cite the IDs
  inline and record only current decisions.
- Phase-5 verification IDs (`M-*`, `W-*`, `P-*`, `I-*`) are local to the
  phase-5 verification contract. They organize completion evidence but
  are not project-wide finding IDs.
- Code and tests cite the spec decision (`D10 in
  docs/design/items-pipeline-m2.md`), not the archived evidence behind
  it.
- Retired documents live in git history, not in the tree: the cleanup
  plan, the phase-5 execution plan and its handoff notes, the QCoro spike
  (`spikes/qcoro/`), the M2 and M3 sequencing documents, the S1-M2 spike
  result, the plan's consumed M2/M3 inboxes, and the GGG email draft.
