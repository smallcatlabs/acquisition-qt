# Getting started

## Installing

Download a package for Windows, macOS, or Linux from the
[releases page](https://github.com/smallcatlabs/acquisition-qt/releases). On Windows the
installer will update the Microsoft Visual C++ runtime and may need a reboot. To
build from source, see [BUILD.md](../../BUILD.md).

## Logging in

The login window has an **OAuth** tab with an **Authenticate** button.

1. Click **Authenticate**. Your web browser opens the Path of Exile OAuth page.
   Approve the request. The browser page then says *You can close this page and
   return to Acquisition.*, whatever the outcome; the login window says whether
   it worked. The button changes to *Re-authenticate (as someone else)* and the
   label shows *You are authenticated as "<account>"*.
2. Choose a **Realm** (PC, Xbox, or PlayStation) and a **League**. The league
   list is fetched live for the chosen realm, so an error here usually means the
   Path of Exile API is unreachable. Changing the realm fetches that realm's
   list; the league box is unavailable until it arrives, and your saved league
   stays selected if the new list has it. You can manually edit this field to
   specify a **Private League**.
3. Click **Login**. The button is disabled until you are authenticated and a
   league is selected.

While Acquisition waits for the browser, the button reads *Authenticating...*
and stays usable. If the browser page was closed or never answered, click the
button again: the earlier attempt is dropped, so its page can no longer log
you in, and the browser opens afresh. The wait has no time limit.

If the browser login fails, a red message in the login window says why and
the button offers **Authenticate** again:

| Message | Meaning |
|---|---|
| *Authentication failed: access_denied (...)*, or another error code | Path of Exile refused, for example because you declined the request in the browser; the code and the text in brackets are the server's |
| *Authentication failed: the reply from the browser did not match this login attempt.* | The browser answered for another attempt, or something else answered in its place |
| *Authentication failed: no token was received from Path of Exile.* | The browser answered, but Path of Exile did not complete the login, or could not be reached |

Acquisition keeps the OAuth token, so on later launches you only need to click
**Login**. The token is refreshed at startup. If that fails, **Login** stays
disabled and a red message says which of two things happened:

| Message | Meaning |
|---|---|
| *The stored login was refused by Path of Exile. Please authenticate again.* | Path of Exile no longer accepts the stored token (it can expire or be withdrawn on their side). Acquisition has removed it; click **Authenticate** |
| *Could not refresh the stored login: the Path of Exile server could not be reached.* | The network or the server failed. The stored token is kept: restart Acquisition to try again, or click **Authenticate** to log in afresh |

If **Login** is enabled but answers *The OAuth token needs to be refreshed* or
*The OAuth token is not valid*, click **Authenticate** again.

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

Before 0.18.12 the Linux AppImage named its data directory after the AppImage
file, for example `~/.local/share/acquisition-0.18.11-x86_64.AppImage`, so each
release started empty. To keep your data from such a version, close
Acquisition and copy that directory's contents into
`~/.local/share/acquisition`.

On macOS the directory was `~/Library/Application Support/Acquisition` before
0.18.12. On a standard (case-insensitive) volume that is the same folder and
nothing changes. On a case-sensitive volume it is a different one: close
Acquisition and copy its contents into
`~/Library/Application Support/acquisition`.

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
