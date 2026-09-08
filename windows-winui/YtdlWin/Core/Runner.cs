/* The download engine: what actually starts a run.
 *
 * ONE RUN AT A TIME, deliberately. The queue is strictly sequential because
 * independent `ytdl` invocations race on shared state (global_manifest.json,
 * channel_manifest.json, the Channel Info refresh throttle, download.log).
 * --workers N is the supported way to get real parallelism -- it enumerates
 * every video up front so no two workers are assigned the same one, and
 * postprocess.ps1 has matching file locking. So "run several at once" is the
 * workers control, not a wider queue, and the queue never starts a second
 * process.
 *
 * THREADING, AND WHY THIS CLASS KNOWS NOTHING ABOUT WinUI.
 *
 * One worker thread owns the queue and the child process. It never touches the
 * UI: lines and state changes go into a buffer under a lock, and the window
 * calls Drain() from a DispatcherQueueTimer to collect them. yt-dlp redraws its
 * progress line several times a second, and pushing each line into a bound
 * collection as it arrived would put the layout system in a re-render storm for
 * the whole of a long download.
 *
 * The other two ports make their runner the observable object itself --
 * @Published on the Swift side, GObject properties on the C side. This one does
 * not, and that is the one structural difference worth pointing at: it means
 * the runner has no dependency on Microsoft.UI at all, which is what lets the
 * conformance suite drive a real run against a fake ytdl.ps1 without starting a
 * window. Given that nothing in this app has ever been compiled, a core that
 * can be tested without a UI thread is worth the small awkwardness of a manual
 * drain.
 */

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Threading;

namespace YtdlWin.Core;

/// One drained batch: the lines that arrived, and the state if it changed.
public sealed class RunnerSnapshot
{
    public required IReadOnlyList<(string Text, bool Transient)> Lines { get; init; }
    /// null when nothing about the queue, history, progress or pause state
    /// changed since the last drain -- which is most ticks.
    public RunnerState? State { get; init; }
}

public sealed class RunnerState
{
    public required IReadOnlyList<RunRecord> Queue { get; init; }
    public required IReadOnlyList<RunRecord> History { get; init; }
    public RunRecord? Current { get; init; }
    public required RunProgress Progress { get; init; }
    public required bool Paused { get; init; }
}

public sealed class Runner
{
    private const int MaxLogLines = 4000;
    private const int MaxHistory = 300;
    /// At most this many lines cross to the UI per drain. A run that produces
    /// output faster than the window can draw must not let one tick starve the
    /// frame clock; the backlog simply moves on the next tick.
    public const int MaxLinesPerDrain = 400;

    private readonly object _lock = new();
    private readonly List<RunRecord> _queue = new();
    private readonly List<RunRecord> _history = new();
    private readonly List<(string Text, bool Transient)> _pendingLines = new();
    private RunRecord? _current;
    private RunProgress _progress = new();
    private bool _paused;
    private bool _stateDirty;
    private bool _cancelRequested;
    private bool _stopRequested;
    private uint _counter;
    private SpawnedChild? _child;

    private Thread? _worker;
    private readonly ManualResetEventSlim _stopped = new(false);

    /* Serialises the two JSON writes so two snapshots cannot reach the same
     * file out of order. A dedicated thread rather than the thread pool: a
     * pool item queued during shutdown may never run, and the last write is
     * the one that records what finished. */
    private readonly object _persistLock = new();

    // MARK: - Lifecycle

    public Runner()
    {
        /* Restore what the last session left behind. A queue that survives a
         * restart is the difference between "I queued twelve channels
         * overnight" being a plan and being a thing you have to babysit. */
        _history.AddRange(ReadRecords(StateFile("history.json")));
        _queue.AddRange(ReadRecords(StateFile("queue.json")));

        /* Anything recorded as running belongs to a process that died with the
         * last window. Left as "running" it would be a row that never resolves.
         *
         * On this platform that is usually literally true rather than merely
         * likely: the job object is KILL_ON_JOB_CLOSE, so the tree died with
         * the app whether it exited cleanly or was killed. */
        foreach (var record in _history.Where(r => r.State == "running"))
        {
            record.State = "failed";
            if (record.LastLine.Length == 0)
                record.LastLine = "Interrupted — the window closed while this was running.";
        }
    }

    /* Separate from the constructor so the window can be built, and only then
     * begin accepting runs -- rather than having a restored queue start
     * emitting into a view that does not exist yet. */
    public void Start()
    {
        lock (_lock)
        {
            if (_worker is not null) return;
            _worker = new Thread(WorkerLoop)
            {
                Name = "ytdl-runner",
                IsBackground = true,
            };
            _worker.Start();
        }
    }

