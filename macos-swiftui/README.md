# ytdl-macos

A native macOS front end for the yt-dlp archival pipeline. SwiftUI and Swift,
no Rust, no webview, no bundled runtime, and no third-party package of any
kind — SwiftUI, AppKit, AVKit, ImageIO and CryptoKit, all of which every Mac
already has.

**Nothing here has ever been compiled.** Every environment this was written in
is Linux — the Swift toolchain for Linux was unreachable from behind the
egress allowlist, and the device bridge provides an Ubuntu VM rather than a
Mac — so the first `xcodebuild` on a real Mac is this code's first compile.
It was written against the complete Linux app and reviewed file by file
instead. Treat a compile error as expected work rather than as a surprise.

It does not reimplement a stage of the pipeline. Downloads are started by
handing a command line to the installed `ytdl.ps1`, exactly as a terminal
would. What it does own is **reading the archive**, which is a fourth
independent implementation of `docs/archive-layout.md` — and why it carries its
own conformance suite.

## Build and run

Requires macOS 14 (Sonoma) and Xcode 15 or newer.

```
open YtdlMac.xcodeproj      # ⌘R to run, ⌘U to test
```

or from a terminal:

```
xcodebuild -project YtdlMac.xcodeproj -scheme ytdl-macos build
xcodebuild -project YtdlMac.xcodeproj -scheme ytdl-macos test
```

The scheme is shared and committed, so a fresh clone builds without opening
Xcode first.

**The project file is generated, not hand-edited.** After adding, renaming or
deleting a source file:

```
python3 Tools/generate-xcodeproj.py
```

Identifiers are derived from the file paths, so regenerating produces the same
file rather than a diff of shuffled hex. Editing `project.pbxproj` by hand is
how a source ends up in the repository and not in the build.

## What it needs at runtime

The same pipeline every app in this repository drives:

| Tool | Why |
|---|---|
| `pwsh` | every stage of the pipeline is a PowerShell 7 script |
| `yt-dlp` | the extraction itself |
| `ffmpeg` | merging, embedding, thumbnails |
| `ffprobe` | the Media tab's stream details; optional, and the page says so |

**A `.app` bundle inherits launchd's PATH, not a login shell's.** That is
`/usr/bin:/bin:/usr/sbin:/sbin` — no Homebrew — so a `pwsh` that every terminal
on the machine finds is invisible to a plain `which` inside the bundle. Both
Homebrew prefixes are therefore checked directly, and `/opt/homebrew/bin` and
`/usr/local/bin` are appended to the PATH of every child this app spawns.
Without that the pipeline fails to find `yt-dlp` and reports it as an extractor
error, several layers from the cause.

## The first launch

A Mac that has never run this app gets one profile, **Default**, written to
`~/Library/Application Support/ytdl-macos/profiles.json` before the window
opens. It carries the app's own defaults — every option unset — so selecting it
is "put the form back", not a preset that decides anything. That is deliberate:
quality, codec and container policy lives in `run_ytdlp.ps1`, on the far side of
the `CLI_VERSION` pin, and a profile shipped here that disagreed with it would
be a second opinion this window has no business having.

Nothing is *selected* on that first launch. The profile is somewhere to go back
to, not something applied to a form you have not touched yet.

It is an ordinary profile otherwise — rename it, save over it, delete it. **A
delete sticks.** The seed is keyed on `profiles.json` being absent rather than
on the list being empty, so once the file exists the default never comes back;
a profile you cannot get rid of would be worse than no profile at all. For the
same reason a `profiles.json` that exists but does not parse is left exactly as
it is: an unreadable store is still somebody's profiles, and replacing it with a
default is the one recovery nobody can undo.

## The panes

| Pane | What it does |
|---|---|
| Library | media cards, thumbnails, live search, threaded scan |
| Downloads | the option form, the command preview, queue, live progress, history, pause, cancel |
| Health | dependency probe, installed files, config, archive stats, log tails |
| Video detail | player, streams, metadata, comments, transcript, files, checksum verify |
| Profiles | named option sets, saved and restored; a fresh install starts with **Default** |

Nothing the GTK app does is missing.

## Where it keeps things

- `~/Library/Application Support/ytdl-macos` — settings, profiles, the queue,
  the run history. **Not** under Caches: macOS empties that directory on its
  own when the disk fills, and losing a set of profiles built up over months is
  losing real work.
