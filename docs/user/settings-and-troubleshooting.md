# Settings and troubleshooting

## Refreshing items

The **Tabs** menu:

| Item | What it does |
|---|---|
| Fetch tabs list | Downloads the list of stash tabs and characters only, without their contents |
| Refresh checked tabs | Downloads the tabs and characters whose check box in the item list is ticked |
| Refresh all tabs | Downloads everything |
| Auto refresh checked tabs | Checkable; repeats *Refresh checked tabs* on a timer |
| Auto refresh interval... | Sets that timer in minutes |
| Get map stashes / Get unique stashes | Checkable; also download the sub-tabs of Map and Unique stash tabs |

Right-click one or more tabs in the item list and choose **Refresh Selected** to
update just those. Refresh Selected on a folder updates the ticked tabs shown
under it in the current search. Tabs whose price is set in the game are always
refreshed. Acquisition also requests the folders themselves, during **Refresh
checked tabs** whenever at least one of a folder's tabs is ticked.

Items already on disk are shown immediately at startup; the refresh replaces
them tab by tab as downloads finish.

## Rate limiting

The Path of Exile API limits how fast Acquisition may fetch. Acquisition reads the
limits the API sends back and paces itself, so a large account simply takes a
while. The status bar button reads **Rate Limit Status** normally, or
*Rate limited for N seconds* while waiting. Clicking it opens the
**Rate Limit Status Window**, which lists each policy and rule with its current
hits, limit, period, and timeout. Nothing here needs configuring; it exists so
you can see why a refresh is paused.

## Settings menu

- **Theme** — Dark, Light, or Default. Applied immediately.
- **Logging** — OFF, FATAL, ERROR, WARN, INFO, DEBUG, or TRACE. The default is
  INFO.
- **POESESSID** — Show or edit the session cookie used for forum posting.

## Logging

Log output goes to the **Event Log** panel at the bottom of the window and to
`log.txt` in the data directory. When reporting a problem, set the level to
DEBUG, reproduce it, and attach `log.txt`. You can also start with
`acquisition --log-level DEBUG`.

## Updates

Acquisition checks the GitHub releases at startup and once a day after that.
When a newer version exists, an **Update available** button appears in the
status bar. Clicking it names the new version and offers **Go to Github** (the
download page), **Ignore**, or **Ignore, and don't ask again**. Pre-releases are
offered separately from stable releases, and a version you choose not to be
asked about again is not offered again.

## Crash reports

With **Report crashes** enabled on the login window, native crashes are sent to
the developer through Sentry. Reports contain a stack trace and the Acquisition
version; they do not include your item data.

The choice is read when Acquisition starts, so changing it takes effect at the
next launch. With it turned off, the crash reporter is not started at all.

## Files and where to find them

See [Getting started](getting-started.md#where-your-data-lives) for the data
directory. Deleting `data/` forces a full re-download and loses your buyouts;
deleting `cache/` only loses cached item images; `settings.ini` holds the
options from the login window and the menus.

## Common problems

| Symptom | Likely cause |
|---|---|
| *Error requesting leagues* on the login window | The API is unreachable; check your connection or proxy |
| Login button stays disabled | You have not authenticated, or no league is selected |
| Refresh seems stuck | Open the Rate Limit Status Window; the API is probably throttling |
| *No forum threads have been set* | Set a thread under **Shop → Forum shop thread…** |
| *POESESSID has not been set* / rejected | Enter a fresh cookie under **Shop → Update shop POESESSID** |
| Cannot change a price | The price comes from the game (tab name or item note); change it there |
| Windows: crash at startup in `MSVCP140.dll` | An outdated Visual C++ runtime; re-run the installer (it updates the runtime) and reboot if asked |

## Getting help

The best place to look or ask is [GitHub Issues](https://github.com/smallcatlabs/acquisition-qt/issues).
