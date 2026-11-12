# Acquisition

Acquisition is an inventory management tool for [Path of Exile](https://www.pathofexile.com/) that has been around for over a decade:

- You can download all of your stash tabs and character inventories for offline search.

- You can list items for trade using forum shop threads, which are indexed by the official trade site. This allows you to list items in remove-only tabs as well as character inventories, which are not otherwise indexed on the official trade site.

Acquisition can run on Windows, macOS, Linux.

Acquisition is being reimplemented in Rust at [smallcatlabs/acquisition](https://github.com/smallcatlabs/acquisition): a daemon, a CLI and an MCP server first, with a GUI to follow (see [ADR 0003](docs/adr/0003-rewrite-vs-evolve.md)). This repository is the Qt application, maintained and released until the replacement ships.

You can download setup packages from [the releases page](https://github.com/smallcatlabs/acquisition-qt/releases).

## User guide

The [user guide](docs/user/README.md) covers logging in, searching and filters, mods and pseudomods, pricing, forum shops, currency tracking, and troubleshooting. It is also reachable from the **Help** menu inside the application.

## Building Acquisition

Acquisition is written in C++ and uses the Qt widget toolkit. It was originally a qmake project, but has been migrated to cmake.

See [BUILD.md](BUILD.md) for more detailed build and release packaging guidance.

## Command line

`acquisition [--data-dir <path>] [--log-level <level>]`

`--data-dir <path>`:
	Set the path where Acquisition should save its data.
	The default on Windows is `%localappdata%\acquisition`.
	The default on macOS is `~/Library/Application Support/acquisition`.
	The default on Linux is `~/.local/share/acquisition`.

`--log-level <level>`:
	Controls the amount of detail in the log.
	Options are `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `FATAL`, and `OFF`.
	This option is case-insensitive.
	The default level for release builds is `INFO`.
	The default level for debug builds is `DEBUG`.

## Reporting issues

If you're having problems with Acquisition, please check the issues page: https://github.com/smallcatlabs/acquisition-qt/issues

You can also contact me on Discord as gerwaric.

## SAST Tools

[PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - static analyzer for C, C++, C#, and Java code.

<a href="https://scan.coverity.com/projects/gerwaric-acquisition">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/31083/badge.svg"/>
</a>
