# Pricing

Acquisition calls a price a **buyout**. Buyouts are stored in your data directory,
never in the game, and are what the [forum shop](forum-shop.md) publishes.

## Setting a price

Select an item, or a whole stash tab or character, in the item list. Folders
cannot be priced: with a folder selected the controls are disabled. Price the
tabs inside it instead. The same holds for the *All Items* row at the top of the
*By Item* view, which is not a tab. With nothing priceable selected (nothing,
or only folders and that row) the controls are disabled; if a refresh removes
the item you had selected, they are cleared and disabled until a row is
selected again. With several rows selected, the controls show the price of
the row you clicked last if it is still selected, otherwise of the first
selected row. The three controls under the list are:

1. **Buyout type**
2. **Currency**
3. **Value**

A choice of buyout type or currency applies at once, with the value in the
box; when a price needs a value and the box is still empty, the choice waits
for you to type one (a refresh does not undo it). A value you type is saved, to the rows that were selected when you began
typing it and to no others, when you:

- press Enter;
- choose a buyout type or currency (the price is saved once, with your choice);
- select other rows, by any means: clicking a row, switching to another search
  or opening a new one, closing the search, switching between *By Tab* and
  *By Item*, or a search change;
- close Acquisition, update the forum shops by hand
  (*Shop > Update forum shop(s)*), or copy the shop to the clipboard
  (*Shop > Copy shop data to clipboard*).

After it is saved, the box shows the stored price.

Nothing else saves it: moving between the three controls, a message window
appearing, or a refresh that keeps the same rows selected leaves it in the box.
So you can select several rows, type one value and press Enter to price them
all. A refresh that changes which rows are selected (one of them was sold, say)
saves the value as typed so far, to the rows it was typed for; rows that are
no longer there are skipped, and the status line says how many.
The *Price* and *Last Update* columns in the item table show the result.

The value must be a number greater than zero. Use a point or a comma as the
decimal separator (`2.5` or `2,5`). Text that is not such a number, for example
`79/20`, `abc` or `0`, is shown in red and is not saved: the rows keep their
previous price, and the status line says the text is not a price. The same
goes for Enter on an empty box and for a price with no currency chosen. What
you entered stays in the box to be corrected, also after closing the window
is cancelled or the shop is updated or copied; only when other rows are
selected instead is it dropped. A comma followed by exactly three
digits, such as `1,000`, is not saved either, because it could mean one
thousand or one; type `1000` or `1.0` instead.

Rows priced in the game (see below) keep their price when you price a selection
that includes them; the status line says so.

### When prices cannot be saved

If Acquisition cannot fully use its price database when you log in (for
example, a newer version of Acquisition last wrote it, or the file cannot be
opened or read), a warning says that prices cannot be saved in this session and
gives the location of the log file, which says why. Prices may still be shown,
but for the rest of that session they are read-only: an edit is not saved, and
the status line says *Price not saved*. Refreshes leave the stored prices alone,
the legacy import refuses to start, and the [forum shop](forum-shop.md) is not
updated. Check the log, fix the cause, and restart.

If the database rejects a single edit at another time, the status line says
*Price not saved* and the log file records the error. The notice goes away
after a few seconds, or as soon as an edit is saved.

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

Every currency the game's price dialog offers, plus three older ones. The
*Tag* is the word Acquisition posts to the forum shop; a price note may also
name the currency by any of the *Note words*, in any case.

| Currency | Tag | Note words |
|---|---|---|
| Orb of Alteration | `alt` | alt, alts, alteration, alterations |
| Orb of Fusing | `fuse` | fusing, fuse, fuses, fusings |
| Orb of Alchemy | `alch` | alch, alchs, alchemy |
| Chaos Orb | `chaos` | chaos |
| Gemcutter's Prism | `gcp` | gcp, gcps, gemcutter, gemcutters |
| Exalted Orb | `exa` | exalted, exa |
| Chromatic Orb | `chrom` | chrome, chrom, chromes, chromatic, chromatics |
| Jeweller's Orb | `jew` | jewellers, jew, jewel, jewels, jeweler, jewelers |
| Orb of Chance | `chance` | chance |
| Cartographer's Chisel | `chisel` | chisel, chisels, cartographer, cartographers |
| Orb of Scouring | `scour` | scour, scours, scouring |
| Blessed Orb | `blessed` | blessed |
| Orb of Regret | `regret` | regret, regrets |
| Regal Orb | `regal` | regal, regals |
| Divine Orb | `divine` | divine, divines |
| Vaal Orb | `vaal` | vaal |
| Perandus Coin | `coin` | coin, coins, perandus, p, shekel |
| Mirror of Kalandra | `mirror` | mirror, mir |
| Silver Coin | `silver` | silver |
| Orb of Annulment | `annul` | annul |
| Orb of Augmentation | `aug` | aug |
| Orb of Transmutation | `transmute` | transmute |
| Scroll of Wisdom | `wisdom` | wisdom |
| Portal Scroll | `portal` | portal |
| Armourer's Scrap | `scrap` | scrap |
| Blacksmith's Whetstone | `whetstone` | whetstone |
| Glassblower's Bauble | `bauble` | bauble |
| Offering to the Goddess | `offer` | offer |
| Dedication to the Goddess | `offer-dedication` | offer-dedication |
| Gift to the Goddess | `offer-gift` | offer-gift |
| Tribute to the Goddess | `offer-tribute` | offer-tribute |
| Lesser Eldritch Ember | `lesser-ember` | lesser-ember |
| Greater Eldritch Ember | `greater-ember` | greater-ember |
| Grand Eldritch Ember | `grand-ember` | grand-ember |
| Exceptional Eldritch Ember | `excep-ember` | excep-ember |
| Lesser Eldritch Ichor | `lesser-echor` | lesser-echor |
| Greater Eldritch Ichor | `greater-echor` | greater-echor |
| Grand Eldritch Ichor | `grand-echor` | grand-echor |
| Exceptional Eldritch Ichor | `excep-echor` | excep-echor |
| Facetor's Lens | `facetors` | facetors |
| Engineer's Orb | `engineers` | engineers |
| Infused Engineer's Orb | `infused-engineers-orb` | infused-engineers-orb |

The *Price* column sorts by currency first: Silver Coin highest, then Mirror
of Kalandra, Divine Orb, Exalted Orb, and so on down to Chromatic Orb. The
currencies from Orb of Annulment down the table have no rank of their own yet;
they sort below all the others, in the order of this table.

In the currency window (*Currency > List currency...*), the currencies from Orb
of Annulment down start with a ratio of 0, which leaves them out of the totals
until you give them a ratio.

## Prices set in the game

If a stash tab's name, or an item's note, contains a trade-site price string such
as `~b/o 5 chaos` or `~price 1 divine`, Acquisition imports it as a buyout with
source *game*. Game-sourced prices are **locked**: you cannot change them in
Acquisition, because the next refresh would overwrite the change. Edit the tab
name or item note in the game instead. A tab with a price of its own, set in
the game or in Acquisition, is always refreshed regardless of its check box, so
its prices stay current; this holds even while the tab is empty or not yet
fetched. Remove-only tabs are the exception: they are refreshed only when
checked.

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

If the current prices cannot be read, planning stops with an error rather than
treating every target as unpriced.

Run a full refresh of stashes and characters first so the matcher has current
data. Existing manual buyouts are never overwritten by the importer.
