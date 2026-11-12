# Acquisition User Guide

Acquisition downloads your Path of Exile stash tabs and character inventories so
you can search them offline, price items, and publish those prices to forum shop
threads that the official trade site indexes.

Acquisition is neither affiliated with nor endorsed by Grinding Gear Games.

## Pages

| Page | What it covers |
|---|---|
| [Getting started](getting-started.md) | Installing, logging in with OAuth, choosing a realm and league, where your data lives |
| [Searching](searching.md) | Search tabs, the By Tab / By Item views, every filter, and the item table columns |
| [Mods and pseudomods](mods-and-pseudomods.md) | The Mods filter and the 35 summed "total" pseudomods |
| [Pricing](pricing.md) | Buyout types, tab-level prices and inheritance, in-game prices, legacy buyout import |
| [Forum shop](forum-shop.md) | Shop threads, the shop template, POESESSID, automatic shop updates |
| [Currency](currency.md) | The currency overview dialog and CSV export |
| [Settings and troubleshooting](settings-and-troubleshooting.md) | Refresh modes, rate limiting, themes, logging, updates, crash reports, data files |

## Sixty-second overview

1. Launch Acquisition and click **Authenticate**. A browser window opens for the
   Path of Exile OAuth grant. Pick a realm and league, then click **Login**.
2. Acquisition loads whatever it already has cached, then upon your request,
   fetches your stash tab list and downloads the checked tabs and characters.
3. Use the search form at the top of the window to filter items. Each search
   lives in its own tab; click **+** to open another.
4. Select an item or a tab and set a price with the buyout controls under the
   item list.
5. To advertise those prices, set a forum shop thread and a POESESSID under the
   **Shop** menu, then choose **Update forum shop(s)**.

## Getting help

- Issues: <https://github.com/smallcatlabs/acquisition-qt/issues>
- Discord: `gerwaric`

## A note on this guide

This guide first shipped with Acquisition 0.18.5 and is new; earlier versions
had no user documentation, so expect gaps.

Menu and dialog names are quoted exactly as they appear in the
application. If something here disagrees with the program, the program wins —
please open an issue so the page can be corrected.
