# Documentation Map

One line per document, plus the traceability rules that connect them.
Each document also states its own status and provenance in its header;
when this map and a header disagree, the header wins.

## User-facing

- `user/README.md` — index of the end-user guide (`user/*.md`): login,
  searching, mods and pseudomods, pricing, forum shop, currency, settings.
  Written from source in August 2026; linked from the top-level README and
  the application's Help menu.

## Active

- `design/legacy-buyout-import.md` — deliberately minimal design for
  recovering pre-0.16 hash-keyed buyouts into the id-keyed store.
  Revised August 14, 2026 to a plan/apply split around an editable XLSX
  plan file (QXlsx); tracer bullet implemented, revision not yet built.
- `design/legacy-buyout-import-investigation.md` — evidence record for
  that design (August 13, 2026): old/new formats, the validated
  hash-to-id join (100% match on a real 0.15.0 database), defects in the
  existing `src/legacy/` bridge code, and where old files survive.
  `design/legacy-buyout-hash-check.py` is the validation script.
- `design/legacy-buyout-import-reviews.md` — review history for that
  design's implementation: round-scoped findings (`R1-*`) with status,
  cleanup notes, and conventions follow-ups.
- `design/folder-tree.md` — spec for the collapsible folder level in the
  By-Tab items tree (issue #234): owner rulings R1–R9, assumptions A1–A4
  (approved), decisions D1–D12, stages S0–S6 and acceptance criteria,
  with dated amendments (S0's Qt results among them). Implemented and
  merged November 11, 2026 (pull request 235); not externally reviewed.
  Supersedes, for By-Tab searches only, M3's "a bucket is a top-level
  row".
- `design/items-pipeline.md` — plan for the delta-native redesign of the
  items refresh pipeline. Milestone 1 shipped July 2026; M2 merged and
  M3 implemented July 30–31, 2026, each against its own frozen spec.
  The M1-M2 status-widget burst measurement ran July 31, 2026 (gate
  fired; coalesce built — `design/m1-m2-result.md`).
- `design/items-pipeline-m2.md` — **frozen** spec (revision 9,
  July 29, 2026, amended post-freeze the same day) for Milestone 2,
  the streaming refresh signal: D-numbered decisions consuming the
  plan's M2 inbox, with an input-traceability table. Implemented and
  shipped (PR #185, July 30); D9's consumer-side machinery is since
  retired by M3 — D1–D8 and D10 remain the live authority for the
  worker/manager/shop side. Review rounds
  1–7 are incorporated (round 4 was an in-repo audit); the S1-M2
  spike selected D9's outcome (a) with S = 60 s (result recorded in
  D9; the spike evidence doc is retired). M2-M2 ran at its stage-4
  checkpoint (missed, remedied by the source-keyed stores, rerun
  passed — `design/m2-m2-result.md`); M1-M2 ran July 31, 2026
  (stuttered, remedied by the D10 dialog-side coalesce, rerun
  passed — `design/m1-m2-result.md`).
- `design/items-pipeline-m2-reviews.md` — that spec's review history:
  round-scoped finding tables (`R1-*`, …) with verdicts and
  resolutions, round narratives, and the revision log.
- `design/items-pipeline-m3.md` — **frozen** spec (revision 4,
  July 30, 2026; rounds 1–3's eighteen findings incorporated, review
  series closed) for Milestone 3, the delta-native items model:
  bucket-scoped model operations replacing the refresh-path
  reset, precomputed cached sort keys plus deferred bucket sorting
  (the July 30 lever hold point), the By-Item per-delta merge, and
  the F67 tie-break fix. Pre-spec evidence:
  `design/m3-sort-profile-result.md` (S1-M3 spike, run on the
  never-merged branch `spike/m3-sort-profile`, including the
  hold-point key-memory extension). The M1-M3 budget gate PASSED at
  S7 (July 31, 2026) — stage rows, the S5 miss and its A′ remedy,
  and the complete table: `design/m1-m3-result.md`.
- `design/items-pipeline-m3-reviews.md` — that spec's review history
  and revision log; rounds 1–3 recorded (R1-1…R1-8, R2-1…R2-6,
  R3-1…R3-4, all accepted) and the freeze.
- `design/network-redesign.md` — accepted, frozen spec for the
  rate-limited networking redesign (typed facade, coroutine pumps, gate),
  revision 11. Records current decisions only; cites review finding IDs inline.
- `design/network-redesign-reviews.md` — that spec's decision history:
  review-round finding tables (ER, IR, R4–R7, S1/S2), round narratives,
  reversal records, and the revision log, including the phase-5
  planning-readiness amendment.
- `design/network-redesign-phase5-verification.md` — phase-5 verification
  contract: the stable evidence-ID registry (`M-*`/`W-*`/`P-*`/`I-*`), worker
  preservation matrix, and the full-chain shutdown/retention harness. Retained
  as the permanent home of the evidence IDs the F56 resolved-ledger entry cites
  by name; the transient execution plan and per-session LSan handoff notes that
  accompanied it were removed after phase 5 landed (git history retains them).
- `design/network-ground-truth.md` — living ledger of numbered claims
  (N1, N2, …) about how the Path of Exile API actually limits requests,
  each with cited evidence. Designs are derived from these claims.
- `design/trade-ground-truth.md` — its companion for listing items for
  sale (T1, T2, …): the trade site's channels and bulk-ratio grammar,
  the forum link code, the in-game price dialog's vocabulary and note
  shapes, and what the API reports of it. The Rust spike's pricing
  rulings cite these.
- `adr/0003-rewrite-vs-evolve.md` — **accepted** (October 2026): the Rust
  implementation ships as Acquisition's agent surface by PoE2 1.0, the
  GUI to follow. This repository is the Qt application, maintained; the
  frozen specs above remain full authority for work here.
- <https://github.com/smallcatlabs/acquisition> — the Rust implementation,
  once the `spikes/rust-playground` branch here; its history continues
  there from the tag `playground-end`, and its `CONTEXT.md` is its decision
  registry. Ground-truth claims are authored there now and copied here:
  the two ledgers above are the copies.
- `cleanup/findings.md` — project-wide register of design/correctness
  findings (F1, F2, …): open findings, standing constraints, and a
  resolved ledger. The `cleanup/` path is historical — the register
  outlived the July 2026 cleanup it was created for, and stays put so
  its git history remains easy to browse.

## Historical

- `adr/0001-qml-ui-migration-strategy.md` — superseded: incremental QML
  migration proposal (its implementation plan remains on the unmerged
  `prepare-qml` branch).
- `adr/0002-defer-qml-migration.md` — accepted: defer the QML migration,
  do the interior design cleanup instead.

## Citation rules

- Problems get F-numbers in `cleanup/findings.md`, are never renumbered,
  and are cited by number everywhere else.
- Ground-truth claims get N-numbers in `network-ground-truth.md` and
  T-numbers in `trade-ground-truth.md`, and are never renumbered;
  designs cite claims rather than restating evidence. When a claim
  falls, every design that cites it falls with it.
- Spec review findings get round-scoped IDs (ER, IR, R4-\*, …) recorded
  in the review-history file; specs cite the IDs inline and record only
  current decisions.
- Phase-5 verification IDs (`M-*`, `W-*`, `P-*`, `I-*`) are local to the
  phase-5 verification contract, which is retained as their permanent home
  (the F56 ledger cites them). They organize completion evidence but are not
  permanent project-wide finding IDs.
- Retired documents (the cleanup plan, the phase-5 execution plan and its
  per-session handoff notes, the phase-0 QCoro spike `spikes/qcoro/` — retired
  July 2026 once the network redesign was no longer in flight; its S1/S2
  findings live in the review history — and superseded spec text) live in git
  history, not in the tree. Retired August 1, 2026 in the pre-merge sweep of
  the M3 branch: the M2 and M3 implementation-sequencing docs (both executed
  and verified — the M3 supersession-map verification is recorded in
  `design/m1-m3-result.md` §S8), the S1-M2 spike result doc (its result and
  numbers are recorded in the M2 spec's D9 and open-items entry), the parent
  plan's consumed M2/M3 input inboxes (each spec's traceability table records
  them), and the GGG email draft (the ask was dropped).
