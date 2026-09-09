/* ytdl-win -- a native front end for the yt-dlp archival pipeline.
 *
 * WHAT THIS IS NOT: a reimplementation of the pipeline. Downloads are started
 * by handing a command line to the installed ytdl.ps1, exactly as a terminal
 * would. Nothing here knows what run_ytdlp.ps1 or postprocess.ps1 do with it.
 *
 * WHAT THIS DOES OWN: reading the archive. That is a FIFTH independent
 * implementation of docs/archive-layout.md, and the price of standalone apps
 * sharing no engine. ArchiveConformanceTests.cs is what keeps it honest.
 *
 * No Rust, no webview, no bundled runtime beyond .NET and the Windows App SDK
 * itself, and no third-party package of any kind. The one external process it
 * depends on is the pipeline's own, plus ffprobe for stream details.
 */

using System;
using Microsoft.UI.Xaml;

namespace YtdlWin;

public partial class App : Application
{
    public static MainWindow? Window { get; private set; }

    public App() => InitializeComponent();

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        /* An unhandled exception in a WinUI 3 app terminates the process with
         * no dialog and nothing in the event log that names the fault. That is
         * a poor way to learn about the first crash in code that has never been
         * compiled, so the handler writes what happened where a person can find
         * it before letting the process go.
         *
         * It deliberately does NOT set e.Handled = true. Swallowing an
         * unhandled exception leaves the app running in whatever state produced
         * it, which for a window driving a download process is worse than
         * stopping. */
        UnhandledException += (_, e) =>
        {
            try
            {
                /* The build stamp is here because its absence already cost
                 * a diagnosis: a stack trace with no way to tell which binary
                 * produced it means "is this entry stale?" has to be answered
                 * by comparing line numbers against git history. */
                var built = "unknown";
                try
                {
                    var dll = System.IO.Path.Combine(AppContext.BaseDirectory, "YtdlWin.dll");
                    if (System.IO.File.Exists(dll))
                        built = System.IO.File.GetLastWriteTime(dll).ToString("O");
                }
                catch (Exception) { /* the stamp is a nicety, the trace is not */ }

                var path = Core.Paths.Join(Core.Paths.StateDir(), "crash.log");
                var text = $"{DateTimeOffset.Now:O}  build {built}\n{e.Exception}\n\n";
                System.IO.Directory.CreateDirectory(Core.Paths.StateDir());
                System.IO.File.AppendAllText(path, text);
            }
            catch (Exception) { /* nothing useful to do if even this fails */ }
        };

        Window = new MainWindow();
        Window.Activate();
    }
}
