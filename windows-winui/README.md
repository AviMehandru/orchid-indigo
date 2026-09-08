# ytdl-win

A native Windows front end for the yt-dlp archival pipeline. WinUI 3 and C#, no
Rust, no webview, and no third-party package of any kind — the Windows App SDK,
.NET and the Windows SDK projections, and nothing else.

**Nothing here has ever been compiled.** Every environment this was written in
is Linux, and no .NET SDK was reachable from it: `dot.net`,
`builds.dotnet.microsoft.com`, `packages.microsoft.com`, `api.nuget.org` and
`download.visualstudio.microsoft.com` are all refused by the egress proxy, so
not even `dotnet build` could be attempted, let alone a Windows build. It was
written against the complete GTK and SwiftUI apps and reviewed file by file
instead. **Treat a compile error as expected work rather than as a surprise**,
and read "What is most likely to be wrong" below before the first build.

It does not reimplement a stage of the pipeline. Downloads are started by
handing a command line to the installed `ytdl.ps1`, exactly as a terminal would.
What it does own is **reading the archive**, which is a fifth independent
implementation of `docs/archive-layout.md` — and why it carries its own
conformance suite.

## Build and run

Requires Windows 10 1809 or newer, the .NET 8 SDK, and the Windows App SDK
workload (Visual Studio 2022 with "Windows application development", or the
standalone SDK).

```
dotnet build YtdlWin.sln
dotnet run --project YtdlWin\YtdlWin.csproj
dotnet test YtdlWin.Tests\YtdlWin.Tests.csproj
```

or open `YtdlWin.sln` in Visual Studio.

The app builds **unpackaged** by default (`WindowsPackageType=None`), because
the pipeline lives at `C:\yt-dlp` and this app reads an archive anywhere on
disk; an MSIX container's filesystem access is a separate conversation for no
benefit to a tool installed alongside a PowerShell pipeline. Everything in the
app is written to work either way — which is why `Settings` uses a plain JSON
file rather than `ApplicationData.Current`, an API that throws outright when
unpackaged.

**The test project compiles the core from source rather than referencing the
app.** That is deliberate: `dotnet test` then needs nothing but the .NET SDK, no
XAML compiler and no Windows App SDK, so the suite can run on a machine where
the app itself does not build yet. For code in this state that is worth more
than architectural tidiness. See the comment at the top of the test `.csproj`.

## What is most likely to be wrong

In descending order of likelihood, since none of it has been checked:

1. **The three `PackageReference` versions.** `Microsoft.WindowsAppSDK`,
   `Microsoft.Windows.SDK.BuildTools`, and the xunit/test-SDK trio are plausible
   version strings, not confirmed ones — no NuGet feed was reachable. If restore
   fails, check nuget.org and pin what is actually current. This is expected,
   not a symptom of anything deeper.
2. **XAML that does not compile.** Property names, attached properties and
   `x:Bind` paths were written from knowledge of the framework rather than
   against a compiler. `Themes/Generic.xaml` is the riskiest file: a control
   whose template is not found does not error, it lays out as a zero-height
   nothing, so if the Downloads form's rows are simply absent that is where to
   look.
3. **The P/Invoke in `Core/ProcessTree.cs`.** The job-object structure layouts
   and the `QueryInformationJobObject` buffer arithmetic are the kind of thing
   that is either exactly right or crashes. The tests in `RunnerProcessTests`
   exercise all of it.
4. **`MediaPlayerElement` behaviour.** See "playback" below — the whole strategy
   rests on `MediaFailed` firing promptly for a container Windows cannot decode,
   which was reasoned about and not observed.

## What it needs at runtime

The same pipeline every app in this repository drives:

| Tool | Why |
|---|---|
| `pwsh` | every stage of the pipeline is a PowerShell 7 script |
| `yt-dlp` | the extraction itself |
| `ffmpeg` | merging, embedding, thumbnails |
| `ffprobe` | the Media tab's stream details; optional, and the page says so |

**Windows PowerShell 5.1 is not a substitute and is deliberately never used as a
fallback.** `powershell.exe` is on every Windows machine, which makes it exactly
the tempting wrong answer: the pipeline's scripts use pwsh 7 syntax and
`$IsWindows`, so 5.1 would fail partway through a run with a parse error rather
than not starting. `setup.ps1` is the one file in the whole project written for
5.1, and its job is to install pwsh 7.

