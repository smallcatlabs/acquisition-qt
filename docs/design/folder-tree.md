# Folder Level in the Items Tree

Status: **ACCEPTED FOR IMPLEMENTATION** (November 11, 2026, on Tom's
go). Not externally reviewed. Changes after this point are recorded as
dated amendments with reasons, never silent edits. Decisions D8 and
D10's click rule depend on Qt behaviour that stage S0 must prove first;
S0's result is recorded here as an amendment.

Origin: <https://github.com/smallcatlabs/acquisition-qt/issues/234> —
tabs inside stash folders are not marked as such.

## Goal

In the By-Tab view, a stash folder is a collapsible row and the tabs it
holds are its children:

```
+ folder
  + tab            (bucket)
    - item
  + tab
+ tab              (not in a folder)
  - item
+ character
```

By-Item view is unchanged. An account with no folders renders exactly as
it does today.

## Relationship to the frozen specs

`items-pipeline-m3.md` describes a bucket as a top-level row. This spec
supersedes that one statement for By-Tab searches. Everything else in
M3 stands and binds this work: refresh-driven changes are row
operations and never a model reset, operations are scoped to the
affected bucket, child indexes carry a position-independent serial, and
the materialization, key-residency and sort rules are untouched. The
frozen specs are not edited.

## Evidence

Read-only inspection of a cached userstore, November 11, 2026
(validating account, Standard league: 16 folders, 77 tabs in folders):

- E1. A tab in a folder carries `folder` = the folder's stash id. No row
  has both `folder` and `parent`; `parent` is used only by Map/Unique
  sub-tabs.
- E2. Tab indexes are global. Each folder's tabs occupy the indexes
  immediately after the folder's own.
- E3. No folder has a `folder` of its own.
- E4. Every `folder` value names a row of type `Folder`.
- E5. Unique tabs occur inside folders; none had cached sub-tabs, so a
  Map/Unique tab with sub-tabs inside a folder is unobserved.

F49 (resolved ledger) already records that folder tabs arrive only
through the stash list and that a folder's own reply carried no
children and no items.

## Owner rulings (Tom, November 11, 2026)

- R1. Add the collapsible folder level.
- R2. Checking a folder row checks its tabs; unchecking unchecks them;
  a folder whose tabs are partly checked shows a third state.
- R3. Folders are expanded by default.
- R4. Tabs are grouped by folder id, not by index adjacency.
- R5. A folder's checkbox covers the tabs visible in the current search
  only.
- R6. Refresh Selected on a folder refreshes the folder's checked
  members.
- R7. No buyouts on folders.
- R8. Items under a folder are not rendered; warn if any are present.
- R9. Folder tabs keep being requested; the request helps detect API
  changes.

## Assumptions awaiting a ruling

Made by the spec author to keep the build unblocked. Each is a
one-function change if overruled.

- A1. In R6, "members" means the members visible in the current search
  (the R5 scope). The folder tab itself is sent with them (R9).
- A2. A folder has no stored refresh flag. On Refresh Checked the worker
  requests a Folder tab when at least one of its member tabs is in the
  checked selection.
- A3. R3 applies to folder rows. Tab buckets keep today's rule
  (collapsed in an unfiltered search, expanded in a filtered one).
- A4. R8 is met by discarding a folder's items in the worker (D12), so
  they are absent from By-Item view and the item counts as well as from
  By-Tab.

## Terms

- **Bucket row**: the index of a bucket in the active mode's bucket
  vector. It stays the bucket handle throughout `Search`, `ItemsModel`
  and `MainWindow`.
- **Folder bucket**: a By-Tab bucket whose location is a stash of tab
  type `Folder`.
- **Member**: a stash location whose folder id equals a folder's id.
- **Nested bucket**: a bucket currently rendered under a folder bucket.

## Decisions

### D1. `ItemLocation` carries folder membership

`ItemLocation` gains the folder id (from `poe::StashTab::folder`, empty
when absent) and an is-folder test (stash type `Folder`).
`rebaseTabMetadata` copies the folder id, so a tab moved between folders
in game is a metadata change. The folder id takes no part in
`operator==`, `operator<`, `GetLegacyHash` or `GetHeader`. No schema
change and no network change: the field is already parsed and already
cached (E1).

### D2. Tree shape

- Depth is fixed at one level. A folder bucket is always top-level,
  whatever its own folder id (E3).
- A non-folder stash bucket is nested exactly when its canonical
  location's folder id names a folder bucket present in the bucket
  vector. Otherwise it is top-level, as today.
- Character buckets and the By-Item flat bucket are top-level.
- **Folder presence.** In an unfiltered search a folder bucket is
  present like any listed tab (already true today). In a filtered search
  a folder bucket is present exactly while at least one of its member
  buckets is present. The folder's location comes from the canonical
  inventory or the published tab list; a folder known to neither is
  absent and its members are top-level.
- A folder bucket holds no items (D12 keeps them out of the pipeline).

### D3. Tree order