    /* Ask the worker to finish, and take the running download's process tree
     * with it. Safe to call more than once.
     *
     * Called when the window closes. The wait is bounded because a queue that
     * keeps downloading after its window is gone -- with nothing reading its
     * output -- is the thing this has to prevent, and a shutdown that hangs
     * waiting for a courtesy is worse than one that does not wait. */
    public void Stop()
    {
        SpawnedChild? child;
        lock (_lock)
        {
            _stopRequested = true;
            child = _child;
            /* Monitor.PulseAll, not a separate event. The worker parks in
             * Monitor.Wait on THIS lock, and only a pulse on the same object
             * wakes it -- an AutoResetEvent set from here would be ignored and
             * the worker would not notice the stop until its next 500ms poll. */
            Monitor.PulseAll(_lock);
        }

        if (child is not null) ProcessTree.KillTree(child);

        if (_worker is not null)
        {
            _stopped.Wait(TimeSpan.FromSeconds(2));
            _worker = null;
        }
    }

    // MARK: - Public actions

    public sealed class EnqueueException : Exception
    {
        public EnqueueException(string message) : base(message) { }
    }

    public string Enqueue(RunOptions opts)
    {
        if (string.IsNullOrWhiteSpace(opts.Url))
            throw new EnqueueException("Enter a URL first.");

        var record = new RunRecord
        {
            Opts = opts.Clone(),
            Command = opts.CommandPreview(),
            State = "queued",
            Started = Format.NowUnix(),
        };

        lock (_lock)
        {
            /* Unique enough without a GUID: the clock, the pid and a counter.
             * These only have to be distinct within one history file. */
            _counter++;
            record.Id = string.Format(CultureInfo.InvariantCulture, "{0}-{1:x}{2:x}",
                Format.NowUnix(), Environment.ProcessId, _counter);
            _queue.Add(record);
            _stateDirty = true;
            PersistLocked();
            Monitor.PulseAll(_lock);
        }
        return record.Id;
    }

    /// True if something was actually running.
    public bool Cancel()
    {
        SpawnedChild? child;
        lock (_lock)
        {
            child = _child;
            if (child is not null) _cancelRequested = true;
        }
        if (child is null) return false;

        /* Off the caller's thread. TerminateJobObject itself returns quickly,
         * but the handle work around it can block on a wedged process, and
         * doing that on the UI thread freezes the window at the exact moment
         * the user pressed Cancel. */
        ThreadPool.QueueUserWorkItem(_ => ProcessTree.KillTree(child));
        return true;
    }

    public void SetPaused(bool value)
    {
        lock (_lock)
        {
            _paused = value;
            _stateDirty = true;
            Monitor.PulseAll(_lock);
        }
    }

    public void RemoveQueued(string id)
    {
        lock (_lock)
        {
            _queue.RemoveAll(r => r.Id == id);
            _stateDirty = true;
            PersistLocked();
        }
    }

    public void ClearHistory()
    {
        lock (_lock)
        {
            _history.Clear();
            _stateDirty = true;
            PersistLocked();
        }
    }

    public bool IsRunning
    {
        get { lock (_lock) { return _current is not null; } }
    }

    // MARK: - Draining to the UI thread

    /// <summary>Collect what the worker has produced since the last call. Called
    /// from the UI thread on a timer; never blocks on anything but the lock.</summary>
    public RunnerSnapshot Drain()
    {
        List<(string, bool)> lines;
        RunnerState? state = null;

        lock (_lock)
        {
            var take = Math.Min(_pendingLines.Count, MaxLinesPerDrain);
            if (take > 0)
            {
                lines = _pendingLines.GetRange(0, take);
                _pendingLines.RemoveRange(0, take);
            }
            else
            {
                lines = new List<(string, bool)>();
            }

            if (_stateDirty)
            {
                _stateDirty = false;
                /* Deep-copied on the way out. The records in _queue and
                 * _history keep being mutated by the worker -- LastLine and the
                 * four counts are written for every line of a running download
                 * -- and handing the UI the live objects would mean a bound
                 * list being modified from a background thread, which in WinUI
                 * is not a race that produces a wrong number but one that
                 * throws inside the layout pass. */
                state = new RunnerState
                {
                    Queue = _queue.Select(r => r.Clone()).ToList(),
                    History = _history.Select(r => r.Clone()).ToList(),
                    Current = _current?.Clone(),
                    Progress = _progress.Clone(),
                    Paused = _paused,
                };
            }
        }

        return new RunnerSnapshot { Lines = lines, State = state };
    }

