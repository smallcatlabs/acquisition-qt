# Searching

## The main window

From top to bottom:

- **Search tabs.** Each tab is an independent search with its own filters. The
  trailing **+** tab creates a new one. Tab captions show the search name and the
  number of matching items, e.g. `Rings [42]`. Right-click a tab for
  **Rename Tab** and **Delete Tab**; middle-click closes a tab.
- **Search form.** Text fields, drop-downs, min/max ranges, and check boxes,
  grouped under *Offense*, *Defense*, *Sockets*, *Requirements*, *Misc*, and
  *Mods*. Searches update as you type.
- **View selector.** *By Tab* groups items under the stash tab or character they
  came from; *By Item* is a flat list.
- **Item list.** A tree of tabs/characters and their items. In *By Tab*, a stash
  folder is a row of its own with the tabs it holds underneath it. Folders start
  expanded; a folder you collapse stays collapsed in that search until you
  expand it again. While a search filter is active, a folder is shown only when
  at least one of its tabs has a match.
  The check box on a tab or character controls whether it is included in
  **Refresh checked tabs**. A folder's check box covers the tabs shown under it
  in the current search: it is ticked when all of them are, empty when none
  are, and partly filled when some are. Ticking or clearing it ticks or clears
  those tabs. (A tab that has a price, or holds a priced item, also shows a
  partly filled box: it is always refreshed so its prices stay current, and
  cannot be cleared.)
  Right-click for **Refresh Selected**, **Check Selected**, **Uncheck Selected**,
  **Check All**, **Uncheck All**, **Expand All**, and **Collapse All**. Check
  All and Uncheck All act on the tab and character rows shown, so they do
  nothing in *By Item*, whose only header is *All Items*. Tabs hidden by the
  current search keep their check state and are still refreshed by **Refresh
  checked tabs**. During a refresh the row you are on follows its item, even
  if the item moves to another tab. Other selected rows stay selected while
  their item stays where it is; an item that was sold or moved to another tab
  is no longer selected, and nor may one moved between the sub-tabs of a map
  or unique tab. A price you set applies to the rows selected at that moment.
- **Buyout controls** under the list — see [Pricing](pricing.md).
- **Item panel** on the right: the item's name, an in-game-style **Tooltip** tab,
  a plain **Text** tab, and **Hide**. **Upload to imgur** uploads the rendered
  tooltip and copies the URL to the clipboard; **Copy for Path of Building**
  copies the item in Path of Building's *Create custom* format.
- **Status bar** with the current activity, a **Rate Limit Status** button, and
  an **Update available** button when a newer release exists.
- **Event Log** at the bottom. Collapsed by default; its button changes to
  *N error(s)* or *N warning(s)* when something needs attention.

## Filters

All filters are ANDed together. Min/max fields are inclusive and either side may
be left blank.

Number fields (min/max, the socket colour counts, and a mod row's min and max)
take a single number: `5`, `-3`, `2.5` or `2,5` (a point or a comma as the
decimal separator), with spaces around it allowed; `1e3` is read as 1000. The
colour counts take whole numbers only. A comma followed by exactly three
digits, such as `1,000`, is not accepted, because it could mean one thousand
or one; type `1000` or `1.0`. Text that is not a number, such as `5%` or
`abc`, is shown in red and does not filter at all: the field counts as left
blank until its text is a number again. Hover over a red field to see what it
accepts. Check boxes match only when checked.

| Group | Filter | Matches |
|---|---|---|
| Top row | Tab | Substring of the stash tab or character name |
| | Name | Substring of the item name |
| | Type | Item category (from the game's item classes); `<any>` disables it |
| | Rarity | `<any>`, Normal, Magic, Rare, Unique, Unique (Foil), Any Non-Unique |
| Offense | Crit. | Critical Strike Chance |
| | DPS, pDPS, eDPS, cDPS | Total, physical, elemental, and chaos DPS |
| | APS | Attacks per Second |
| Defense | Armour, Evasion, Shield, Block | Armour, Evasion Rating, Energy Shield, Chance to Block |
| Sockets | Sockets | Number of sockets |
| | Links | Size of the largest linked group |
| | Colors R/G/B | At least this many sockets of each colour |
| | Linked R/G/B | At least this many of each colour in one linked group |
| Requirements | R. Level, R. Str, R. Dex, R. Int | Requirement values |
| Misc | Quality | Quality (unlisted quality counts as 0) |
| | Level | Gem level, or the *Level* property on other items |
| | Map Tier | Map tier |
| | ilvl | Item level |
| Flags | Alt. art | Alternate-art items |
| | Priced | Items with an active buyout |
| | Unidentified, Influenced, Crafted, Enchanted, Corrupted, Fractured, Split, Synthesized, Mutated | The corresponding item flag |
| Mods | Mods | Modifier rows; see [Mods and pseudomods](mods-and-pseudomods.md) |

*Influenced* means Shaper, Elder, Crusader, Redeemer, Hunter, or Warlord only;
fractured, synthesised, and eldritch items have their own filters.

*Priced* follows the prices: when you save a price, on an item or a tab, the
search is filtered again at once, so an item whose price you cleared leaves
it and an item that now has a price joins it. The rows you had selected that
are still shown stay selected. A refresh, an item's note or a tab's name
priced in the game, and an import of old prices are followed too. Other
searches that use *Priced* are filtered again when you next open them.

## Item table columns

Name · Price · Last Update · Q (quality) · Stack · Corr · Mast (crafted) · Ench ·
Inf (S/E/H/W/C/R for the six influences) · PD · ED (three elemental columns) ·
CD · APS · DPS · pDPS · eDPS · cDPS · Crit · Ar · Ev · ES · B · Lvl · ilvl.

Click a column header to sort. *Last Update* is when the price was last changed.
