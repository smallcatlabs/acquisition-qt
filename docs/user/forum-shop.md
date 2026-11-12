# Forum shop

The official trade site indexes the *Trading* forums, so a forum thread that
lists your priced items makes them searchable there — including items in
remove-only tabs and character inventories, which the site does not index from
your stash directly. Acquisition writes and updates those threads for you.

## One-time setup

1. Create one or more threads in the appropriate Trading forum on
   pathofexile.com. Note the number in each thread's URL.
2. **Shop → Forum shop thread…** — enter the thread number. Enter several,
   separated by commas, if you have more items than fit in one post; Acquisition
   spreads the items across them in order. The menu item shows the configured
   numbers, e.g. *Forum shop thread... [1234567]*.
3. **Shop → Update shop POESESSID** — paste your `POESESSID` cookie from
   pathofexile.com. OAuth cannot edit forum posts, so this cookie is required.
   The same dialog is under **Settings → POESESSID**. If the site rejects the
   cookie, Acquisition warns you and stops updating until you enter a new one.

## Publishing

- **Shop → Update forum shop(s)** rewrites every configured thread with the
  current buyouts. Each item is wrapped in a `[spoiler]` with its price prefix,
  and the whole list is placed where `[items]` appears in the template.
- **Shop → Automatically update shop** (checkable) republishes after every
  refresh, but only if something changed since the last post.
- **Shop → Copy shop data to clipboard** gives you the generated forum markup
  without posting, for pasting by hand.

Items priced *[Ignore]* are omitted; *No price* items appear without a tag.
Which tag each buyout type produces is listed in [Pricing](pricing.md).

## The template

**Shop → Edit Shop Template** opens the text that becomes the thread body. The
default is simply `[items]`. Anything else you type — a greeting, contact
details, rules — is kept, and `[items]` is replaced with the generated list.
The template is applied to every thread.