    /// The state as it stands, for the first paint before any drain has run.
    public RunnerState CurrentState()
    {
        lock (_lock)
        {
            return new RunnerState
            {
                Queue = _queue.Select(r => r.Clone()).ToList(),
                History = _history.Select(r => r.Clone()).ToList(),
                Current = _current?.Clone(),
                Progress = _progress.Clone(),
                Paused = _paused,
            };
        }
    }

    // MARK: - The worker

    private void WorkerLoop()
    {
        while (true)
        {
            RunRecord item;
            lock (_lock)
            {
                while (_queue.Count == 0 || _paused)
                {
                    if (_stopRequested)
                    {
                        _stopped.Set();
                        return;
                    }
                    /* The lock is released across the wait, which is what
                     * Monitor.Wait does and what makes this correct: Enqueue
                     * takes the same lock, so holding it here would deadlock
                     * the thing this is waiting for. The 500ms timeout is a
                     * backstop, not the mechanism -- every state change pulses
                     * this lock. */
                    Monitor.Wait(_lock, 500);
                }
                if (_stopRequested)
                {
                    _stopped.Set();
                    return;
                }

                item = _queue[0];
                _queue.RemoveAt(0);
                _stateDirty = true;
                PersistLocked();
            }

            item.State = "running";
            item.Started = Format.NowUnix();
            RunOne(item);
        }
    }

    private void RunOne(RunRecord record)
    {
        var pwsh = Paths.FindPwsh();
        if (pwsh is null)
        {
            Fail(record,
                "pwsh (PowerShell 7) was not found. Every stage of this pipeline is a PowerShell " +
                "script, so nothing can run without it — install it with " +
                "`winget install Microsoft.PowerShell` and re-run setup. Windows PowerShell 5.1 " +
                "is not a substitute: the pipeline's scripts need pwsh 7.");
            return;
        }

        var script = Paths.Join(Paths.ScriptsDir(), "ytdl.ps1");
        if (!Paths.IsRegularFile(script))
        {
            Fail(record,
                $"{script} does not exist. This app drives the installed pipeline, not a " +
                "checkout — run the installer, or set YTDLP_INSTALL_ROOT to where it lives.");
            return;
        }

        /* Where this run will write. --workers > 1 splits into
         * download.worker-<id>.log files instead, which the history row links to
         * by directory rather than by name. */
        var dataRoot = string.IsNullOrWhiteSpace(record.Opts.DataRoot)
            ? Paths.InstallRoot()
            : Paths.ExpandTilde(record.Opts.DataRoot.Trim());
        record.LogPath = Paths.Join(Paths.Join(dataRoot, @"Archive Logs\Logs"), "download.log");

        lock (_lock)
        {
            _progress = new RunProgress { Stage = "starting" };
            _current = record;
            _cancelRequested = false;
            _stateDirty = true;
        }

        var env = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (System.Collections.DictionaryEntry e in Environment.GetEnvironmentVariables())
        {
            if (e.Key is string k && e.Value is string v) env[k] = v;
        }
        env["PATH"] = Paths.ChildPath();

        SpawnedChild child;
        try
        {
            /* -NoProfile matters more here than on the other two platforms. A
             * Windows machine is far more likely to have a populated
             * $PROFILE -- Oh My Posh, a corporate logon script, module
             * autoloading -- and every line of it would print into this run's
             * log before the pipeline said anything, and would slow the start
             * of every queued item. */
            var args = new List<string> { "-NoProfile", "-NonInteractive", "-File", script };
            args.AddRange(record.Opts.ToArgs());
            child = ProcessTree.Run(pwsh, args, env);
        }
        catch (Exception ex)
        {
            Fail(record, ex.Message);
            return;
        }

        lock (_lock) { _child = child; }

        if (child.JobAssignmentFailed)
        {
            Ingest("Note: this run's process tree could not be placed in a job object, so " +
                   "Cancel can only stop the top process. The download itself is unaffected.",
                   transient: false);
        }

        /* Both pumps must finish before the status is read: they are what fill
         * in LastLine and the session summary, and a record written while they
         * are still draining would lose the counts the run just reported.
         *
         * Two threads rather than reading one and then the other, because a
         * child that fills the stderr pipe while this end is still reading
         * stdout deadlocks -- the classic redirected-process hang, and one that
         * a --workers run producing warnings would hit for real. */
        var pumps = new[]
        {
            StartPump(child.Process.StandardOutput.BaseStream),
            StartPump(child.Process.StandardError.BaseStream),
        };

        // Nothing is ever written to the child, and an open stdin leaves yt-dlp
        // able to wait on one forever.
        try { child.Process.StandardInput.Close(); } catch (Exception) { /* already gone */ }

        foreach (var pump in pumps) pump.Join();
        var code = child.WaitExitCode();

        bool cancelled;
        lock (_lock)
        {
            cancelled = _cancelRequested;
            _cancelRequested = false;
            _child = null;

            /* Nothing is copied out of _current here, and it is worth saying
             * why, because the ports this is based on DO copy at this point.
             *
             * In Swift a RunRecord is a struct, so `pendingCurrent` is a
             * separate copy and the counts the pumps wrote into it have to be
             * carried back to the record being finished. In C# it is a class:
             * _current IS this same object, and Ingest has been writing
             * LastLine and the four counts straight into it all along. A copy
             * here would be a no-op, and writing one would suggest the two are
             * distinct -- which is exactly the misreading that would cause
             * somebody to later "fix" it by cloning and silently lose every
             * count the run reported. */
        }
        child.Dispose();

        record.ExitCode = code;
        record.Finished = Format.NowUnix();
        record.State = cancelled ? "cancelled" : (code == 0 ? "done" : "failed");
        Finish(record);
    }