Unlike the macOS app, this one does *not* have to work around a launcher PATH: a
Windows GUI process inherits the PATH Explorer was started with, which is the
user's. What it does not see is a PATH entry added since the user last signed
in — and "I just installed PowerShell and this still says it is missing" is
exactly the moment somebody opens the Health pane. So the standard install
locations of the three ways pwsh arrives (MSI, Store alias, Chocolatey/scoop
shim) are checked directly.

## The panes

| Pane | What it does |
|---|---|
| Library | media cards, thumbnails, live search, virtualised grid, threaded scan |
| Downloads | the option form, the command preview, queue, live progress, history, pause, cancel |
| Health | dependency probe, installed files, config, archive stats, log tails |
| Video detail | player, streams, metadata, comments, transcript, files, checksum verify |
| Profiles | named option sets, saved and restored |

Nothing the GTK or SwiftUI apps do is missing.

## Where it keeps things

- `%LOCALAPPDATA%\ytdl-win\state` — settings, profiles, the queue, the run
  history.
- `%LOCALAPPDATA%\ytdl-win\cache` — derived and disposable.

**Local, not Roaming**, and that is the interesting choice. Roaming is where
user settings that should follow a person between machines belong, and profiles
would qualify — but `queue.json` and `history.json` are full of absolute paths,
and roaming a queue to a machine where those folders do not exist produces a
queue that fails one item at a time for reasons nothing explains. One directory
that is honestly machine-local beats a split that would roam half a coherent
state.

Both are **this app's own**. The GTK app uses `$XDG_CACHE_HOME/ytdl-gtk`, the
macOS app `~/Library/Caches/ytdl-macos`, and `archive-viewer.py`
`ytdlp-archive-viewer`; readers with different index formats sharing a directory
would mean each treating the others' files as corrupt.

**Nothing derived is ever written inside the archive.** `postprocess.ps1` writes
a `checksums.sha256` over every file in a video folder, so a stray file there
makes that manifest stop verifying.

## The four things Windows does differently

**Cancel is a job object, not `taskkill /T`.** This is the biggest departure
from what `claude/native-frontend-architecture.md` assumed Windows would do,
and it is a correctness fix rather than a preference. `taskkill /T` walks the
parent/child chain from a point-in-time snapshot, so it misses a descendant
spawned during the walk — and, worse, any descendant whose parent has already
exited, because the chain it walks is broken there. yt-dlp exiting before ffmpeg
finishes is not a rare case; it is the normal shape of the end of a download. A
job object has no snapshot and no chain: a child of a process in a job is in
that job by construction, and `TerminateJobObject` takes all of them in one
call with no dependency on anything being installed.

That buys something the other two platforms do not have. The job is created
`KILL_ON_JOB_CLOSE`, so the download tree dies with this process **even when the
app is killed outright** rather than quitting. On Linux and macOS an app killed
with SIGKILL orphans its download and it runs to completion with nothing reading
it.

It costs something too, and the cost is real: **there is no SIGTERM.** The GTK
and macOS apps send SIGTERM, wait, then SIGKILL, which gives ffmpeg a moment to
close its output file. Windows offers no way to ask a console-less process tree
to stop politely — `GenerateConsoleCtrlEvent` needs a shared console, and this is
a GUI process whose children deliberately have none. So cancel is the equivalent
of SIGKILL to everything, immediately, and a cancelled run here can leave a
partially written file where the same cancel on Linux would not. That is
tolerable rather than good: the pipeline writes into `_incomplete/` and sweeps
it, and the layout contract already tells every consumer to tolerate a truncated
media file. It should not be papered over — a graceful cancel needs a real
answer, not a shorter timeout.

**Output is read as raw bytes, and `\r\n` is one terminator.** The obvious .NET
answer is `BeginOutputReadLine`, and it cannot be used: it splits on `\r` as
well as `\n` but does not tell the caller *which* terminator ended a line — and
that single bit is the entire difference between a progress bar that redraws in
place and a log with four thousand near-identical rows in it. So
`ProcessTree.PumpLines` reads the stream itself. The Windows-specific half is
that everything the pipeline emits through PowerShell ends `\r\n`: treating the
`\r` as a redraw and the `\n` as an end-of-line would mark every ordinary log
line transient, and the log would show one line at a time overwriting itself.
Neither of the other two ports has this problem, and neither has a test for it;
this one does, including the case where the `\r\n` straddles a read boundary.

