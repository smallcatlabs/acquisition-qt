# Getting started

## Installing

Download a package for Windows, macOS, or Linux from the
[releases page](https://github.com/smallcatlabs/acquisition-qt/releases). On Windows the
installer will update the Microsoft Visual C++ runtime and may need a reboot. To
build from source, see [BUILD.md](../../BUILD.md).

## Logging in

The login window has an **OAuth** tab with an **Authenticate** button.

1. Click **Authenticate**. Your web browser opens the Path of Exile OAuth page.
   Approve the request. The button changes to *Re-authenticate (as someone else)*
   and the label shows *You are authenticated as "<account>"*.
2. Choose a **Realm** (PC, Xbox, or PlayStation) and a **League**. The league
   list is fetched live, so an error here usually means the Path of Exile API is
   unreachable. You can manually edit this field to specify a **Private League**.
3. Click **Login**. The button is disabled until you are authenticated and a
   league is selected.

Acquisition keeps the OAuth token, so on later launches you only need to click
**Login**. The token is refreshed at startup; if the refresh fails you will see
*The OAuth token needs to be refreshed* or *The OAuth token is not valid* — click
**Authenticate** again.

Login is also refused, with a message under the button, when it cannot tell
who the session is for: *The OAuth token does not name an account; please
authenticate again.* (click **Authenticate** again), or *Please select a realm and
a league before logging in.* A refused login does not save the realm.

OAuth covers reading your stashes and characters. Posting to forum shop threads
still needs a POESESSID cookie; see [Forum shop](forum-shop.md).

### Advanced options

**Show Advanced Options** reveals:

| Option | Effect |
|---|---|
| Remember me | Keep the sign-in (account, OAuth token and POESESSID) for the next launch; unchecked, only those are forgotten on exit |
| Report crashes | Send crash reports to the developer via [Sentry](https://sentry.io); a change takes effect at the next launch |
| Use system proxy | Route network traffic through the operating system's proxy settings |
| Logging Level | Same as the **Settings → Logging** menu in the main window |
| Theme | Dark, Light, or Default (the platform theme) |

## What happens after login

The main window title reads `Acquisition [version] - <League> League [<account>]`.

Acquisition first loads any items it cached from a previous session, so a large
account is searchable within seconds of logging in. If the cache cannot be read,
the status bar says *Could not read the cached items; see the log*; the window
starts empty until a refresh fetches the tabs again. Then you can use it to fetch
the stash tab list from the API. You can refresh **all tabs**, **checked tabs**,
or **selected tabs** using the right click context menu and/or menubar. See
[Settings and troubleshooting](settings-and-troubleshooting.md#refreshing-items)
for the refresh modes and [Searching](searching.md) for the layout of the window.

## Where your data lives

Everything Acquisition writes goes into one data directory:

| Platform | Default |
|---|---|
| Windows | `%localappdata%\acquisition` |
| macOS | `~/Library/Application Support/acquisition` |
| Linux | `~/.local/share/acquisition` |

Inside it you will find `settings.ini`, `log.txt`, a `data/` folder with one
SQLite database per account (`userstore-<account>.db`) plus the legacy
per-league databases, and a `cache/` folder for item images.

Earlier versions also kept a `network_cache/` folder of downloaded web
replies, in the default directory even when `--data-dir` named another one.
Acquisition no longer writes it and deletes it from the data directory it
starts with. If you only ever ran with `--data-dir`, an old `network_cache/`
may remain in the default directory; it is safe to delete.

Your sign-in is stored there unencrypted: `settings.ini` holds the POESESSID,
and the global database in `data/` holds the OAuth token. Anyone who can read
those files can act as your account, so do not share `settings.ini` or the data
directory, and turn off **Remember me** on a computer you share.

The directory is chosen when Acquisition starts: use `--data-dir <path>` to run
with a different one, for example to keep two accounts apart or to try a beta
without touching your real data:

```
acquisition --data-dir /path/to/other/dir
```

Only one Acquisition at a time can use a data directory. A second one started
on the same directory says so, shows what holds the lock and where the lock
file is, and exits. The running one keeps a lock file, `acquisition.lock`,
which is removed when it closes. One left behind by a crash on the same
computer is taken over at the next start. One written on another computer (a
shared or synced data directory) or one that cannot be read is not: if you are
sure no Acquisition is using the directory, delete the file named in the
message.

The other command-line options are `--log-level <level>`; see
[Settings and troubleshooting](settings-and-troubleshooting.md#logging), and
`--check-tls`, which prints the TLS backends Acquisition finds and exits
without starting.
