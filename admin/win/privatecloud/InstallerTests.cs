// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace PrivateCloud.Setup
{
    internal static class InstallerTests
    {
        static int passed;
        static string sandbox;
        static string Hash(byte[] data) { using (SHA256 sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "").ToLowerInvariant(); }
        static void Assert(bool result, string message) { if (!result) throw new Exception(message); }
        static void Reject(Action action) { bool rejected = false; try { action(); } catch { rejected = true; } Assert(rejected, "Expected failure"); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }
        static MemoryStream Archive(string path, byte[] data)
        {
            MemoryStream bytes = new MemoryStream();
            using (ZipArchive zip = new ZipArchive(bytes, ZipArchiveMode.Create, true))
                using (Stream entry = zip.CreateEntry(path).Open()) entry.Write(data, 0, data.Length);
            bytes.Position = 0;
            return bytes;
        }
        static void Install(string root, string version, byte[] data, Action integration, Action rollback)
        {
            Package package = new Package(Hash(data) + "\tprivateCloud.exe\n");
            using (MemoryStream archive = Archive("privateCloud.exe", data))
                InstallerCore.Install(root, version, package, archive, Encoding.UTF8.GetBytes("launcher"), integration, rollback, delegate { });
        }
        [STAThread]
        static int Main(string[] args)
        {
            sandbox = Path.Combine(Path.GetTempPath(), "privateCloud-installer-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(sandbox);
            try
            {
                byte[] one = Encoding.UTF8.GetBytes("first application"), two = Encoding.UTF8.GetBytes("second application");
                string root = Path.Combine(sandbox, "installed");
                Test("fresh install verifies and activates", delegate {
                    Install(root, "1", one, delegate { }, delegate { });
                    Assert(InstallState.Read(root).Current == "1", "Version not active");
                    Assert(File.ReadAllBytes(Path.Combine(root, "versions/1/privateCloud.exe")).Length == one.Length, "Wrong payload");
                });
                Test("fresh verification failure can be retried successfully", delegate {
                    string fresh = Path.Combine(sandbox, "fresh-retry");
                    Package package = new Package(Hash(two) + "\tprivateCloud.exe\n");
                    using (MemoryStream archive = Archive("privateCloud.exe", one))
                        Reject(delegate { InstallerCore.Install(fresh, "1", package, archive, one, delegate { }, delegate { }, delegate { }); });
                    Install(fresh, "1", two, delegate { }, delegate { });
                    Assert(InstallState.Read(fresh).Current == "1", "A failed first install blocked retry");
                });
                Test("upgrade retains previous version and unrelated data", delegate {
                    File.WriteAllText(Path.Combine(root, "user-note.txt"), "keep");
                    Install(root, "2", two, delegate { }, delegate { });
                    Assert(InstallState.Read(root).Current == "2" && InstallState.Read(root).Previous == "1", "Missing recovery version");
                    Assert(File.ReadAllText(Path.Combine(root, "user-note.txt")) == "keep", "Unrelated data changed");
                });
                Test("same verified version reinstalls idempotently and retains rollback", delegate {
                    Install(root, "2", two, delegate { }, delegate { });
                    Assert(InstallState.Read(root).Current == "2" && InstallState.Read(root).Previous == "1", "Same-version install discarded rollback");
                });
                Test("rollback verifies and switches atomically", delegate {
                    InstallerCore.Rollback(root);
                    Assert(InstallState.Read(root).Current == "1" && InstallState.Read(root).Previous == "2", "Rollback failed");
                });
                Test("activation failure restores old version and integration", delegate {
                    bool restored = false;
                    Reject(delegate { Install(root, "3", two, delegate { throw new IOException("injected registration failure"); }, delegate { restored = true; }); });
                    Assert(restored && InstallState.Read(root).Current == "1", "Failed upgrade activated");
                    Assert(!Directory.Exists(Path.Combine(root, "versions/3")), "Failed version retained");
                });
                Test("integration rollback failure still restores launcher and cleans failed version", delegate {
                    File.WriteAllText(Path.Combine(root, "privateCloud.exe"), "old launcher");
                    Package package = new Package(Hash(two) + "\tprivateCloud.exe\n");
                    using (MemoryStream archive = Archive("privateCloud.exe", two))
                        Reject(delegate { InstallerCore.Install(root, "3", package, archive, two, delegate { throw new IOException("activation failed"); }, delegate { throw new IOException("rollback failed"); }, delegate { }); });
                    Assert(File.ReadAllText(Path.Combine(root, "privateCloud.exe")) == "old launcher", "Launcher recovery skipped");
                    Assert(InstallState.Read(root).Current == "1" && !Directory.Exists(Path.Combine(root, "versions/3")), "Other recovery steps skipped");
                });
                Test("corrupt payload leaves active version untouched", delegate {
                    Package corrupt = new Package(Hash(two) + "\tprivateCloud.exe\n");
                    using (MemoryStream archive = Archive("privateCloud.exe", one))
                        Reject(delegate { InstallerCore.Install(root, "4", corrupt, archive, one, delegate { }, delegate { }, delegate { }); });
                    Assert(InstallState.Read(root).Current == "1", "Corrupt payload activated");
                });
                Test("zip traversal rejected", delegate {
                    Package package = new Package(Hash(one) + "\tprivateCloud.exe\n");
                    using (MemoryStream archive = Archive("../escaped.exe", one)) Reject(delegate { package.Extract(archive, Path.Combine(sandbox, "traversal")); });
                    Assert(!File.Exists(Path.Combine(sandbox, "escaped.exe")), "Path traversal wrote outside destination");
                });
                Test("unowned directory rejected", delegate {
                    string unowned = Path.Combine(sandbox, "unowned"); Directory.CreateDirectory(unowned); File.WriteAllText(Path.Combine(unowned, "important.txt"), "keep");
                    Reject(delegate { Install(unowned, "1", one, delegate { }, delegate { }); });
                    Assert(File.ReadAllText(Path.Combine(unowned, "important.txt")) == "keep", "User file modified");
                });
                Test("tampered rollback version rejected", delegate {
                    File.WriteAllText(Path.Combine(root, "versions/2/privateCloud.exe"), "tampered");
                    Reject(delegate { InstallerCore.Rollback(root); });
                    Assert(InstallState.Read(root).Current == "1", "Tampered version activated");
                });
                Test("missing archive entry rejected", delegate {
                    Package package = new Package(Hash(one) + "\tprivateCloud.exe\n" + Hash(two) + "\tneeded.dll\n");
                    using (MemoryStream archive = Archive("privateCloud.exe", one)) Reject(delegate { package.Extract(archive, Path.Combine(sandbox, "incomplete")); });
                });
                Test("invalid installed version rejected", delegate { Reject(delegate { InstallState.VersionPath(root, "../outside"); }); });
                Test("duplicate and absolute manifest paths rejected", delegate {
                    Reject(delegate { new Package(Hash(one) + "\tprivateCloud.exe\n" + Hash(one) + "\tprivateCloud.exe\n"); });
                    Reject(delegate { new Package(Hash(one) + "\tC:/escaped.exe\n"); });
                });
                Test("damaged active-version record rejected", delegate {
                    string damaged = Path.Combine(sandbox, "damaged"); Directory.CreateDirectory(damaged);
                    File.WriteAllText(Path.Combine(damaged, "active-version.txt"), "../elsewhere\n\n");
                    Reject(delegate { InstallState.Read(damaged); });
                });
                Test("argument quoting preserves trailing slashes", delegate { Assert(WindowsIntegration.Quote("C:\\cloud\\") == "\"C:\\cloud\\\\\"", "Trailing slash quoting"); });
                Test("installed launcher sets managed startup marker only for its child", delegate {
                    string before = Environment.GetEnvironmentVariable("PRIVATECLOUD_MANAGED_STARTUP");
                    System.Diagnostics.ProcessStartInfo start = WindowsIntegration.ApplicationStart(Path.Combine(root, "versions/1"), new[] { "--background" });
                    Assert(start.EnvironmentVariables["PRIVATECLOUD_MANAGED_STARTUP"] == "1", "Missing managed startup marker");
                    Assert(start.Arguments == "\"--background\"" && !start.UseShellExecute, "Incorrect child launch settings");
                    Assert(Environment.GetEnvironmentVariable("PRIVATECLOUD_MANAGED_STARTUP") == before, "Parent environment changed");
                });
                Test("maintenance lock excludes another thread and releases", delegate {
                    bool rejected = false;
                    using (InstallLock gate = new InstallLock())
                    {
                        Thread contender = new Thread(delegate() { try { using (InstallLock other = new InstallLock()) { } } catch (IOException) { rejected = true; } });
                        contender.Start(); contender.Join();
                    }
                    Assert(rejected, "Concurrent maintenance was allowed");
                    using (InstallLock after = new InstallLock()) { }
                });
                Test("real Windows shortcut creates and atomically updates in temporary directory", delegate {
                    string link = Path.Combine(sandbox, "privateCloud.lnk");
                    string first = Path.Combine(root, "versions/1/privateCloud.exe");
                    string second = Path.Combine(root, "versions/2/privateCloud.exe");
                    WindowsIntegration.CreateShortcut(link, first);
                    Assert(String.Equals(WindowsIntegration.ReadShortcut(link), Path.GetFullPath(first), StringComparison.OrdinalIgnoreCase), "Wrong shortcut target");
                    WindowsIntegration.CreateShortcut(link, second);
                    Assert(String.Equals(WindowsIntegration.ReadShortcut(link), Path.GetFullPath(second), StringComparison.OrdinalIgnoreCase), "Shortcut update failed");
                });
                Test("real launcher starts isolated installed child with managed startup and arguments", delegate {
                    Assert(args.Length == 2, "Launcher fixture paths are required");
                    string launchRoot = Path.Combine(sandbox, "launcher-installed");
                    byte[] app = File.ReadAllBytes(args[1]);
                    Package package = new Package(Hash(app) + "\tprivateCloud.exe\n");
                    using (MemoryStream archive = Archive("privateCloud.exe", app))
                        InstallerCore.Install(launchRoot, "1", package, archive, File.ReadAllBytes(args[0]), delegate { }, delegate { }, delegate { });
                    string receipt = Path.Combine(sandbox, "launched.txt");
                    using (System.Diagnostics.Process launcher = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Path.Combine(launchRoot, "privateCloud.exe"), "--background --record " + WindowsIntegration.Quote(receipt)) { UseShellExecute = false }))
                        Assert(launcher.WaitForExit(5000) && launcher.ExitCode == 0, "Installed launcher failed");
                    for (int i = 0; i < 50 && !File.Exists(receipt); ++i) Thread.Sleep(100);
                    string[] recorded = File.ReadAllLines(receipt);
                    Assert(recorded[0] == "1" && recorded[1] == "--background", "Child environment or arguments missing");
                    Assert(String.Equals(recorded[2], Path.Combine(launchRoot, "versions", "1"), StringComparison.OrdinalIgnoreCase), "Child working directory mismatch");
                });
                Test("uninstall removes owned application files and preserves other files", delegate {
                    string staging = Path.Combine(root, "versions", "staging-abandoned"); Directory.CreateDirectory(staging);
                    File.WriteAllText(Path.Combine(staging, "uncommitted.txt"), "keep");
                    InstallerCore.UninstallFiles(root);
                    Assert(!File.Exists(Path.Combine(root, "versions/1/privateCloud.exe")), "Application retained");
                    Assert(File.ReadAllText(Path.Combine(root, "user-note.txt")) == "keep", "User file deleted");
                    Assert(File.ReadAllText(Path.Combine(staging, "uncommitted.txt")) == "keep", "Uncommitted staging folder removed");
                });
                Console.WriteLine(passed + " tests passed. Isolated test artifacts: " + sandbox);
                return 0;
            }
            catch (Exception ex) { Console.Error.WriteLine(ex); Console.Error.WriteLine("Artifacts: " + sandbox); return 1; }
        }
    }
}
