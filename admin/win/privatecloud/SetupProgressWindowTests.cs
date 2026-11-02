// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows.Forms;

namespace PrivateCloud.Setup
{
    internal static class SetupProgressWindowTests
    {
        static int passed;
        static void Assert(bool value, string message) { if (!value) throw new Exception(message); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }

        static void Exercise(int result, bool throwWork, bool throwCompletion)
        {
            int uiThread = Thread.CurrentThread.ManagedThreadId;
            bool shown = false, painted = false, observedBeforeWork = false, onStaWorker = false, activeChecked = false;
            Exception failure = null;
            string log = Path.Combine(Path.GetTempPath(), "privateCloud-progress-fixture.log");
            using (ManualResetEvent ready = new ManualResetEvent(false))
            using (ManualResetEvent release = new ManualResetEvent(false))
            using (SetupProgressWindow window = new SetupProgressWindow(delegate(Action<string> report)
            {
                observedBeforeWork = shown && painted;
                onStaWorker = Thread.CurrentThread.ManagedThreadId != uiThread && Thread.CurrentThread.GetApartmentState() == ApartmentState.STA;
                report("Verifying the installation package…");
                ready.Set();
                if (!release.WaitOne(3000)) throw new TimeoutException("Fixture worker was not released.");
                if (throwWork) throw new IOException("Fixture failure " + new String('x', 20000));
                return result;
            }, delegate(int code)
            {
                if (throwCompletion) throw new IOException("Fixture completion formatting failure");
                return code == 0 ? "privateCloud is installed. You can close this window." : "Fixture installation failed. Read the log path below.";
            }, log))
            using (System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 20 })
            {
                window.Shown += delegate { shown = true; };
                window.Paint += delegate { painted = true; };
                Stopwatch deadline = Stopwatch.StartNew();
                timer.Tick += delegate
                {
                    try
                    {
                        if (deadline.ElapsedMilliseconds > 6000) throw new TimeoutException("Fixture window did not finish.");
                        if (!activeChecked && ready.WaitOne(0) && window.StatusText == "Verifying the installation package…")
                        {
                            Assert(observedBeforeWork, "Work began before the setup window was shown and painted");
                            Assert(onStaWorker, "Work did not run on a separate STA thread");
                            Assert(!window.CloseEnabled && !window.WorkComplete, "Active work could be closed");
                            window.Close();
                            Assert(window.Visible && !window.IsDisposed, "Closing interrupted active work");
                            Assert(window.LogPathText == log, "Log path is missing");
                            activeChecked = true;
                            release.Set();
                        }
                        if (!window.WorkComplete) return;
                        Assert(activeChecked, "No responsive progress state was observed");
                        int expected = throwWork ? 1 : result;
                        Assert(window.ResultCode == expected, "Actual installer result was replaced");
                        Assert(window.CloseEnabled, "Completed setup cannot close");
                        Assert(window.StatusText == (expected == 0 ? "Setup complete" : "Setup could not finish"), "Wrong outcome heading");
                        if (throwWork)
                        {
                            Assert(window.DetailText.Contains("Fixture failure"), "Worker exception was hidden");
                            Assert(window.DetailText.Length < 16500 && window.DetailText.Contains("truncated"), "Exception display is unbounded");
                        }
                        else if (throwCompletion) Assert(window.DetailText.Contains("result " + expected), "Completion fallback lost result");
                        else Assert(window.DetailText.Contains(expected == 0 ? "installed" : "failed"), "Completion detail is missing");
                        timer.Stop();
                        window.Close();
                    }
                    catch (Exception problem)
                    {
                        failure = problem;
                        release.Set();
                        timer.Stop();
                        // This fixture only runs an in-memory wait; no installation is active.
                        window.Dispose();
                        Application.ExitThread();
                    }
                };
                timer.Start();
                Application.Run(window);
                Assert(activeChecked, "Progress inspection did not run");
                if (failure != null) throw failure;
                Assert(window.WorkComplete, "Window returned before the worker completed");
            }
        }

        static void RunReturnsResult()
        {
            Exception failure = null;
            Stopwatch deadline = Stopwatch.StartNew();
            using (System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer { Interval = 20 })
            {
                timer.Tick += delegate
                {
                    SetupProgressWindow window = null;
                    foreach (Form form in Application.OpenForms) if (form is SetupProgressWindow) { window = (SetupProgressWindow)form; break; }
                    if (window == null) return;
                    if (deadline.ElapsedMilliseconds > 5000)
                    { failure = new TimeoutException("Run did not complete"); timer.Stop(); window.Dispose(); Application.ExitThread(); }
                    else if (window.WorkComplete)
                    { timer.Stop(); window.Close(); }
                };
                timer.Start();
                int result = SetupProgressWindow.Run(delegate(Action<string> report) { report("Finishing the fixture…"); return 7; },
                    delegate(int code) { return "Fixture result: " + code; }, "fixture-log.txt");
                if (failure != null) throw failure;
                Assert(result == 7, "Run did not return the actual result after Close");
            }
        }

        [STAThread]
        static int Main(string[] args)
        {
            Control.CheckForIllegalCrossThreadCalls = true;
            Application.EnableVisualStyles();
            if (args.Length > 0 && args[0] == "--preview")
            {
                bool fail = args.Length > 1 && args[1] == "failure";
                return SetupProgressWindow.Run(delegate(Action<string> report)
                {
                    Thread.Sleep(350); report("Verifying the installation package…");
                    Thread.Sleep(650); report("Finishing setup…");
                    Thread.Sleep(350); return fail ? 1 : 0;
                }, delegate(int result)
                {
                    return result == 0 ? "privateCloud setup finished successfully.\r\n\r\nPreview only: no installation was performed."
                        : "Setup could not finish because access to the installation location was denied.\r\nRead the setup log for details.\r\n\r\nPreview only: no installation was performed.";
                }, Path.Combine(Path.GetTempPath(), "privateCloud-preview.log"));
            }
            try
            {
                Test("visible progress precedes separate STA work and blocks close until success", delegate { Exercise(0, false, false); });
                Test("nonzero work result shows failure and permits close", delegate { Exercise(12, false, false); });
                Test("worker exception displays bounded detail and returns failure", delegate { Exercise(0, true, false); });
                Test("completion formatting failure preserves the actual work result", delegate { Exercise(0, false, true); });
                Test("Run returns the worker result after the completed window closes", RunReturnsResult);
                Test("Run rejects a non-STA caller before opening a window", delegate
                {
                    bool refused = false;
                    Thread worker = new Thread(delegate()
                    {
                        try { SetupProgressWindow.Run(delegate { return 0; }, delegate { return "done"; }, "fixture"); }
                        catch (InvalidOperationException) { refused = true; }
                    });
                    worker.SetApartmentState(ApartmentState.MTA); worker.Start();
                    Assert(worker.Join(3000) && refused, "Non-STA caller was not rejected");
                });
                Console.WriteLine(passed + " setup progress tests passed; no installation or configuration changes ran.");
                return 0;
            }
            catch (Exception failure) { Console.Error.WriteLine(failure); return 1; }
        }
    }
}
