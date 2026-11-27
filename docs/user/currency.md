# Currency

Every refresh counts the currency items in your stashes and characters.

## Currency overview

**Currency → List currency...** opens a table with one row per currency type
(the twenty types listed in [Pricing](pricing.md#currencies)) and the columns:

- **Name** and **Count**
- **Amount a chaos Orb can buy** / **Value in Chaos Orb**
- **Amount a Divine Orb can buy** / **Value in Divine Orb**

The *Amount … can buy* fields are ratios you enter yourself (for example, how
many Alteration Orbs one Chaos buys); Acquisition does not fetch market rates.
Totals at the bottom show **Total Chaos Orbs**, **Total Divine Orbs**, and
**Total Scrolls of Wisdom**. The **show chaos ratio** and **show divine ratio**
check boxes hide or reveal the ratio columns and are remembered between runs.
If the saved ratios cannot be read when you log in, the defaults are shown and
nothing is saved that session, so the saved ratios are kept; the log says why.

Before version 0.18.12 the second pair of columns was in Exalted Orbs. The
first time a league is opened with 0.18.12 or later, the ratios you entered
against the Exalted Orb are restated against the Divine Orb, using your own
figure for how many Divine Orbs an Exalted Orb buys: if an Exalted Orb bought
0.1 Divine Orbs and 15 Chaos Orbs, a Divine Orb now buys 150 Chaos Orbs. If
that figure was 0, there is nothing to restate them with and the Divine
ratios start at 1. Check the column once after updating.

The dialog stays open and updates as refreshes complete.

## Export

**Currency → Export to CSV...** writes a history file (default name
`acquisition_export_currency.csv` in your home directory) with a `Date` column,
a `Total value` column, and one column per currency type. Each row is a snapshot
taken at a refresh, so the file can be charted to see your wealth over time.
`Total value` is in Divine Orbs; rows recorded before version 0.18.12 are in
Exalted Orbs.
