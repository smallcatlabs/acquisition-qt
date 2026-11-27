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
   cookie, Acquisition warns you, clears it, and turns **Automatically update
   shop** off (the menu item is unchecked) until you enter a new one.

## Publishing

- **Shop → Update forum shop(s)** rewrites every configured thread with the
  current buyouts. Each item is wrapped in a `[spoiler]` with its price prefix,
  and the whole list is placed where `[items]` appears in the template. If an
  update is still running, you are told to try again when it has finished.
  The first update after upgrading to 0.18.12 rewrites every thread once, even
  if no price changed: the markup itself changed (every spoiler is now closed,
  and a price title has single spaces).
- **Shop → Automatically update shop** (checkable) republishes after every
  refresh, but only if something changed since the last post. If no thread or
  no POESESSID is set, a warning says so once per session rather than after
  every refresh; updating by hand tells you each time.
- **Shop → Copy shop data to clipboard** builds the forum markup from the
  current prices and copies it without posting, for pasting by hand. It needs
  no thread, but it does need the POESESSID: a stash item's link carries its
  tab's number, which pathofexile.com gives only to a POESESSID. Without one
  nothing is copied and you are told why. Every page is copied, separated by a
  `-------- next page --------` line, and the status bar says how many pages
  were copied. An item whose tab is not in pathofexile.com's list of your tabs
  cannot be linked: it is left out of the copy, as it is of a post, and the
  status bar says how many items were left out. Copying never changes what was last posted, so the next update
  still posts if the shop changed.

Items priced *[Ignore]* are omitted; *No price* items appear without a tag.
Which tag each buyout type produces is listed in [Pricing](pricing.md).

These situations stop or limit a post, and Acquisition tells you about each:

- **More items than the threads hold.** The pages that fit are posted and the
  rest are not. A warning says how many more threads to add under
  **Shop → Forum shop thread…**. It appears each time you update by hand, and
  once per session when the shop updates automatically.
- **Your prices could not be loaded.** If Acquisition could not read its price
  database at startup, it posts nothing and leaves your threads as they are,
  so a problem on your computer cannot blank your shop. Check the log, fix
  the cause, and restart.
- **Your shop's threads or template could not be read.** The same: nothing is
  posted or copied, and a thread or template you set that session lasts
  until you close Acquisition but is not saved, so the saved ones are kept.
- **The site or the forum does not take the update.** The status bar says
  "Forum shop not updated" and why: the site did not answer, a thread could
  not be opened for editing, or the forum did not save the post. When the
  forum gives a reason, the message repeats it in the forum's own words. Two
  you may see:
  - *"Failed to find item. Did you specify the correct location and x y?"*
    Your stash has probably changed since the last refresh. Refresh your
    tabs and update again.
  - *"Permission Denied: You don't have permission to access this area."*
    The thread number belongs to a thread another account started. Check
    the number under **Shop → Forum shop thread…**.

  A message gives the same reason each time you update by hand, and once per
  session when the shop updates automatically. Threads already updated in
  that run keep their new content; the next update starts again from the
  first thread.
- **You are no longer logged in.** If the forum answers with its "Login
  Required" page, Acquisition clears the saved POESESSID and turns automatic
  updates off. Enter a new one to update again.
- **The forum asks you to wait.** For a wait of a minute or less, Acquisition
  waits and tries that thread again, starting from a fresh edit page; each
  thread gets at most three attempts in all. A longer wait stops the update,
  and the message says how long to wait before updating again.

## The template

**Shop → Edit Shop Template** opens the text that becomes the thread body. The
default is simply `[items]`. Anything else you type — a greeting, contact
details, rules — is kept, and `[items]` is replaced with the generated list.
The template is applied to every thread.

A template without `[items]` lists no items: the thread gets the template's
text and nothing else. Acquisition warns you when you save such a template and
each time you update the shop by hand; an automatic update warns once per
session. The update still goes ahead.