    private Thread StartPump(System.IO.Stream stream)
    {
        var t = new Thread(() => ProcessTree.PumpLines(stream, Ingest))
        {
            IsBackground = true,
            Name = "ytdl-pump",
        };
        t.Start();
        return t;
    }

    /// Called on a pump thread for every line of the child's output.
    private void Ingest(string line, bool transient)
    {
        lock (_lock)
        {
            if (OutputParser.ParseProgressLine(line, _progress)) _stateDirty = true;

            if (_current is not null)
            {
                _current.LastLine = line;
                if (OutputParser.ParseSessionSummary(line) is { } summary)
                {
                    _current.VideosTouched = summary.Videos;
                    _current.ArchiveSkipped = summary.Skipped;
                    _current.Errors = summary.Errors;
                    _current.Warnings = summary.Warnings;
                }
                _stateDirty = true;
            }

            _pendingLines.Add((line, transient));
            /* Bounded on the PRODUCER side too. The drain takes at most 400
             * lines per tick, so a run that outpaces it -- a --sync of a large
             * channel does -- would otherwise grow this without limit for the
             * length of the download. The oldest lines are the ones to lose. */
            if (_pendingLines.Count > MaxLogLines)
                _pendingLines.RemoveRange(0, _pendingLines.Count - MaxLogLines);
        }
    }

    private void Fail(RunRecord record, string why)
    {
        record.State = "failed";
        record.Finished = Format.NowUnix();
        record.LastLine = why;

        lock (_lock) { _pendingLines.Add((why, false)); }
        Finish(record);
    }

    private void Finish(RunRecord record)
    {
        lock (_lock)
        {
            _current = null;
            _child = null;
            _progress = new RunProgress();
            _history.Insert(0, record);
            if (_history.Count > MaxHistory)
                _history.RemoveRange(MaxHistory, _history.Count - MaxHistory);
            _stateDirty = true;
            PersistLocked();
        }
    }

    // MARK: - Persistence

    public static string StateFile(string name) => Paths.Join(Paths.StateDir(), name);

    /* Caller holds the lock. Snapshots under it and writes OUTSIDE it.
     *
     * Two JSON files is real disk work, and this lock is taken by both pump
     * threads for every line of a running download. Writing while holding it
     * stalled the whole run -- and the UI thread with it, since Drain and
     * Enqueue take the same lock. */
    private void PersistLocked()
    {
        var history = _history.Select(r => r.Clone()).ToList();
        var queue = _queue.Select(r => r.Clone()).ToList();

        ThreadPool.QueueUserWorkItem(_ =>
        {
            lock (_persistLock)
            {
                WriteRecords(history, StateFile("history.json"));
                WriteRecords(queue, StateFile("queue.json"));
            }
        });
    }

    public static void WriteRecords(IReadOnlyList<RunRecord> records, string path)
    {
        var data = JsonFile.Write(w =>
        {
            w.WriteStartArray();
            foreach (var r in records) r.WriteTo(w);
            w.WriteEndArray();
        });
        AtomicFile.Write(data, path);
    }

    public static List<RunRecord> ReadRecords(string path)
    {
        var array = JsonFile.Array(path);
        if (array is null) return new List<RunRecord>();

        var records = new List<RunRecord>();
        foreach (var item in array.Value.EnumerateArray())
        {
            if (item.ValueKind == JsonValueKind.Object) records.Add(RunRecord.FromJson(item));
        }
        return records;
    }
}