The By-Tab bucket vector is kept in depth-first tree order at all
times: each folder bucket is followed immediately by its nested
buckets. The order is lexicographic on (group anchor, header before
members, self), each compared with the existing `bucketDisplayLess`,
where a nested bucket's group anchor is its folder bucket and every
other bucket is its own anchor. `bucketDisplayLess` is total, so this
is total. One function decides a location's nesting parent and is used
by both the ordering and the row mapping.

### D4. Row mapping

`RebuildRowLookups` additionally derives, for the active mode: the
bucket rows of the top-level buckets, and for each bucket row its
parent bucket row (or none) and its row within that parent. Cost stays
O(tab count) per structural operation, which M3 already accepted.

Two conversions replace every direct `index(bucket_row, 0)`,
`index.row()` and `index.parent().row()` use:

- bucket row → that bucket's header index;
- model index → bucket row (for a header) or owning bucket row plus item
  row (for an item).

### D5. Index identity

`internalId` is 0 for a top-level row and otherwise the serial of the
parent bucket. The row kind follows from the parent: a row under a
folder bucket is a bucket header; a row under any other bucket is an
item. Item indexes are therefore unchanged from M3. A serial that no
longer resolves yields an invalid parent and empty data.

### D6. Header rendering

A nested bucket header renders exactly as a top-level tab header does
today. A folder header renders its header text and stash colour, its
derived check state (D10) and no buyout suffix (D11).

### D7. Structural operations

All are row operations. The required end state after any delta or
snapshot is the tree a fresh `FilterItems` over the same published
state would build.

- **Insert.** A bucket is inserted under its nesting parent at its tree
  position. In a filtered search, inserting a bucket whose folder is
  absent but known inserts the folder bucket first. Inserting a folder
  bucket moves any present top-level buckets that are its members under
  it. `DeltaApplication` reports every inserted bucket row, as
  `SnapshotReconciliation` does.
- **Remove.** A bucket is removed from under its parent. In a filtered
  search, removing a folder's last present member removes the folder
  bucket. A folder bucket that still has present members is never
  removed with them: its members are first moved to the top level.
- **Emptiness.** "A filtered search hides empty buckets" means no items
  for a non-folder bucket and no present members for a folder bucket.
  Every site that applies the rule goes through one predicate. An empty
  delta for a folder bucket therefore never removes a folder that has
  members.
- **Metadata.** After `SetLocation`, the bucket is repositioned under
  tree order: within its parent when only its index changed, across
  parents when its folder id changed (D8). A folder bucket's move
  carries its nested run in the vector. The presence rule is applied to
  the folder left and the folder entered.
- **Final snapshot.** `ReconcileFinalSnapshot` converges to the same
  tree. Surviving buckets' metadata is refreshed and membership settled
  before any folder bucket is removed. A clean snapshot still pays one
  O(tab count) ordered check and no moves.

### D8. Cross-parent move

A change of nesting parent is `beginMoveRows` between parents. The
`Bucket` object survives the move with its serial, items, keys, sorted
flag and expanded mark. If S0 shows that `QTreeView`, persistent
indexes or `QAbstractItemModelTester` do not hold up under a
cross-parent move, the fallback is remove-then-insert of the same
`Bucket` object, with `MainWindow` re-expanding it from its expanded
mark and the existing selection-intent pass re-adopting any selected
item.

### D9. View

- Every site in `MainWindow` that decides "bucket or item" from
  `parent().isValid()`, or treats a top-level row number as a bucket
  row, uses the D4 conversions instead: the expand and collapse
  handlers, Expand All, Collapse All, expansion save and restore, the
  scroll anchor, current-row handling, buyout edit, selection
  reselect and reconcile, and the insertion-expansion tails.
- **Folder expansion.** Folders are expanded after a reset and when
  inserted. Each search keeps the set of folder keys the user
  collapsed; a folder in that set stays collapsed across resets and
  re-insertion. The set is maintained from the view's expanded and
  collapsed signals.
- **Tab expansion** keeps today's rules, applied to every tab bucket
  header whether top-level or nested.
- Expand All and Collapse All apply to every bucket header, folders
  included. Expansion calls address bucket headers only; nothing stores
  expansion state for item rows.
- Expanding or collapsing a folder materializes nothing. A tab expanded
  inside a collapsed folder stays materialized; no new eviction rule.
- Selecting a folder header shows its header text in the details pane
  and disables the buyout widgets (D11).

### D10. Refresh checkboxes

- A folder's check state is derived from its member buckets present in
  the current search (R5): all refreshing → checked, none → unchecked,
  otherwise partially checked. A refresh-locked member counts as
  refreshing. A folder with no present members shows no checkbox.
- Checking or unchecking a folder writes that value to each present
  member that is not refresh-locked, then repaints the members and the
  folder. Any value other than unchecked means checked.
- Changing a nested tab's checkbox repaints its folder header.
- Check Selected and Uncheck Selected on a folder row behave as a
  click. Check All and Uncheck All write to every bucket except folder
  buckets.
- `refreshCheckStates` covers every header row, nested ones included.
- Known overlap, accepted: a tab row already uses the partially-checked
  glyph for "refresh-locked", so the glyph means locked on a tab and
  mixed on a folder. Unchecking a folder that holds a locked tab leaves
  the folder partially checked.
