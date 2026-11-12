# Pricing

Acquisition calls a price a **buyout**. Buyouts are stored in your data directory,
never in the game, and are what the [forum shop](forum-shop.md) publishes.

## Setting a price

Select an item, or a whole stash tab or character, in the item list. Folders
cannot be priced: with a folder selected the controls are disabled. Price the
tabs inside it instead. The three controls under the list are:

1. **Buyout type**
2. **Currency**
3. **Value**

Changes apply immediately to the selection. The *Price* and *Last Update* columns
in the item table show the result.

### Buyout types

| Type | Forum tag | Meaning |
|---|---|---|
| Buyout | `~b/o` | Asking price; buyers may still offer less |
| Fixed price | `~price` | Non-negotiable price |
| Current Offer | `~c/o` | The best offer received so far |
| No price | — | Listed in the shop with no price |
| [Ignore] | — | Excluded from the shop entirely |
| [Inherit] | — | Use the tab's buyout (the default for items) |

### Tab prices and inheritance

Pricing a stash tab or character prices every item in it that is still on
*[Inherit]*. Setting an item's own buyout overrides the tab's for that item;
setting it back to *[Inherit]* restores the tab price. *Current Offer* is not
meaningful for a tab and is logged as obsolete if it is found on one.

### Currencies

Alteration, Fusing, Alchemy, Chaos, Gemcutter's Prism, Exalted, Chromatic,
Jeweller's, Chance, Cartographer's Chisel, Scouring, Blessed, Regret, Regal,
Divine, Vaal, Perandus Coin, Mirror of Kalandra, Silver Coin. The forum tags are
the usual short forms (`chaos`, `exa`, `divine`, `fuse`, …).

## Prices set in the game

If a stash tab's name, or an item's note, contains a trade-site price string such
as `~b/o 5 chaos` or `~price 1 divine`, Acquisition imports it as a buyout with
source *game*. Game-sourced prices are **locked**: you cannot change them in
Acquisition, because the next refresh would overwrite the change. Edit the tab
name or item note in the game instead. Tabs priced in the game are always
refreshed regardless of their check box, so their prices stay current.

## Recovering prices from old versions

Before version 0.16, Acquisition stored buyouts in a different database. Two
items under the **Buyouts** menu bring them forward:

- **Recover legacy buyouts…** — choose a pre-0.16 data file. Acquisition matches
  the old buyouts against the current stashes and characters and shows how many
  it matched, how many are ambiguous, and how many are orphaned. You can
  **Import now**, or **Save plan for review…** to get an editable `.xlsx`
  workbook.
- **Import buyout plan…** — apply a saved (and optionally edited) plan. Each row
  has an *action* of `import` or `skip`.

Run a full refresh of stashes and characters first so the matcher has current
data. Existing manual buyouts are never overwritten by the importer.
