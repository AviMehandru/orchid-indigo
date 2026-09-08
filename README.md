# orchid-indigo

Three native desktop apps for the [yt-dlp archival pipeline][pipeline], one
per platform, each written in that platform's own language and toolkit.

[pipeline]: https://github.com/AviMehandru/orchid-ochre

| Platform | Toolkit | Language | Status |
|---|---|---|---|
| Linux | GTK4 + libadwaita | C | working |
| macOS | SwiftUI | Swift | complete |
| Windows | WinUI 3 | C# | not started |

**No Rust. No webview. No shared engine.** Each app depends only on what its
platform already ships.

## How this relates to the other two repositories

```
orchid-ochre    the pipeline: PowerShell, the CLI, the archive
orchid-cobalt   the Tauri desktop app (Rust + webview)
orchid-indigo   this repo
```

The pipeline is the only thing that *writes* an archive. Everything else
reads one, and starts downloads by handing a command line to the installed
`ytdl.ps1` exactly as a terminal would. Nothing here reimplements a single
stage of the pipeline; if a flag works in a terminal and not in one of these
windows, that is a bug in the window.

The Tauri app is not replaced by these and is not going away. It stays as the
Rust GUI; these are a separate third thing.

## Why three implementations, and what it costs

This is the expensive decision in the project, made deliberately and worth
being honest about.

Each app reimplements archive reading, progress parsing, health checks and
path resolution in its own language. There is no shared core — not a library,
not a helper process, not an FFI boundary. The benefit is that each app is
genuinely native, installs with nothing exotic, and can be built by anyone who
has that platform's ordinary toolchain.

The cost is that **the archive layout now has five consumers**:

1. `postprocess.ps1` writes it
2. `archive-viewer.py` reads it
3. the Tauri app reads it
4. `linux-gtk/` reads it
5. `macos/` and `windows/` will read it

Five copies of one agreement drift apart silently. A layout change that
updates four of them produces a green build everywhere and one platform whose
library comes up empty, with no error anywhere.

`docs/archive-layout.md` in the pipeline repo names the only mitigation that
works without shared code:

> Out-of-repo consumers are expected to keep their own conformance test that
> builds a fixture tree in this shape and asserts their reader finds it. That
> test is what turns a bump into a build failure on their side rather than a
> bug report from a user.

So that is not optional here, it is the deal. **Every app in this repository
carries its own conformance suite against a fabricated archive.** An app
without one does not belong in this repo.

## `CLI_VERSION`

One pin for the whole repository: which pipeline ref these apps expect, and
which archive layout version they read. Each app asserts that number against
its own constant in its own tests, so the copies cannot drift.

Deliberately one number for all three apps rather than one per app. Three
answers to "which layout does this project read" would mean the first symptom
of disagreement is one platform being broken while the others are fine, which
is the hardest version of this bug to find.

## Building

Each app builds independently with its platform's ordinary toolchain. There is
no top-level build system, because there is nothing for one to coordinate.

- **Linux** — see [`linux-gtk/README.md`](linux-gtk/README.md).
  `meson setup build && meson compile -C build`
- **macOS** — see [`macos-swiftui/README.md`](macos-swiftui/README.md).
  `open macos-swiftui/YtdlMac.xcodeproj`, or
  `xcodebuild -project macos-swiftui/YtdlMac.xcodeproj -scheme ytdl-macos build`.
  **It has never been compiled** — read that README's first paragraph before
  the first build.
- **Windows** — not started

## After a pipeline upgrade

Run every app's conformance suite. That is what this structure buys, and it
only pays off if the tests are actually run:

```sh
cd linux-gtk && meson test -C build
xcodebuild -project macos-swiftui/YtdlMac.xcodeproj -scheme ytdl-macos test
```

If one fails, the pipeline's layout moved and this repo has not caught up.
Read `docs/archive-layout.md` in the pipeline repo — the "Upgrading a reader"
section lists the changes in the order they bite — then update the app, then
raise `REQUIRES_ARCHIVE_LAYOUT`. In that order. Raising the pin first gives
you a green build that lies.

## The invariants every app here inherits

From `archive-viewer.py`, and not negotiable:

- **The archive is read-only.** Nothing under `Youtube Videos/` is created,
  moved or modified. `postprocess.ps1` writes a `checksums.sha256` covering
  every file in a video folder, so a derived file dropped in there makes that
  manifest stop verifying. Derived state goes to the platform's cache
  directory — and each app uses its *own*, because the index formats differ
  and a shared directory would mean each treating the others' files as
  corrupt.
- **No caller ever names a filesystem path.** Content is addressed by an
  opaque key plus an index into the entry's own file list, and the path is
  resolved from the index and re-checked. Traversal is off the table because
  no route accepts a path, not because a filter has to be right.
- **Nothing is re-encoded without being asked.**

## Licence

MIT. See [LICENSE](LICENSE).
