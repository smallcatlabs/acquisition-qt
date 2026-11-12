# Trade Ground Truth

**Status: living research document, begun September 4, 2026.** This is
the companion to `network-ground-truth.md` for a different subject: not
how the Path of Exile API limits requests, but how items get *listed for
sale* — the trade site's two channels, the forum shop mechanism, the
in-game price dialog, and what the sanctioned API does and does not
report about any of it. The Rust spike's pricing rulings
(`spikes/rust-playground/decisions/pricing.md`: C67 the price value, C68
the currency table, C69 the listing state, C74 the shop render, C81 the
effective price) rest on these claims. When a claim here falls, every ruling that cites it falls
with it; that is the point.

Rules of this document (the same as `network-ground-truth.md`):

- **Claims are numbered T1, T2, … and never renumbered.** Designs,
  rulings and reference tables cite them by number.
- Every claim carries a **source tag** and a **confidence**. When better
  evidence lands, the claim is upgraded in place with the new citation —
  history stays in git.
- A claim states what the evidence shows and no more. An endpoint that
  returned no `forum_note` for one item is a claim about that endpoint
  and that item, not about the public-stash stream.
- Hypotheses and open questions live here too, clearly marked. An open
  question is one hand experiment or one read away; it is not acted on
  as if answered.
- GGG's own text is quoted verbatim. The owner's observations are quoted
  verbatim and marked as the owner's. The saved web pages the SITE and
  COMMUNITY claims were read from are kept locally by the owner and are
  never committed (the spike's `pricing-info/`, gitignored).

Source tags:

| Tag | Meaning |
|-----|---------|
| DOC | The official API reference (`https://www.pathofexile.com/developer/docs/reference`), a sanctioned surface; re-read September 4, 2026 |
| SITE | A dated browser read of the trade site (`https://www.pathofexile.com/trade`) by the owner, saved as a page; the saved copy is the observation. Under the spike's C79 the site is a governed surface: a human reads it, tooling never fetches it |
| OWNER | The owner's own observation in game or on the website, verbatim, dated |
| RUN | The spike's live run against the sanctioned API: the `LIVE-TESTING.md` run-ledger row plus the facts it landed, dated. Nothing here was fetched from a surface GGG does not sanction |
| CODE | What the C++ application on `master` and the third-party tool Procurement do — evidence of what worked once, never of what the forum requires |
| COMMUNITY | The PoE Wiki (`poewiki.net`), a third party's reading of GGG's surfaces; dated, corroboration only |
| HYP | Hypothesis — plausible, load-bearing, unverified |

Confidence: **Confirmed** (GGG's own text, or a direct observation of
the API's response), **High** (strong secondary evidence), **Provisional**
(acting on it, would like better evidence — a single in-game observation
by the owner, or a 2022 community page).

Dates: the price-notes run and the API re-read fall on September 4, 2026
UTC, which is the evening of September 3 in the owner's time zone. The
saved pages and the owner's in-game observations are dated September 3.
T16–T19 are the owner's hand tests of September 6, 2026, run while the
spike's listing state was being ruled. T20–T24 are the owner's
observations of September 7, 2026, at the shop render's validation
reading. T25–T34 are the site as the oracle (the spike's pricing slice,
step 7, item 5): the owner's saved seller-account searches and exchange
pages of September 8 and 9, 2026, joined by item id against the
account's facts of the same days, and four in-game experiments run
between the two captures; T17 is rewritten from the same evidence.

---

## Claims ledger

### The trade site

**T1. The trade site indexes two listing channels and no third: the
shop forums, and public premium stash tabs.** [SITE — Confirmed; About
page, saved September 3, 2026]
The About panel's first sentence, verbatim:

> "This website allows you to search through items listed for sale in
> Shop Forums as well as those listed using Public Premium Stash Tabs
> in-game."

Nothing on the page names another channel. The API reference's
`/public-stash-tabs` stream (T5) is how the second channel reaches
third parties; the forum is the only route for an item the stash
channel cannot see — a non-public tab, or a character's inventory (T8).

**T2. A bulk-exchange listing is written as a ratio,
`~price <wanted>/<lot> <currency>`, on the item offered, "either
in-game or on the forum"; the denominator is a lot size, not a
divisor.** [SITE — Confirmed; About page, saved September 3, 2026]
The "Bulk Item Exchange" section, verbatim:

> "Certain items may be listed for bulk sale (listed below). Your
> eligible items will be grouped together under one price, with the
> total amount of items presented as "stock". In order to list exchange
> entries, either in-game or on the forum, use the ratio format on the
> item you wish to offer for the type of item you would like in
> return."

The two worked examples, verbatim (each shows a stack of the offered
item with its stack size, then the note):

> For example: [a Chaos Orb stack of 10] `~price 3/1 alch` will list
> your intent to buy 3 Orbs of Alchemy for 1 Chaos Orb.
>
> Alternatively: [an Orb of Transmutation stack of 20]
> `~price 2/35 chaos` will list your intent to buy 2 Orbs of Chaos for
> 35 Orbs of Transmutation . Though this is technically equivalent to
> selling 17.5 Orbs of Transmutation per Chaos Orb, pricing this way is
> more appropriate for bulk sets of items.

So the grammar is `~price <wanted>/<lot of this item> <currency
wanted>`: the numerator counts the currency asked for, the denominator
counts the offered item per lot. GGG's own text says `2/35` is
deliberately not `1/17.5`; by the same token `3/1` is not `3`. A
consumer that reduces the pair or compares it as a rational loses what
the seller wrote (the spike's C67 keeps the pair unreduced).

The eligible items ("listed below" — the "Item Tags – *group*" panels)
are rendered at runtime from a list the saved page does not contain;
the capture holds the template only. Which items are bulk-eligible is
therefore **not established** here (Q10). Whether a ratio on an item
outside that list is ignored, listed singly, or grouped is open (Q6).

**T3. "Stock" counts every publicly listed item of that type, priced or
not.** [SITE — Confirmed; About page, saved September 3, 2026]
Verbatim: "Please note: Stock includes all of the items of that type
that are publicly listed (priced or not)." So an unpriced item in a
public tab is still *listed* for the purpose of stock; whether an
unpriced forum link is indexed at all is a separate question (Q5).

**T4. The trade site's realms are `pc`, `xbox` and `sony` (labelled
"PoE 1 PC", "PoE 1 Xbox", "PoE 1 Sony"); on September 3, 2026 each
realm carried the same eight leagues.** [SITE — Confirmed for that
date; the page's embedded options, saved September 3, 2026]
The saved page's option data lists three realms and, for each, the
leagues `Allflame`, `Hardcore Allflame`, `Ruthless Allflame`,
`HC Ruthless Allflame`, `Standard`, `Hardcore`, `Ruthless`,
`Hardcore Ruthless`. The site read was of the PoE 1 site; no `poe2`
realm appears on it. This matches N42's finding that the API reports
`poe2` as a realm the documented `pc | xbox | sony` enumeration lacks;
whether PoE2 has a trade site of its own was not read. The league list
is a snapshot: it changes every league.

**T25. The seller-account search shows a stash item only from a tab
whose listing entry carries `metadata.public: true`; a priced name on a
non-public tab reaches nothing.** [RUN — Confirmed; September 8, 2026]
The spike's join of the site's rows against the account's facts of the
same day (pricing slice, step 7, item 5): 547 stash rows for the account
in Standard, every one from one of the 13 tabs the API listed public;
the 13 non-public priced tabs (`~price 30 chaos (C)`, twelve
`(Remove-only)` names, 1,121 items) put nothing on the site, nor did
114 price notes in non-public places. Closes Q11.

**T26. A socketed item is never listed on its own; a currency-class
stack is not in the item search; a stack in a priced public tab, or
carrying a price note, is offered on the bulk exchange instead.** [RUN —
Confirmed; September 8–9, 2026]
107 gems in sockets of items in public tabs, none on the site (24 of
them in a tab priced 8888 chaos); eight stacks the spike's state
expected absent from the item search, three of them offered on the
exchange: the test tab's Scroll of Wisdom at its note's 1.5 divine, the
ratio tab's two stacks at the ratio (T11).

**T27. The bulk exchange keys one row per item type per account, every
public stack's offer under it, shown per unit; "stock" counts every
public stack of the type, priced or not.** [SITE — Confirmed; September
8–9, 2026]
The Scroll of Wisdom row carried two offers — 1.5 divine (a stack noted
so) and 8888 chaos (a second stack in a tab named so) — with stock 40,
the two stacks' 21 and 19 together (T3 confirmed); the ratio tab's "2
for 5000 chaos" is shown as 2500 for 1 in the compact layout.

**T28. The trade site reads a price out of a note that carries text
after the currency word, labels the listing "Price with Note", and shows
the whole text.** [SITE — Confirmed; September 8 and 9, 2026]
`~price 777 chaos testing` on an item in a tab priced 8888 chaos listed
at 777 chaos under a fourth label (the others: "Exact Price", "Asking
Price", "No Price Set"); `~price 666 chaos tested` the next day
likewise. The game stores such a note whole (T17).

**T31. The site drops the fractional part of a chaos amount in its
display and keeps it for divine.** [SITE — Confirmed for those two
words; September 8, 2026]
`999.1234`, `999.123`, `999.12` and `999.1` chaos each show "999";
`1.4 divine` shows "1.4". A display fact; the amount is the seller's.

**T32. The indexer resolves the game's eldritch shorthand to the site's
long ids, and does not read `facetors` at all.** [SITE — Confirmed;
September 8, 2026]
`excep-ember`, `grand-ember`, `greater-ember`, `lesser-ember`,
`excep-echor`, `grand-echor`, `greater-echor`, `lesser-echor` list under
`exceptional-eldritch-ember` … `-ichor` at the right price (T16 widened
by eight); `~price 999 facetors`, the word the game's own dialog writes
(T9), lists as "No Price Set", and the word is absent from the exchange's
currency list (T33). The owner reported the latter to GGG as a bug on
September 8 and does not price in that word.

**T33. The bulk exchange's item list, by group.** [SITE — Confirmed for
15 of 22 groups; September 8, 2026]
The site's "Items I Want" panel, expanded and saved: 750 ids with
display names in Currency (101), Fragments/Scarabs/Mapping (232),
Ducats, Enshrouding Crystals, Foulborn Currency & Wombgifts, Allflame
Embers, Runegrafts (31), Tattoos & Omens (113), Expedition Currency,
Delirium Orbs, Catalysts, Oils & Extractor, Fossils & Resonators,
Essences (105), Maps (54); seven groups not expanded (Sanctum, Heist,
Beasts, Cards, Shaper/Elder maps, Unique maps, Legacy). Vials are not
on it. The list is `crates/acquisition-plan/reference/exchange-items-2026-09-08.json`
in the spike, a proposal under its C68. Answers Q10 for those groups.

### The API

**T5. `Item` carries both `note` and `forum_note`, each "user-generated
text"; `Item` is the object `/stash`, `/character` and
`/public-stash-tabs` all return; `StashTab.index` is an optional
unsigned integer; `StashTab.metadata.public` is "always true if
present".** [DOC — Confirmed; reference re-read September 4, 2026]
The reference's `Item` object lists, in this order and wording:
`note ?string user-generated text`, `forum_note ?string user-generated
text`. `StashTab` lists `index ?uint` and, under `metadata`, `public
?bool always true if present` and `folder ?bool always true if present`.
`PublicStashChange` (the `/public-stash-tabs` stream's element) carries
`public bool if false then optional properties will be null`,
`accountName ?string`, `stash ?string the name of the stash`,
`stashType`, `league`, and `items array of Item`. The stream is
documented as "all public stashes in all leagues for the given realm",
PoE1 only, under scope `service:psapi`.

What this does *not* say: which endpoints ever populate `forum_note`.
The field's existence in the shared `Item` schema is the only
documented trace of the forum channel in the API. See T6 and Q9.

**T6. `GET /character/{name}` does not report a forum listing: on
September 4, 2026 it returned the forum-listed, forum-priced,
site-listed body armour with neither `note` nor `forum_note`.** [RUN —
Confirmed for `/character` and this item; ledger row 2026-09-04,
response 543]
The run (`LIVE-TESTING.md` ledger row dated 2026-09-04; daemon pid
31313, 00:51–00:52 UTC) sent `GET /character/I_EXIST` and received 200
with 53 items. The owner had, on September 3, linked and priced that
character's body armour (Foulborn Skin of the Lords, `inventoryId`
`BodyArmour`, x 0, y 0) in a forum shop post (T7), and the trade site
lists it (T8). The item came back with no `note` and no `forum_note`;
no item in the run's three responses, and none in the spike's store,
carries `forum_note`.

The claim is about `/character` and one item. Whether
`/public-stash-tabs` carries `forum_note` for a forum-listed stash item
is untested and, for the spike, moot — it never reads that stream (Q9).
Consequence for the spike: a forum listing is intent it holds and text
it renders, never a fact it observes; "did my post index?" is answered
on the trade site by a human. The forum is write-only from the tool's
side.

### The forum shop mechanism

**T7. The website's item-link button emits a `linkItem` code that
carries `realm=`; once the post is saved, the forum has replaced the
code with `[item post="<post id>" index="<ordinal>"]` — the link is
resolved at post time into an item bound to the post.** [OWNER —
Confirmed for the shape the site emitted on September 3, 2026; one
post]
The owner, September 3, 2026: the website's link button for the
character item emitted, verbatim,

```
[linkItem realm="pc" location="BodyArmour" character="I_Exist" x="0" y="0"]
```

and once the post was saved, the post's edit view showed, verbatim,

```
[item post="26816146" index="1"]
```

So `realm=` is part of what the site emits today (the C++ application
was right to include it; the 2022 wiki and Procurement, T14 and T15,
omit it). And the text a poster submits is not what the forum stores:
the forum resolves each link when the post is saved, and the stored
form names the post and an ordinal, not a location. Consequences: a
tool cannot compare its rendered text with the forum's content;
"unchanged since last post" can only be a comparison with the tool's
own previous output (the C++ app's page hash). Whether the forum
re-resolves a linked item that later moves is open (Q8). The character
attribute was emitted with the character's display capitalization
(`I_Exist`) while the run's request path (T6) used I_EXIST. Settled
September 7, 2026: the spike's render wrote `character="I_EXIST"`, the
listing's spelling, and the link resolved with the rest of the page —
the owner: "all 27 items are linked" — so the forum's match does not
depend on the display capitalization.

**T8. A character-inventory item is listable through the forum: the
item linked in T7 appears on the trade site as a listing.** [OWNER —
Confirmed for one item, September 3, 2026]
The owner, verbatim: the body armour on the character in the Allflame
league "shows up on the official site as a trade listing". Together
with T1 this is the forum channel doing the one thing the stash channel
cannot: listing an item that is not in a public premium tab. (The C++
user documentation makes the same claim in general terms; this is the
first time it was observed on this account with a dated post.)

**T21. A forum link with no price beside it is indexed: the trade site
lists the item as "No Price Set".** [OWNER — Confirmed for one post;
September 7, 2026]
The owner, verbatim: "items linked to the forum without a price
annotation are listed with 'No Price Set' on the trade site." So the
forum channel indexes an unpriced link the way the stash channel
indexes an unpriced item in a public tab (T3): listed, with no price.
The same under an empty spoiler title (`[spoiler=""]`, the C++ app's
no-price row, T15), read September 7, 2026 on the render's page — the
owner, verbatim: "Horror Spur is empty under the title." Q5 is
answered. What the spike takes: a hand `no_price` item posts as
its link alone, under an empty spoiler title — the C++ app's no-price
row (T15) — and the render's cell for it is a posting row (C74).

**T22. The forum accepts a post shaped as nested spoilers — one spoiler
per price whose title is the price text, the links run together on its
line, all inside one page spoiler — resolves each link into `[item
post= index=]` in place, and the trade site prices every item in a
spoiler by that spoiler's title.** [OWNER — Confirmed for one post;
September 7, 2026]
The owner's post, as the forum stored it after resolving the links,
verbatim:

```
[spoiler="Shop Post 1 of 2 (3 items)"]
[spoiler=" ~price 321 divine"][item post="26816146" index="1"][item post="26816146" index="2"][/spoiler]
[spoiler=" ~price 2 mirror"][item post="26816146" index="3"][/spoiler]
[/spoiler]
```

This is the C++ app's shape (T15: a spoiler per buyout with the price
as its title, the page in a plain spoiler) with the page spoiler now
titled, and it is what the owner asked the render to write: "Items with
the exact same price should be listed together; all prices should be
wrapped in a spoiler tag; newlines only between spoiler tags; each page
should be wrapped in a spoiler." The owner, the same day, on the
site's reading of it, verbatim: "the spoiler text is used as the price
for every item within that spoiler block." So the forum takes the
shape, resolves the links inside it, and the indexer reads the price
from the enclosing spoiler's title — the C++ app's form (T15) was
right about where the indexer looks, and the wiki's "after the code"
(T14) is at best one of two places. Which of a spoiler title and a
price line after a link wins when both are present is not tested.

**T23. A forum post is limited to 50,000 characters, as a hard limit
enforced by the site.** [OWNER — Confirmed; September 7, 2026]
The owner, verbatim: "I confirmed there is a hard 50,000 character
limit for posts." The C++ app's constant (T15) was the real bound all
along; Q4 is answered. What the spike takes: the render's default page
size stays 50,000, now a measured limit rather than an inherited
constant, and a page is counted in characters with the template around
it.

**T34. A forum-listed item shows on the item search with the thread as
its seller link, and a forum-listed exchange-eligible stack on the
exchange; the site dates a forum listing by something other than the
post.** [SITE — Confirmed; September 8, 2026]
Four items from non-public remove-only tabs listed "Asking Price" with
`/forum/view-thread/<n>` links from two threads, shown "listed last
month" and "2 months ago" for posts days old; five Runegrafts and a
Wombgift from the same tabs offered on the exchange at 4321 blessed.
T8 and T12 at nine rows.

### The in-game price dialog

**T9. The in-game price dialog offers 39 currencies; for each, the word
the game writes into `note` is fixed and is the site's spelling, not
the community abbreviation: `exalted`, `chrome`, `jewellers`, `fusing`
where the C++ table wrote `exa`, `chrom`, `jew`, `fuse`; `echor` is the
game's spelling. `chisel`, `coin` and `silver` are not offered.**
[RUN + OWNER — Confirmed for the words (the game wrote them); the
display names and the "not offered" list are the owner's reading of
the dialog on September 4, 2026]
The run fetched the owner's `ACQUISITION-PRICE-TEST` tab (`GET
/stash/Standard/03cb479c65`, response 542: a public premium tab with no
price in its name, 80 items, 50 with notes) after the owner had priced
one item in every currency the dialog offers. The 39 distinct words
that GGG's client wrote, and the dialog entry the owner chose for each
(one to one, checked against the notes; the spike's committed fixture
`crates/acquisition-plan/reference/price-notes-2026-09-04.txt` is the
verbatim corpus):

| word | display name | word | display name |
|------|--------------|------|--------------|
| `chaos` | Chaos Orb | `scrap` | Armourer's Scrap |
| `divine` | Divine Orb | `whetstone` | Blacksmith's Whetstone |
| `alch` | Orb of Alchemy | `gcp` | Gemcutter's Prism |
| `exalted` | Exalted Orb | `bauble` | Glassblower's Bauble |
| `alt` | Orb of Alteration | `offer` | Offering to the Goddess |
| `mirror` | Mirror of Kalandra | `offer-dedication` | Dedication to the Goddess |
| `chrome` | Chromatic Orb | `offer-gift` | Gift to the Goddess |
| `blessed` | Blessed Orb | `offer-tribute` | Tribute to the Goddess |
| `fusing` | Orb of Fusing | `lesser-ember` | Lesser Eldritch Ember |
| `jewellers` | Jeweller's Orb | `greater-ember` | Greater Eldritch Ember |
| `regal` | Regal Orb | `grand-ember` | Grand Eldritch Ember |
| `vaal` | Vaal Orb | `excep-ember` | Exceptional Eldritch Ember |
| `chance` | Orb of Chance | `lesser-echor` | Lesser Eldritch Ichor |
| `annul` | Orb of Annulment | `greater-echor` | Greater Eldritch Ichor |
| `aug` | Orb of Augmentation | `grand-echor` | Grand Eldritch Ichor |
| `regret` | Orb of Regret | `excep-echor` | Exceptional Eldritch Ichor |
| `scour` | Orb of Scouring | `facetors` | Facetor's Lens |
| `transmute` | Orb of Transmutation | `engineers` | Engineer's Orb |
| `wisdom` | Scroll of Wisdom | `infused-engineers-orb` | Infused Engineer's Orb |
| `portal` | Portal Scroll | | |

Against the C++ application's 19 tags (T15): twelve identical (`chaos`,
`chance`, `divine`, `alch`, `alt`, `scour`, `regret`, `mirror`,
`blessed`, `regal`, `gcp`, `vaal`); four spelled differently by the game
(`exa`→`exalted`, `chrom`→`chrome`, `jew`→`jewellers`, `fuse`→`fusing`);
three the dialog does not offer (`chisel`, `coin`, `silver` — the owner,
September 4: no longer in the game); 23 words the C++ table lacks. The
owner also reports having seen forum posts with `mir` for Mirror, so the
indexer's matching is looser than the dialog's output; how loose is not
established, and the spike does not model it (C68).

**T10. The shapes the dialog writes: `~price <amount> <word>` (exact),
`~b/o <amount> <word>` (negotiable), amounts as integers or decimals to
at least four places and to at least five digits, `~skip ` for "Do not
index", and `~price  <word>` — an empty amount, two spaces — left after
an invalid entry.** [RUN + OWNER — Confirmed for the shapes observed;
the meaning of `~skip` and of the empty amount is the owner's word,
September 4, 2026]
From the same tab, verbatim notes (the corpus is the fixture in T9):
`~price 999 <word>` (the usual form, 39 times, once per currency),
`~b/o 999 chaos`, `~b/o 1.5 divine`, `~price 1.4 divine`,
`~price 999.1 chaos`, `~price 999.12 chaos`, `~price 999.123 chaos`,
`~price 999.1234 chaos` (twice), `~price 12345 chaos`, `~skip ` (with a
trailing space), `~price  chaos` (empty amount, two spaces). Thirty of
the 80 items carry no `note`. No ratio note is present: the owner
reports the dialog does not offer the ratio on incubators (T12), and
the empty-amount note is what it left behind after one was attempted.

The owner, September 4, 2026: the empty amount "is what the game leaves
after an invalid entry such as a ratio on a non-bulk item"; `~skip` is
the game's "Do not index" choice; the five-digit and four-decimal
amounts are the game's own (whether a fifth decimal is truncated or
refused is not established). The prefix, one space, the amount, one
space, the word — that is the whole grammar the game wrote. A parser
that treats an empty amount as "no price" rather than "invalid" reads
the game's residue as a decision (the spike's parser reads it
`invalid`; since T18 the listing state gives it no effect and the tab
applies, C69).

**T16. The indexer matches a currency word loosely: `~price 5 exa`
and `~price 5 exal` both list at 5 Exalted Orbs.** [OWNER — Confirmed
for those two words; September 6, 2026]
The owner, verbatim: "`~price 5 exa` works, and so does `~price 5
exal`." How loose the matching is in general (a prefix of the word, of
the display name, or a fixed alias list) is not established, and the
spike does not model it (C68): acquisition writes only the table's
tags, and a hand-typed word the table lacks reads as no effect in the
listing state with the note shown verbatim (C69) — an alias row, cited
to a test like this one, is the fix when one matters.

**T17. The in-game price dialog stores a note whole and displays only
the part it parsed: `~price 5 chaos (A)` is shown as `~price 5 chaos`,
served by the API as typed, and read by the trade site through the
suffix.** [OWNER — Confirmed; September 8, 2026, superseding the
September 6 reading]
The September 6 observation — "Setting `~price 5 chaos (A)` in game
causes the `(A)` to be removed" — was the dialog's display, not its
store. The owner, September 8, verbatim: "In game the pricing note is
exactly '~price 777 chaos' but on the trade site it shows as 'Price
with Note' of 777 chaos with the text '~price 777 chaos testing'
rendered in the web UI. I have confirmed both of these things are true
at the same time. […] I confirmed this by using the UI to set the
item's note to '~price 666 chaos tested'. In game this turned into
'~price 666 chaos' as soon as I looked at it. On the trade site it's
listed as 'Price with Note' just like the 777 case." The spike's facts,
served by the API, held the whole text both times. The dialog's four
options (Note, Negotiable Price, Exact Price, Do Not Index) stand as
described on September 6. So a note can carry trailing text the way a
tab name does (T11), and a parser must read a note as it reads a tab
name — the word after the amount, anything after it tolerated (the
spike's parser v2, C69); one that held a note to the exact grammar
read `~price 777 chaos testing` as no price at all.

### Tab prices and what beats what

**T11. A tab is priced only by renaming it in game; a public tab whose
name is a valid price lists every item in it at that price; a ratio in
a tab name lists nothing on the item search and offers the tab's
exchange-eligible stacks on the bulk exchange at the ratio.** [OWNER —
the owner's in-game observations, September 3, 2026; the ratio's two
surfaces RUN — Confirmed, September 8–9, 2026]
The owner, verbatim: "tab prices can only be set in-game by renaming
the tab"; and, on the tab named with a ratio: "The in-game tab prices
may be invisible, but every item in a priced tab shows up in the trade
listing unless the tab pricing is invalid. It looks like tab prices do
not support the 'X/Y' syntax." So the tab name is a price source with
two outcomes for the whole tab — valid, applying to each item, or
invalid, unlisting all of them — with no per-item residue.

The owner's own priced tabs (RUN, the same account's listing of
September 4: 13 tabs in pc/Standard, none of them public) carry
trailing text after the price, verbatim: `~price 30 chaos (C)`,
`~price 20 chaos (Remove-only)`, `~price 20 chaos (A) (Remove-only)`,
and so on — the `(Remove-only)` suffix is the game's own marking of a
remove-only tab. None of those tabs is public, so whether the site
tolerates the trailing text is not established here; the C++ parser
tolerated it. Whether an item's own `note` overrides its tab's name
price on the site was open (Q2) until T29.

The ratio's two surfaces (the spike's join, September 8–9): two public
tabs, `~b/o 5000/2 chaos` (56 items) and `~price 1000/2 chaos` (70),
put two rows on the item search — the two items carrying their own
`~price 1999 chaos` note, so a valid note lists inside a tab whose name
lists nothing — and the first tab's Infused Engineer's Orbs and
Tailoring Orb on the exchange as "2 for 5000 chaos" (T27). `~b/o` with
a ratio is accepted in a tab name; the tab's two Vials, not
exchange-eligible (T33), are offered nowhere.

**T12. A forum shop lists individual items, never whole tabs; the
in-game dialog offers the ratio format only on bulk-tradeable items; a
forum price takes precedence over a tab's in-game price.** [OWNER —
Confirmed for the precedence, one item, September 7, 2026; the rest
Provisional from the owner's observations of September 3, 2026]
The owner, verbatim: "only individual items can be listed in forum
shops, not entire tabs"; "only bulk-tradeable items can use the 'X/Y'
format for price"; "items priced in forums take precedence over the tab
if the tab has an in-game price". The first agrees with the link-code
grammar (T7, T14: a link names one item by position); the second agrees
with T2's "eligible items"; the third is the first evidence about a
*relation* between the two channels and points one way only — a forum
price against an item *note* is untested (Q2), as is a forum price
against a game `~skip` (Q7). The precedence was read directly on
September 7, 2026: an item in the owner's public tab named `~price 222
divine`, posted through the forum at 99 chaos, is listed by the site at
99 chaos — the owner, verbatim: "Dread Dome is 99 chaos."; the spike's
render relies on it (C81: an item row beats a tab name).

**T18. A `~` note the site cannot read has no effect: `~c/o 5 chaos`,
`~price 5` (no word) and the empty-amount residue `~price  chaos` each
list the item as "No Price Set" in an individually-priced public tab,
and at the tab's price in a tab set to exact or negotiable.** [OWNER —
Confirmed for those three notes; September 6, 2026]
The owner, verbatim: "All other pricing tests result in no-effect and
the item is listed as 'No Price Set' without notes. If the tab is set
to negotiable or exact, the item will inherit the tab's price with no
notes." The owner's description of the tab side, the same day: a public
tab has three modes — "Each Item Individually Priced" (public, no tab
price), "Set Negotiable Price On All Items" and "Set Exact Price On All
Items" (the tab name becomes `~b/o` or `~price`, T11). Whether the
site displays the note's text beside "No Price Set" or beside a tab
price is not established: the owner's earlier description (a free-text
note is shown as "No Price Set" with the text; under a priced tab as
"Price with Note") and the test's "without notes" differ, and nothing
in the spike depends on it. What the spike takes: an unreadable note is
not a price and not a `skip`; the tab applies (C69, C81). A *valid*
note against a tab price remains Q2.

**T19. A ratio note on a non-bulk item lists nothing: the item does not
appear on the site at all.** [OWNER — Confirmed for one item; September
6, 2026]
The owner, verbatim: "the item with the 'X/Y' note was not even listed
in trade at all", tested on a non-bulk item (`~b/o 22/10 chaos`, the
sixth of the September 6 notes). The tab-name analogue is T11 (a ratio
in a name unlists the tab). The dialog does not offer the ratio on such
an item (T12), so the note was typed as "Note" text. The spike reads a
ratio note as a price (T2, C67) and does not know which items are bulk
(Q10, parked); whether the site lists it is the site's, reported
beside the raw note. Q6's in-game half is answered; the forum half
stays open.

**T29. An item's own note beats a valid tab price on the site.** [SITE
— Confirmed; September 8–9, 2026]
The same item, in the tab named `~price 8888 chaos`, listed at its
note's 777, then 666 (T28); the other unnoted items of the tab at 8888.
Closes Q2 in the note's favour — the C++ order the spike's C69 kept.

**T30. A priced public unique tab lists its substashes' uniques at the
tab's price; a folder named with a price lists its public child's items
unpriced; a priced public tab inside a folder lists at its own price.**
[RUN + SITE — Confirmed; September 9, 2026]
`Uniques 1`, made public and named `~price 4554 chaos`: 326 of its 370
uniques in 19 substashes listed at 4554 chaos (the rest were the
capture's paging, the owner's check). Folder `3.19` named `~price 3333
chaos` with `3.19 Cursebot` public and unpriced inside it: its items
"No Price Set". `3.19 Helix Raider` named `~price 2222 chaos` inside
that folder: 9 of 9 at 2222. Confirms the spike's C80. A remove-only
unique tab accepts a price name and cannot be made public.

### Item addressing

**T13. Every stash item's `inventoryId` is the literal `Stash1`,
whatever tab or substash it is in; a socketed item has no position and
no `inventoryId`; a character item's `inventoryId` is its slot; `index`
was present on every one of the 402 listed tabs.** [RUN — Confirmed for
this account; the spike's facts, listing of September 3, 2026 11:36
UTC]
The spike's census (`PRICING-SLICE.md`): 792 of 792 stash items carry
`inventoryId` `Stash1`; socketed items (24 in stash, 531 on characters)
carry neither `x`/`y` nor `inventoryId`; character items carry slot
names (`MainInventory`, `Weapon2`, `PassiveJewels`, `BodyArmour`, …);
`index` is present on all 402 tabs listed in pc/Standard (16 of them
folders, 82 of them substashes). The documented `index` is optional
(T5), so "present on all" is an observation, not a guarantee.

Consequence: the `Stash<n>` in a forum link code (T7, T14, T15) cannot
be read off the item; it has to be derived from the tab, and the only
candidate in the data is the tab's `index` (the C++ app uses
`index + 1`). Which tab `Stash<n>` names when a folder occupies an
index is T24: the folder takes no number, so `index + 1` is one too
high past it; a substash item has no observed address at all (T20).
Socketed items have no address and cannot be linked.

**T20. The website's stash view offers no link for an item in a map or
unique substash: such items cannot be selected there at all.** [OWNER —
Confirmed for this account's map and unique tabs; September 7, 2026]
The owner, verbatim: "the web stash view doesn't allow me to select
items from map or unique stashes at all." So the one instrument that
emits link codes (T7) emits none for a substash item, and Q3's
experiment cannot be run as written. Whether a hand-typed
`Stash<n>` with a substash's index would resolve is a separate
question nobody has tried; until a code for such an item is observed
somewhere, the spike's render blocks the cell and counts it (C74),
naming this claim.

**T24. The website's stash view lists the top-level tabs and folder
children in stash order, folders and substashes left out, numbered
from 0; its item-link button writes `Stash<that number + 1>`, with the
attributes in the order `realm`, `location`, `league`, `x`, `y`.**
[OWNER — Confirmed for this account's Allflame stash; September 7,
2026]
The owner saved the website's tab list for the Allflame league (`numTabs:
48`, each tab `{ "n": <name>, "i": <number>, "id": <the API's id>, "type":
… }`): the 48 non-folder top-level tabs of the 97 the API lists, in the
API's order, numbered `i` 0–47 — the folder "Acquisition Test" (API
index 16) absent, its two children at `i` 16 and 17 (API indices 17 and
18), no substash of the map or unique tabs present. The link the
website's button wrote for an item in the first child, verbatim:

```
[linkItem realm="pc" location="Stash17" league="Allflame" x="9" y="0"]
```

So `Stash<n>` is the tab's one-based rank among the tabs the website
lists, not the API's `index + 1` (T13, T15): the two agree until the
first folder and differ by the folder count after it — the render's
first page wrote `Stash18` for that item and the link did not resolve.
The same day the owner saved the Standard list: 322 tabs numbered 0–321
against the API's 402 — the 16 folders and 64 substashes absent, the
274 remove-only tabs present — and the rank rule matched every one of
the 322 with the folders interleaved (the spike's
`reference/website-tabs-2026-09-07.json` holds both lists beside the
API's, pinned by `t24_stash_numbers_match_the_websites_own_list`). So
the website leaves out folders and substashes and nothing else seen.
Q1 is answered; the attribute order is the one T7 showed for a
character item.

### Community evidence

**T14. Per the PoE Wiki's 2022 guide: the indexer reads the public
stash API and the trading subforums "a few times every minute", has a
thread limit (hence bumping), threads live in per-league shop
subforums, the thread title does not matter, the link code is obtained
by clicking an item in the website's stash view, and the price is
written after the code.** [COMMUNITY — Provisional; "Last updated on
March 7, 2022", page last edited December 1, 2024; saved September 3,
2026]
Verbatim, from the guide:

> "Pathofexile.com/trade gets its information through the Public stash
> tab API, as well as from the forums (particularly the trading
> subforums). The forums have a system to link items from your game
> stash to your posts, which helps verify if the item is still
> available, in the same way the stash tab indexer does. The items can
> then be priced by appending the correct syntax after the code that
> links the item."
>
> "A few times every minute, pathofexile.com/trade look at any posts on
> the forum, and updates the index with any listed items. The indexer
> has a limit to how many threads it searches, which is why it is good
> practice to regularly 'bump" the thread, or post a comment to have it
> appear higher in the subforum."
>
> "Go to the Standard League - Shops subforum. Create a thread. The
> title of the post does not matter."
>
> "Click only one time on the item you want to sell. A code, looking
> like this one, will be copied to the text of your forum post:
> `[linkItem location="Stash2" league="Standard" x="5" y="0"]`. If you
> preview your post, you'll see the item picture and a proper link in
> your post."
>
> "After the code, you can tell the indexer what price you want to sell
> the item for. For example, the syntax for a buyout (b/o) of 3 Chaos
> Orb … would be `~b/o 3 chaos`."

The 2022 link code has no `realm=`; the site emits one today (T7). The
polling cadence, the thread limit and the bumping practice are the
wiki's account, not GGG's, and are dated; nothing in this file rests on
them beyond noting that publishing would have to own bumping.

### What the code did

**T15. The C++ application and Procurement emit the same two link
shapes — `[linkItem location="Stash<index+1>" league="<L>" x="<x>"
y="<y>"]` and `[linkItem location="<inventoryId>" character="<name>"
x="<x>" y="<y>"]` — the C++ app with `realm="<r>"` appended and
Procurement without; the C++ app groups items under a spoiler whose
title is the price, Procurement writes the price on the line after the
code; the C++ post limit is a 50,000-character constant; the C++ app
never posted a game-sourced price.** [CODE — what worked once; C++
`master` at 33928a87, Procurement at its last commit (December 22,
2022), read September 3–4, 2026]
The C++ app (`src/itemlocation.cpp`, `GetForumCode`): the stash form
takes the tab index and writes `Stash<index + 1>`; the character form
writes the item's `inventoryId` as `location` and the character's
name; both end with `realm="<r>"`, the realm from settings.
`src/shop.cpp`: items sorted by buyout, each run of equal buyouts
wrapped as `[spoiler=" ~b/o 3 chaos"]…[/spoiler]` (the prefix map
carries a leading space: `" ~b/o "`, `" ~price "`, `" ~c/o "`; the
value is formatted with up to 15 significant digits; a no-price row
gets an empty title); pages cut at `kMaxCharactersInPost = 50000`,
each page substituted for the one `[items]` token of the shop template
and wrapped in a plain `[spoiler]`; the page set is hashed (MD5) to
skip an unchanged post. `src/buyout.cpp`: `IsPostable` is `source !=
game && (priced || no_price)` — a price read from a tab name or an item
note was never written to the forum; `src/currency.cpp` holds the 19
tags (T9 compares them). The C++ user documentation states the forum's
purpose as listing "items in remove-only tabs and character
inventories, which the site does not index from your stash directly".

Procurement (`Procurement/ViewModel/ForumExportVisitors/VisitorBase.cs`):
the same two shapes without `realm=`; the `location` is the item's own
`inventoryId` as the legacy website stash endpoint reported it (under
T13 the OAuth API's value, `Stash1` for everything, would not serve);
the price on the following line as `~b/o <v>`, `~c/o <v>` or
`~price <v>`; tab-wide buyouts keyed by tab **name**; a currency map of
16 abbreviations (`chrom alt jew chance chisel fuse alch scour blessed
chaos regret regal gcp divine exa vaal`), a strict subset of the C++
19 (no `coin`, `mirror`, `silver`). Its last release is 1.29.2 and its
last commit December 22, 2022 (GitHub, read September 4, 2026).

None of this is evidence of what the forum requires: two tools agreeing
on a shape shows the shape was accepted at the time each was written.
The 50,000 figure is a constant, not a measured limit (Q4). The spoiler
form and the next-line form are both folklore about where the indexer
looks for the price; the wiki's "after the code" (T14) and the site's
own bulk examples (T2, a note on the item) are the only GGG-adjacent
statements.

---

## Open questions

Each is one hand experiment by the owner or one read; the render's
policy table (C74) reports the cell as blocked-and-counted until it is
answered.

- **Q1. `Stash<n>` numbering under folders and substashes.** Answered
  September 7, 2026: the one-based rank among the tabs the website
  lists, folders and substashes left out (T24); remove-only tabs are
  listed and numbered like any other.
- **Q2. Item note versus tab name in game.** Answered September 8–9,
  2026: a valid note beats a valid tab price (T29); an unreadable one
  lets the tab apply (T18). Still open: a forum price against an item
  note (T12 covers forum versus tab only).
- **Q3. The link code for a substash item** (a child of a map or unique
  tab). The website's link button cannot be pressed on one (T20,
  September 7, 2026), so the experiment as written has no result; what
  remains open is whether any hand-typed code resolves to such an item.
- **Q4. The real post size limit.** Answered September 7, 2026: a hard
  50,000-character limit (T23).
- **Q5. Is an unpriced forum link indexed?** Answered September 7,
  2026: yes, as "No Price Set" (T21).
- **Q6. A ratio on a non-bulk item posted to the forum: ignored,
  listed singly, or grouped?** In game the dialog refuses the case
  (T10, T12) and a typed one unlists the item (T19); the forum is a
  free text field. Half answered September 9, 2026: `~b/o a/b` is
  accepted in a tab name and offers on the exchange (T11); the forum
  half stays open.
- **Q7. A game `~skip` against a forum price.** Does the forum price
  list an item the game marked "Do not index" (T10, T12)?
- **Q8. Does the forum re-resolve a linked item that moves after
  posting?** T7 shows the link is resolved at post time into
  `[item post= index=]`; whether that binding follows the item or
  breaks is unknown. Experiment: move a linked item, then check the
  post's preview and the site.
- **Q9. Does `/public-stash-tabs` carry `forum_note`?** T6 rules it
  out for `/character`; the stream is the one endpoint left. Moot for
  the spike (it never reads the stream); answerable by anyone with the
  `service:psapi` scope.
- **Q10. The bulk-eligible item list** (T2's "listed below"). Answered
  for 15 of 22 groups on September 8, 2026 (T33); the seven groups the
  owner did not expand stay open in the claim's own words.
- **Q11. Does the seller-account search show items from non-public
  priced tabs?** Answered September 8, 2026: no — `metadata.public` is
  the whole condition (T25); the search, joined against the facts, was
  the oracle for the spike's listing state as a whole.

---

## Appendix — Related registers

- `docs/design/network-ground-truth.md` — the N-claims (rate limiting,
  endpoint policies, realm and character shapes). T4 and T13 lean on
  N42 and N43.
- `spikes/rust-playground/decisions/pricing.md` — the pricing rulings
  (C64–C81). Each ruling that rests on a claim cites it inline; that
  registry, not this file, is the record of which ruling cites what.
- `spikes/rust-playground/SURFACES.md` — the register of governed
  surfaces under C79: the trade site, the forum, the wiki and
  Procurement, each with the access method its claims here were read
  under.
- `spikes/rust-playground/crates/acquisition-plan/reference/price-notes-2026-09-04.txt`
  — the verbatim note corpus behind T9 and T10, committed as a parser
  fixture.
- `spikes/rust-playground/LIVE-TESTING.md` — the run ledger; the row
  dated 2026-09-04 is T6, T9 and T10's run.