- Refresh Selected on a folder row sends the folder and its present
  members whose refresh state is checked (R6, A1).
- The worker's checked selection includes a Folder tab when at least
  one of its member tabs is in the selection (A2, R9). Folder ids are
  never written to or read from the stored refresh flags.

### D11. No buyouts on folders

With a folder header current the buyout widgets are disabled. A buyout
edit over a multi-selection skips folder rows. There is no cascade to
members. A tab buyout stored against a folder id by an earlier version
is left in the store and not rendered.

### D12. Worker guards

- Items carried by a Folder-type stash, in a live reply or a cached
  row, are discarded with one warning naming the tab and the count
  (R8). They never enter the item store, so the two view modes cannot
  disagree about them.
- A tab listed under a folder's `children` whose `folder` is missing or
  names another folder logs a warning. No fallback is applied.

## Stages

One branch. Each stage is one or more commits, each carrying its tests,
with the whole suite passing at every commit.

- **S0. Qt assumptions, as permanent tests.** (a) A cross-parent
  `beginMoveRows` of a row that has children, under `QTreeView` with
  `QAbstractItemModelTester` attached: expansion of the moved row and
  persistent indexes of its children survive. (b) Clicking a
  partially-checked checkbox on an item that is user-checkable but not
  user-tristate delivers a checked value to `setData`. Record both
  results in this spec; (a) selects between D8's two mechanisms.
- **S1. Data and worker guards.** D1 and D12.
- **S2. Conversion helpers with an identity mapping.** D4's two
  conversions replace the direct uses, with top-level row equal to
  bucket row. No behaviour change; no existing test expectation is
  edited.
- **S3. The tree.** D2, D3, D4, D5, D6, D7, D8.
- **S4. The view.** D9.
- **S5. Checkboxes, Refresh Selected, buyouts.** D10, D11.
- **S6. Documentation.** `docs/README.md` map entry, the user guide
  where it describes the items tree, and this spec's status line.

## Acceptance criteria

- Existing tests pass with no change to their expectations. Edits that
  only follow a renamed or re-signatured API are allowed. An account
  with no folders produces the same model as before.
- `QAbstractItemModelTester` in fatal mode stays attached through every
  folder scenario below.
- No model reset occurs on a refresh path with folders present.
- **Convergence.** After each scenario, in both a filtered and an
  unfiltered search, the model's tree equals the tree of a fresh
  refilter of the same published state:
  1. a member arrives in a filtered search whose folder is absent;
  2. a folder's last visible member leaves a filtered search;
  3. an empty delta arrives for a folder that has members;
  4. a tab moves into a folder, out of a folder, and between folders;
  5. a folder's index changes;
  6. a folder is deleted at the snapshot while members survive;
  7. a member is discovered before its folder;
  8. a tab's folder id names no known folder;
  9. the view mode switches to By-Item and back after deltas;
  10. a nested bucket is expanded, sorted and re-sorted.
- A selected item in a nested bucket stays selected, or is re-adopted,
  through scenarios 4 and 5.
- D9, D10 and D11 each have tests at the `MainWindow` level: default
  and sticky folder expansion, the three derived check states, the
  cascade, the locked-member case, the locations Refresh Selected
  sends, and the disabled buyout widgets.
- D12's two warnings and the A2 selection rule have worker-level tests.
- The item-row paths of `data`, `index` and `parent` stay O(1) expected;
  structural operations stay O(tab count). Where the existing M3
  benchmark targets are runnable, their before and after numbers are
  recorded in the branch's final report.

## Out of scope

- Showing the folder name in By-Item view, the details pane or the
  tab-name filter.
- A buyout that cascades from a folder to its tabs.
- Stopping the request for Folder tabs (R9).
- More than one level of nesting.

## Amendments

Dated, with reasons. The decisions above stay as written; where an
amendment changes one, the amendment governs.

- **November 11, 2026 — S0 result.** Both Qt assumptions hold on Qt
  6.12.0, pinned permanently by `tst_qtassumptions`:
  (a) `crossParentMoveKeepsExpansionAndChildIndexes`: a `beginMoveRows`
  of a row with children between two parents, from a parent to the top
  level, back, within one parent, and of a top-level row carrying its
  subtree, under `QTreeView` with `QAbstractItemModelTester` attached,
  keeps the moved row expanded, keeps its children's persistent indexes
  valid and pointing at the same rows, and keeps a selected child
  selected. The test model encodes indexes the way `ItemsModel` does
  (internalId 0 at the top level, otherwise the parent's serial). A
  mutant replacing the move with remove-then-insert fails the test.
  D8 therefore uses `beginMoveRows` between parents; the
  remove-then-insert fallback is not built.
  (b) `partialCheckboxClickDeliversChecked`: a mouse click on the check
  indicator of a partially-checked, user-checkable, non-tristate item
  delivers `Qt::Checked` to `setData`, and the next click delivers
  `Qt::Unchecked`. D10's click rule stands as written.
