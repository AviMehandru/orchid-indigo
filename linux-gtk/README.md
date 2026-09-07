# ytdl-gtk — the Linux native app

A GTK4 front end for the yt-dlp archival pipeline, written in C.

**No Rust, no webview, no bundled runtime.** Three system libraries that a
GNOME desktop already has: GTK4, GLib and json-glib.

This is one of three standalone native apps in this repository (GTK4/C here,
SwiftUI on macOS, WinUI 3 on Windows). They share no code by design — see the
[repository README](../README.md) for why, and for what that costs.

## Build

```
sudo apt install libgtk-4-dev libjson-glib-dev meson ninja-build
meson setup build
meson compile -C build
meson test -C build
./build/ytdl-gtk
```

Fedora: `sudo dnf install gtk4-devel json-glib-devel meson ninja-build`.

Built with `-Werror`. The compiler is most of what replaces a borrow checker
in a C codebase, so its output is not advisory.

## Running

The archive is found the way the pipeline finds it — `$YTDLP_INSTALL_ROOT`,
then `~/yt-dlp`, `~/Documents/yt-dlp`, `~`. To point it somewhere else:

```
./build/ytdl-gtk --archive-root /mnt/nas/yt-dlp
```

That accepts anything reasonable: a data root, the `Youtube Videos` folder,
`Complete Archive` itself, or a single channel folder. Same acceptance set as
`archive-viewer.py --root`, so a path that works for one works for both.

## What works

Archive discovery and indexing (layout 1 and 2), media-file selection,
thumbnails, the library grid, live search, threaded scanning with progress.

## What is not built yet

Downloads, health, video detail, settings, playback. The Downloads and Health
panes are placeholders that say so — the window states what it is rather than
looking finished and doing nothing.

## The conformance test

`tests/test_archive.c` builds a fixture tree in the shape
`docs/archive-layout.md` **in the pipeline repo** documents, and asserts this
reader finds it. The contract asks every out-of-repo consumer to keep one:

> This repo's tests passing and the pipeline's tests passing says nothing
> about the two agreeing; only a fixture in the documented shape does.

It covers the cases a layout-1 reader gets wrong — `Final Audio.<ext>`, a
folder with no media at all, `Pre-merge streams/`, format-id names, a newer
layout version — plus path containment and the folder-name fallback.

**Run it after any pipeline upgrade.** That is the whole point of it.

## The archive is read-only

Nothing here creates, moves or modifies anything under `Youtube Videos/`.
`postprocess.ps1` writes a `checksums.sha256` covering every file in a video
folder, so a derived file dropped in there makes that manifest stop verifying.
Derived state goes to `$XDG_CACHE_HOME/ytdl-gtk`, which is deliberately not
shared with the Python viewer's or the Tauri app's cache — different index
formats, and each would treat the others' files as corrupt.