**Playback does not predict, it asks.** The macOS app decides from a static
extension list because its answer is crisp and permanent: AVFoundation does not
read Matroska, full stop. Windows has no such list to write. MP4/H.264/AAC is
in the box, Matroska has been readable since Windows 10, and VP9, Opus, AV1 and
HEVC each depend on a Store media extension that may or may not be present, may
be in-box on one Windows build and not another, and in HEVC's case is a paid
item. So the player is handed the original file and `MediaFailed` decides —
and because ffprobe has already read the container by then, the note that
replaces the player can name the codec that actually failed and the specific
Store extension it needs, rather than guessing from the extension. That is a
better message than either of the other two apps can give.

What it does **not** do is quietly remux to a cache directory. That apparatus is
exactly what this project deleted, and it would be writing derived state for
something nobody asked for.

**MAX_PATH is handled explicitly.** This is the one correctness problem that is
this platform's alone. `run_ytdlp.ps1` keeps the install root at `C:\yt-dlp`
rather than under the user profile *because* of MAX_PATH — the per-video paths
this pipeline builds run to about 240 characters before the data root is
prefixed. Past 260 an ordinary Win32 path fails, and it does not fail loudly:
the directory enumeration comes back empty, the most deeply nested files
(`Video metadata/`, `Pre-merge streams/`) are missing from the listing, and the
library shows a video with no metadata and no error anywhere. `app.manifest`
declares `longPathAware`, which is necessary and not sufficient — it only takes
effect when the machine's `LongPathsEnabled` policy is also on, which by default
it is not — so `Paths.Extended()` prefixes `\\?\` past a threshold and the scan
uses it. `ArchiveConformanceTests` builds a genuinely over-260-character fixture
and asserts the reader finds what is in it.

## Tests

```
dotnet test YtdlWin.Tests\YtdlWin.Tests.csproj
```

**Nothing in the suite touches YouTube, and nothing touches the machine's real
archive, install or preferences.** YouTube is unreachable from the environments
this project is developed in and no real download has ever been started through
any window in it, so everything is verified against fabrications:

- **Archive** — a fixture tree in the shape `docs/archive-layout.md` documents,
  covering the cases a layout-1 reader gets wrong: `Final Audio.<ext>`, a folder
  with no media at all, `Pre-merge streams/`, format-id names, a newer layout
  version, the folder-name fallback, path containment, and
  `REQUIRES_ARCHIVE_LAYOUT` asserted against this app's own constant by reading
  CLI_VERSION rather than hardcoding the number. Plus the two failures that are
  this platform's own: a manifest whose `media_file` is spelled with
  backslashes, and a video folder past MAX_PATH.
- **Downloads** — the argument builder and both output parsers against
  fabricated output; the line pump over a `MemoryStream` for every combination
  of `\n`, `\r` and `\r\n` including a chunk-straddling one; and a real
  `cmd.exe` fake that **spawns a grandchild**, with the assertion made against
  the job's process list rather than a pid, because on Windows a pid is not a
  question you can ask.
- **Detail** — the comment tree against a deliberately awkward shape (a reply
  before its parent, an orphan whose parent is absent, equal like counts read
  twice to catch an unstable sort), and the caption collapse against rolling
  captions **three lines deep**. That third line is the whole test: comparing
  each cue against the last emitted text rather than the previous full cue works
  for two lines and then silently stops.
- **Profiles and settings** — written with the profile directory pointed at a
  temp directory, so running the suite cannot clobber the profiles of whoever
  runs it.
- **Culture** — the percentage parser, the JSON number reader and the formatters
  are asserted under a `de-DE` culture. `.NET` formats and parses against
  `CurrentCulture` by default, so "23.976" read on a machine whose decimal
  separator is a comma gives 23976 — a frame rate three orders of magnitude
  wrong, with nothing throwing.

Test parallelism is **off** for the assembly, and that is a correctness
requirement rather than a preference — `Paths.EnvironmentOverride` and Health's
probe cache are static by design, and a class redirecting the home directory
while another reads the real environment fails in whichever test happens to be
running rather than the one that caused it.

## Not yet true

`TreatWarningsAsErrors` is `false`. The GTK app builds with `-Werror` and this
should too; it is off for the reason at the top of this file, because a
deprecation warning failing the build before the first clean one would be noise
rather than signal. Turn it on once it compiles clean — it is one line in the
`.csproj`, and the same line is the same compromise the macOS app records.

The job assignment happens immediately *after* `Process.Start` rather than with
`CREATE_SUSPENDED`, so there is a microseconds-wide window in which a child
could spawn a descendant outside the job. The window is written down in
`ProcessTree.cs` rather than hidden, along with why closing it was judged the
wrong trade for code that has never run.

No real download has been started through this window, on any platform. Nor
through any other window in this project.
