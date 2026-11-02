// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;

namespace PrivateCloud.Setup
{
    internal static class SetupPresentationTests
    {
        static int passed;
        static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }
        static void Visible(string[] args, int result)
        {
            bool shown = false;
            bool ran = false;
            string status = null;
            int actual = SetupPresentation.Run(args, delegate(Action<string> report) {
                Assert(shown, "Setup work started without showing an interactive result window.");
                ran = true;
                report("Checking installation");
                return result;
            }, delegate(Func<Action<string>, int> work) {
                shown = true;
                return work(delegate(string text) { status = text; });
            });
            Assert(shown && ran && status == "Checking installation" && actual == result, "Interactive work, status or result was lost.");
        }
        static void Silent(string[] args)
        {
            int ran = 0;
            int result = SetupPresentation.Run(args, delegate(Action<string> report) { ++ran; report("Internal operation"); return 17; },
                delegate { throw new Exception("Internal operation unexpectedly opened a window."); });
            Assert(ran == 1 && result == 17, "Silent operation lost its exit code.");
        }
        static int Main()
        {
            try
            {
                Test("normal and already-elevated interactive installation present before work", delegate { Visible(new string[0], 0); });
                Test("interactive failure remains observable", delegate { Visible(new string[0], 1); });
                Test("uninstall worker presents removal result", delegate { Visible(new[] { "--uninstall-worker" }, 0); });
                Test("no-launch install still presents completion", delegate { Visible(new[] { "--no-launch" }, 0); });
                foreach (string[] args in new[] { new[] { "--silent" }, new[] { "--elevated-install" }, new[] { "--elevated-uninstall" },
                    new[] { "--configure-user" }, new[] { "--verify-package", "isolated-destination" }, new[] { "--uninstall" } })
                    Test("internal mode has no extra UI: " + args[0], delegate { Silent(args); });
                Console.WriteLine(passed + " setup presentation checks passed.");
                return 0;
            }
            catch (Exception failure) { Console.Error.WriteLine(failure); return 1; }
        }
    }
}
