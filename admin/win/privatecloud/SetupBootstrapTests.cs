// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;

namespace PrivateCloud.Setup
{
    internal static class SetupBootstrapTests
    {
        static int passed;
        static void Assert(bool result, string message) { if (!result) throw new Exception(message); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }
        static string ReadActiveLog(string path)
        {
            using (StreamReader reader = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)))
                return reader.ReadToEnd();
        }
        static int Main()
        {
            string sandbox = Path.Combine(Path.GetTempPath(), "privatecloud-bootstrap-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(sandbox);
            try
            {
                Test("setup attempts have independent immediately readable logs", delegate {
                    using (SetupLog first = SetupLog.CreateInDirectory(sandbox))
                    using (SetupLog second = SetupLog.CreateInDirectory(sandbox))
                    {
                        first.Write("first attempt"); second.Write("second attempt");
                        Assert(first.FilePath != second.FilePath, "Log was reused.");
                        Assert(ReadActiveLog(first.FilePath).Contains("first attempt"), "First log was not flushed.");
                        Assert(!ReadActiveLog(first.FilePath).Contains("second attempt"), "Logs were mixed.");
                    }
                });
                Test("initial path failure is caught and returned to the visible result", delegate {
                    using (SetupLog log = SetupLog.CreateInDirectory(sandbox))
                    {
                        string result, status = "";
                        int code = MachineSetupProgram.RunSetup(new string[0], false, log, delegate(string text) { status = text; }, out result,
                            delegate { throw new IOException("Simulated startup path failure"); });
                        Assert(code == 1 && result == "Simulated startup path failure" && status.Contains(result), "Startup failure escaped feedback.");
                        Assert(ReadActiveLog(log.FilePath).Contains("Simulated startup path failure"), "Failure was not logged.");
                    }
                });
                Test("unavailable diagnostic storage does not replace startup failure", delegate {
                    using (SetupLog log = SetupLog.TryCreate(delegate { throw new UnauthorizedAccessException("Log directory denied"); }))
                    {
                        string result;
                        int code = MachineSetupProgram.RunSetup(new string[0], false, log, delegate { }, out result,
                            delegate { throw new IOException("Original startup failure"); });
                        Assert(log.FilePath == null && code == 1 && result == "Original startup failure", "Logging hid the original error.");
                    }
                });
                Test("unelevated internal worker refuses before installation", delegate {
                    using (SetupLog log = SetupLog.CreateInDirectory(sandbox))
                    {
                        string result;
                        int code = MachineSetupProgram.RunSetup(new[] { "--elevated-install", "--silent" }, false, log, delegate { }, out result,
                            delegate { return MachineInstallPaths.ForTestRoot(Path.Combine(sandbox, "never-created")); });
                        Assert(code == 1 && result == "Administrator setup is required." && !Directory.Exists(Path.Combine(sandbox, "never-created")), "Unprivileged worker changed the installation.");
                    }
                });
                Test("package verification errors remain non-mutating and logged", delegate {
                    using (SetupLog log = SetupLog.CreateInDirectory(sandbox))
                    {
                        string result;
                        int code = MachineSetupProgram.RunSetup(new[] { "--verify-package", sandbox }, false, log, delegate { }, out result,
                            delegate { return MachineInstallPaths.ForTestRoot(Path.Combine(sandbox, "never-created")); });
                        Assert(code == 1 && result.Contains("already exists") && !Directory.Exists(Path.Combine(sandbox, "never-created")), "Verification entered installation.");
                    }
                });
                Test("malformed verification commands never fall through to installation", delegate {
                    foreach (string[] args in new[] { new[] { "--verify-package" }, new[] { "--verify-package", sandbox, "--silent" },
                        new[] { sandbox, "--verify-package" }, new[] { "--elevated-install", "--verify-package", sandbox } })
                        using (SetupLog log = SetupLog.CreateInDirectory(sandbox))
                        {
                            string result;
                            int code = MachineSetupProgram.RunSetup(args, false, log, delegate { }, out result,
                                delegate { return MachineInstallPaths.ForTestRoot(Path.Combine(sandbox, "never-created")); });
                            Assert(code == 1 && result.StartsWith("Package verification requires exactly:") &&
                                !Directory.Exists(Path.Combine(sandbox, "never-created")), "Malformed verification escaped the diagnostic path.");
                        }
                });
                Console.WriteLine(passed + " bootstrap checks passed.");
                return 0;
            }
            catch (Exception failure) { Console.Error.WriteLine(failure); return 1; }
            finally
            {
                // The freshly generated sandbox contains only this test's log files.
                foreach (string path in Directory.GetFiles(sandbox, "*.log")) File.Delete(path);
                Directory.Delete(sandbox);
            }
        }
    }
}