- `~/Library/Caches/ytdl-macos` — derived and disposable.

Both are **this app's own**. The GTK app uses `$XDG_CACHE_HOME/ytdl-gtk` and
`archive-viewer.py` uses `ytdlp-archive-viewer`; three readers with three index
formats sharing a directory would mean each treating the others' files as
corrupt.

**Nothing derived is ever written inside the archive.** `postprocess.ps1`
writes a `checksums.sha256` over every file in a video folder, so a stray file
there makes that manifest stop verifying.

## The three things macOS does differently

**Playback degrades honestly.** AVFoundation does not read Matroska, which is
the pipeline's default container — this is the one place macOS is harder than
Linux rather than easier, because GStreamer plays it directly. What this app
does NOT do is remux to a cache directory the way the Tauri build did: that
apparatus is exactly what this project deleted, and it writes derived state for
something the user did not ask for. Instead `AVPlayer` plays what it can
(MP4/M4V/MOV/M4A and the other AVFoundation formats), and everything else gets
a sentence naming the container and a button that opens it in IINA, mpv or VLC.
`--container mp4` is a supported pipeline option, so a user who wants in-window
playback for everything has a real answer.

**Cancel goes through `posix_spawn`, not `Process`.** Cancel must kill the
process *group* — `ytdl.ps1` starts a child `pwsh`, which starts `yt-dlp`,
which starts `postprocess.ps1` and `ffmpeg`. `Foundation.Process` has no
child-setup hook and no way to ask for a new process group, so it cannot
express the requirement at all. The `/bin/sh -c 'set -m; exec …'` trick does
not work either: `exec` replaces the shell without forking, so no job-control
`setpgid` happens and the child stays in *this app's* group — where `kill(-pid)`
would take the app down with it. `Sources/Core/Spawn.swift` is sixty lines of
`posix_spawn` with `POSIX_SPAWN_SETPGROUP`, and it is the only C-level code
here.

**There is no toast, and no adaptive-width story.** libadwaita has
`AdwToastOverlay`; macOS has nothing equivalent and no honest way to build one,
so transient state lives in a permanent quiet status line at the bottom of the
window and the one case that must interrupt — no archive at all — gets an
alert. The GTK app is adaptive down to 360px because GNOME targets phone-shaped
windows; macOS does not, so the window sets a sensible minimum and stops.

## Tests

```
xcodebuild -project YtdlMac.xcodeproj -scheme ytdl-macos test
```

**Nothing in the suite touches YouTube, and nothing touches the machine's real
archive, install or preferences.** YouTube is unreachable from the environments
this project is developed in and no real download has ever been started through
any window in it, so everything is verified against fabrications:

- **Archive** — a fixture tree in the shape `docs/archive-layout.md` documents,
  covering the cases a layout-1 reader gets wrong: `Final Audio.<ext>`, a
  folder with no media at all, `Pre-merge streams/`, format-id names, a newer
  layout version, the folder-name fallback, path containment, and
  `REQUIRES_ARCHIVE_LAYOUT` asserted against this app's own constant.
- **Downloads** — the argument builder and both output parsers against
  fabricated output, and a real `/bin/sh` fake that **spawns a grandchild**, so
  killing the process group can be told from killing the child. A test against
  a single-process fake passes just as happily when only the child is killed.
- **Detail** — the comment tree against a deliberately awkward shape (a reply
  before its parent, an orphan whose parent is absent, equal like counts), and
  the caption collapse against rolling captions **three lines deep**. That third
  line is the whole test: comparing each cue against the last emitted text
  rather than the previous full cue works for two lines and then silently
  stops.
- **Profiles and settings** — written with `HOME` pointed at a temp directory,
  so running the suite cannot clobber the profiles of whoever runs it.

## Not yet true

`SWIFT_TREAT_WARNINGS_AS_ERRORS` is `NO`. The Linux app builds with `-Werror`
and this should too; it is off for the reason at the top of this file, because
a deprecation notice failing the build before the first clean one would be
noise rather than signal. Turn it on once it compiles clean — it is one line in
`Tools/generate-xcodeproj.py`.

No real download has been started through this window, on any platform. Nor
through any other window in this project.
