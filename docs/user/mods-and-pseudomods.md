# Mods and pseudomods

## The Mods filter

The **Mods** section of the search form holds any number of rows. Each row is a
searchable drop-down of modifier names plus optional **min** and **max** boxes.
Click **Add mod** for another row and **X** to remove one. An item matches only
if it has *every* listed mod, with each value inside its range.

The drop-down filters as you type: every space-separated word you enter must
appear somewhere in the mod name, case-insensitively, in any order. Typing
`total fire` shows `+#% total to Fire Resistance`; typing `socketed minion`
shows `+# total to Level of Socketed Minion Gems`.

Mod names use `#` as the placeholder for the number, exactly as the trade site
does. Unlike the trade site, Acquisition does **not** separate implicit,
explicit, crafted, enchant, or fractured lines: they all go into one table
keyed by mod name, so a single row matches the line wherever it appears on the
item. If the same line occurs twice on one item (for example as both an implicit
and an explicit), only one value is kept for that name; use the
`… total …` pseudomods below when you need lines added together.

## Pseudomods

Pseudomods are synthetic modifiers whose value is the **sum of several real
mods** on the item. They appear in the same drop-down as ordinary mods and are
always named `… total …`. They exist so you can search for "at least 100 total
elemental resistance" without enumerating every combination of single, dual, and
all-resistance lines. The list mirrors the one poe.trade used.

Both implicit and explicit lines feed into the sum. A pseudomod is only present
on an item that has at least one of its contributing mods.

### Resistances

| Pseudomod | Sums |
|---|---|
| `+#% total to Fire Resistance` | Fire; Fire and Cold; Fire and Lightning; Fire and Chaos; all Elemental |
| `+#% total to Cold Resistance` | Cold; Fire and Cold; Cold and Lightning; Cold and Chaos; all Elemental |
| `+#% total to Lightning Resistance` | Lightning; Fire and Lightning; Cold and Lightning; Lightning and Chaos; all Elemental |
| `+#% total to Chaos Resistance` | Chaos; Fire and Chaos; Cold and Chaos; Lightning and Chaos |
| `+#% total Elemental Resistance` | Fire + Cold + Lightning totals (all-Elemental counts three times) |
| `+#% total Resistance` | Elemental total + Chaos total |

### Attributes

| Pseudomod | Sums |
|---|---|
| `+# total to Strength` | Strength; Strength and Dexterity; Strength and Intelligence; all Attributes |
| `+# total to Dexterity` | Dexterity; Strength and Dexterity; Dexterity and Intelligence; all Attributes |
| `+# total to Intelligence` | Intelligence; Strength and Intelligence; Dexterity and Intelligence; all Attributes |

### Speed, damage, and crit

| Pseudomod | Sums |
|---|---|
| `+#% total Attack Speed` | `#% increased Attack Speed` |
| `+#% total Cast Speed` | `#% increased Cast Speed` |
| `+#% total increased Physical Damage` | `#% increased Physical Damage`; `#% increased Global Physical Damage` |
| `+#% total Critical Strike Chance for Spells` | `#% increased Spell Critical Strike Chance`; `#% increased Global Critical Strike Chance` |

### Socketed gem levels

`+# total to Level of Socketed … Gems` exists for **Gems** (the generic line
alone), Elemental, Fire, Cold, Lightning, Chaos, Spell, Projectile, Bow, Melee,
Minion, Strength, Dexterity, Intelligence, Aura, Movement, Curse, Vaal, Support,
Skill, Warcry, and Golem. Each sums its own specific line with the generic
`+# to Level of Socketed Gems`; Fire, Cold, and Lightning also include
`+# to Level of Socketed Elemental Gems`.

For example, on an item with `+1 to Level of Socketed Gems` and `+2 to Level of
Socketed Fire Gems`, `+# total to Level of Socketed Fire Gems` is 3.

### Where the list lives

The authoritative list is `src/pseudomods.cpp` in the source tree. If you want
another pseudomod, that file is the only place to add it — the drop-down and the
per-item sums are both generated from it.
