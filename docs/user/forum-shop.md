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
- **Shop → Automatically update shop** (checkable) republishes after every
  refresh, but only if something changed since the last post. If no thread or
  no POESESSID is set, a warning says so once per session rather than after
  every refresh; updating by hand tells you each time.
- **Shop → Copy shop data to clipboard** gives you the generated forum markup
  without posting, for pasting by hand.

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
- **The site or the forum does not take the update.** The status bar says
  "Forum shop not updated" and why: the site did not answer, a thread could
  not be opened for editing, the forum could not find one of your items
  (refresh your tabs and update again), or the forum reported an error. A
  message gives the same reason each time you update by hand, and once per
  session when the shop updates automatically. Threads already updated in
  that run keep their new content; the next update starts again from the
  first thread.
- **The forum asks you to wait.** For a wait of a minute or less, Acquisition
  waits and tries that thread again, starting from a fresh edit page, up to
  three times. A longer wait stops the update, and the message says how long
  to wait before updating again.

## The template

**Shop → Edit Shop Template** opens the text that becomes the thread body. The
default is simply `[items]`. Anything else you type — a greeting, contact
details, rules — is kept, and `[items]` is replaced with the generated list.
The template is applied to every thread.
