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

OAuth covers reading your stashes and characters. Posting to forum shop threads
still needs a POESESSID cookie; see [Forum shop](forum-shop.md).

### Advanced options

**Show Advanced Options** reveals:

| Option | Effect |
|---|---|
| Remember me | Keep the account and league for the next launch |
| Report crashes | Send crash reports to the developer via [Sentry](https://sentry.io) |
| Use system proxy | Route network traffic through the operating system's proxy settings |
| Logging Level | Same as the **Settings → Logging** menu in the main window |
| Theme | Dark, Light, or Default (the platform theme) |
| Profile Folder | Shows the data directory in use (see below) |

## What happens after login

The main window title reads `Acquisition [version] - <League> League [<account>]`.

Acquisition first loads any items it cached from a previous session, so a large
account is searchable within seconds of logging in. Then you can use it to fetch
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

Use `--data-dir <path>` to run with a different directory, for example to keep
two accounts apart or to try a beta without touching your real data:

```
acquisition --data-dir /path/to/other/dir
```

The other command-line option is `--log-level <level>`; see
[Settings and troubleshooting](settings-and-troubleshooting.md#logging).
