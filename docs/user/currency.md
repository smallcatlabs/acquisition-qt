# Currency

Every refresh counts the currency items in your stashes and characters.

## Currency overview

**Currency → List currency...** opens a table with one row per currency type
(the twenty types listed in [Pricing](pricing.md#currencies)) and the columns:

- **Name** and **Count**
- **Amount a chaos Orb can buy** / **Value in Chaos Orb**
- **Amount an Exalted Orb can buy** / **Value in Exalted Orb**

The *Amount … can buy* fields are ratios you enter yourself (for example, how
many Alteration Orbs one Chaos buys); Acquisition does not fetch market rates.
Totals at the bottom show **Total Chaos Orbs**, **Total Exalted Orbs**, and
**Total Scrolls of Wisdom**. The **show chaos ratio** and **show exalt ratio**
check boxes hide or reveal the ratio columns and are remembered between runs.

The dialog stays open and updates as refreshes complete.

## Export

**Currency → Export to CSV...** writes a history file (default name
`acquisition_export_currency.csv` in your home directory) with a `Date` column,
a `Total value` column, and one column per currency type. Each row is a snapshot
taken at a refresh, so the file can be charted to see your wealth over time.
