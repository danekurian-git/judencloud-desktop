// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace PrivateCloud.Setup
{
    internal static class MachineInstallerTests
    {
        static int passed;
        static string sandbox;
        static readonly byte[] one = Encoding.UTF8.GetBytes("application one");
        static readonly byte[] two = Encoding.UTF8.GetBytes("application two");
        static readonly byte[] broker = Encoding.UTF8.GetBytes("broker fixture");
        static void Assert(bool result, string message) { if (!result) throw new Exception(message); }
        static Exception Reject(Action action)
        {
            try { action(); } catch (Exception failure) { return failure; }
            throw new Exception("Expected rejection");
        }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }
        static string Root(string name) { return Path.Combine(sandbox, name); }
        static string Hash(byte[] bytes)
        {
            using (SHA256 sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant();
        }
        static Package Payload(byte[] app)
        {
            return new Package(Hash(app) + "\tprivateCloud.exe\n" + Hash(broker) + "\tprivateCloudDirectBroker.exe\n");
        }
        static MemoryStream Archive(byte[] app)
        {
            MemoryStream result = new MemoryStream();
            using (ZipArchive archive = new ZipArchive(result, ZipArchiveMode.Create, true))
            {
                using (Stream stream = archive.CreateEntry("privateCloud.exe").Open()) stream.Write(app, 0, app.Length);
                using (Stream stream = archive.CreateEntry("privateCloudDirectBroker.exe").Open()) stream.Write(broker, 0, broker.Length);
            }
            result.Position = 0;
            return result;
        }
        static void VerifyFixtureDirectory(string directory)
        {
            // An injected seam, not a claim that the real Windows DACL is tested here.
            Assert(Directory.Exists(directory), "Expected protected directory");
            InstallState.Child(directory, "protection-check");
        }
        static void Install(string root, string version, byte[] data, Action stop = null, Action activate = null,
            Action stopFailed = null, Action restore = null, Action<string> create = null, Action<string> verify = null)
        {
            using (MemoryStream archive = Archive(data))
                MachineInstallerCore.Install(MachineInstallPaths.ForTestRoot(root), version, Payload(data), archive,
                    create ?? delegate(string path) { Directory.CreateDirectory(path); }, verify ?? VerifyFixtureDirectory,
                    stop ?? delegate { }, activate ?? delegate { }, stopFailed ?? delegate { }, restore ?? delegate { }, delegate { });
        }
        static void AssertVersion(string root, string version)
        {
            MachineInstallPaths paths = MachineInstallPaths.ForTestRoot(root);
            Assert(MachineInstallState.Read(paths, root).Version == version, "Unexpected installed version");
        }

        [STAThread]
        static int Main()
        {
            sandbox = Path.Combine(Path.GetTempPath(), "privateCloud-machine-installer-tests-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(sandbox);
            try
            {
                Test("production path is the stable Program Files root", delegate {
                    Assert(MachineInstallPaths.ForMachine().Root == Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "privateCloud"), "Wrong production root");
                });
                Test("fresh install protects staging before writing either executable", delegate {
                    string root = Root("fresh");
                    bool protectedEmpty = false, stopped = false;
                    Install(root, "1", one, delegate { stopped = true; }, delegate {
                        Assert(stopped && protectedEmpty, "Activation order");
                        Assert(File.Exists(Path.Combine(root, "privateCloudDirectBroker.exe")), "Missing helper at stable root");
                    }, null, null, null, delegate(string directory) {
                        VerifyFixtureDirectory(directory);
                        if (!protectedEmpty) { Assert(Directory.GetFileSystemEntries(directory).Length == 0, "Payload preceded protection"); protectedEmpty = true; }
                    });
                    AssertVersion(root, "1");
                    Assert(File.ReadAllText(Path.Combine(root, "privateCloud.exe")) == Encoding.UTF8.GetString(one), "Wrong GUI payload");
                    Assert(!Directory.Exists(Path.Combine(root, "versions")), "GUI must not be a version-directory launcher");
                });
                Test("protection failure prevents extraction and stopping the old app", delegate {
                    string root = Root("unprotected");
                    string staged = null;
                    bool stopped = false;
                    Reject(delegate { Install(root, "1", one, delegate { stopped = true; }, null, null, null,
                        delegate(string directory) { staged = directory; Directory.CreateDirectory(directory); },
                        delegate { throw new IOException("injected DACL rejection"); }); });
                    Assert(!stopped && !Directory.Exists(root), "Unprotected payload activated");
                    Assert(staged != null && Directory.GetFiles(staged, "*.exe", SearchOption.AllDirectories).Length == 0, "Unprotected executables were extracted");
                });
                Test("foreign files in a staging destination are retained", delegate {
                    string staged = null;
                    Reject(delegate { Install(Root("foreign-stage"), "1", one, null, null, null, null, delegate(string directory) {
                        staged = directory; Directory.CreateDirectory(directory); File.WriteAllText(Path.Combine(directory, "privateCloud.exe"), "foreign");
                    }); });
                    Assert(File.ReadAllText(Path.Combine(staged, "privateCloud.exe")) == "foreign", "Rejected staging content deleted");
                });
                Test("upgrade preserves the verified old root and unknown program-directory files", delegate {
                    string root = Root("upgrade"); Install(root, "1", one);
                    File.WriteAllText(Path.Combine(root, "note.txt"), "keep");
                    Install(root, "2", two);
                    MachineInstallPaths paths = MachineInstallPaths.ForTestRoot(root);
                    string previous = MachineInstallerCore.OwnedPrevious(paths, VerifyFixtureDirectory);
                    AssertVersion(root, "2"); AssertVersion(previous, "1");
                    Assert(File.ReadAllText(Path.Combine(previous, "note.txt")) == "keep", "Previous extra file changed");
                });
                Test("same version repairs integration without creating another recovery copy", delegate {
                    string root = Root("same"); Install(root, "1", one); Install(root, "2", two);
                    int directories = Directory.GetDirectories(sandbox).Length, activated = 0;
                    string previous = MachineInstallState.Read(MachineInstallPaths.ForTestRoot(root), root).Previous;
                    Install(root, "2", two, null, delegate { ++activated; });
                    Assert(activated == 1 && Directory.GetDirectories(sandbox).Length == directories, "Idempotent repair added a copy");
                    Assert(MachineInstallState.Read(MachineInstallPaths.ForTestRoot(root), root).Previous == previous, "Lost rollback reference");
                    Reject(delegate { Install(root, "2", one); });
                });
                Test("bad new hashes leave current installation running and unchanged", delegate {
                    string root = Root("bad-hash"); Install(root, "1", one); bool stopped = false;
                    using (MemoryStream archive = Archive(one))
                        Reject(delegate { MachineInstallerCore.Install(MachineInstallPaths.ForTestRoot(root), "2", Payload(two), archive,
                            delegate(string path) { Directory.CreateDirectory(path); }, VerifyFixtureDirectory,
                            delegate { stopped = true; }, delegate { }, delegate { }, delegate { }, delegate { }); });
                    Assert(!stopped, "Stopped before verifying payload"); AssertVersion(root, "1");
                    Assert(Directory.GetDirectories(sandbox, "bad-hash.staging-*").Length == 0, "Failed owned stage retained");
                });
                Test("stop failure restores integration without moving program files", delegate {
                    string root = Root("stop-fails"); Install(root, "1", one); bool restored = false;
                    Reject(delegate { Install(root, "2", two, delegate { throw new IOException("stop failed"); }, null, null,
                        delegate { AssertVersion(root, "1"); restored = true; }); });
                    Assert(restored, "Old integration not restored"); AssertVersion(root, "1");
                });
                Test("activation failure stops new service then restores paths before old service", delegate {
                    string root = Root("activate-fails"); Install(root, "1", one); string events = "";
                    Reject(delegate { Install(root, "2", two, delegate { events += "stop;"; }, delegate {
                        AssertVersion(root, "2"); events += "activate;"; throw new IOException("start failed");
                    }, delegate { AssertVersion(root, "2"); events += "stop-new;"; }, delegate {
                        AssertVersion(root, "1"); events += "restore;";
                    }); });
                    Assert(events == "stop;activate;stop-new;restore;", "Wrong recovery order: " + events);
                    Assert(Directory.GetDirectories(sandbox, "activate-fails.staging-*").Length == 0, "Failed new payload retained");
                });
                Test("fresh activation failure removes new payload before restoring registration", delegate {
                    string root = Root("fresh-activation-fails"); bool restored = false;
                    Reject(delegate { Install(root, "1", one, null, delegate { throw new IOException("activate"); }, null,
                        delegate { Assert(!Directory.Exists(root), "New root still active"); restored = true; }); });
                    Assert(restored && !Directory.Exists(root), "Fresh failure not restored");
                });
                Test("failed new-service stop retains both program copies and skips rollback", delegate {
                    string root = Root("new-stop-fails"); Install(root, "1", one); bool restored = false;
                    Exception failure = Reject(delegate { Install(root, "2", two, null, delegate { throw new IOException("activate"); },
                        delegate { throw new IOException("cannot stop new service"); }, delegate { restored = true; }); });
                    Assert(failure is AggregateException && !restored, "Unsafe recovery continued");
                    AssertVersion(root, "2");
                    string previous = MachineInstallerCore.OwnedPrevious(MachineInstallPaths.ForTestRoot(root), VerifyFixtureDirectory);
                    AssertVersion(previous, "1");
                });
                Test("directory rollback failure preserves copies and never starts old service", delegate {
                    string root = Root("restore-path-fails"); Install(root, "1", one); bool restored = false;
                    MachineInstallPaths paths = MachineInstallPaths.ForTestRoot(root);
                    Exception failure = Reject(delegate { Install(root, "2", two, null, delegate { throw new IOException("activate"); }, delegate {
                        string previous = MachineInstallState.Read(paths, root).Previous;
                        string staged = paths.ResolveSibling(previous.Replace(".previous-", ".staging-"), "staging");
                        Directory.CreateDirectory(staged); File.WriteAllText(Path.Combine(staged, "keep.txt"), "keep");
                    }, delegate { restored = true; }); });
                    Assert(failure is AggregateException && !restored, "Started service before paths restored");
                    AssertVersion(root, "2"); AssertVersion(MachineInstallerCore.OwnedPrevious(paths, VerifyFixtureDirectory), "1");
                });
                Test("integration rollback failure still restores old files and reports all failures", delegate {
                    string root = Root("restore-integration-fails"); Install(root, "1", one);
                    Exception failure = Reject(delegate { Install(root, "2", two, null, delegate { throw new IOException("activate"); }, null,
                        delegate { AssertVersion(root, "1"); throw new IOException("restore registration"); }); });
                    Assert(failure is AggregateException && ((AggregateException)failure).InnerExceptions.Count == 2, "Recovery failure hidden");
                    AssertVersion(root, "1");
                    Assert(Directory.GetDirectories(sandbox, "restore-integration-fails.staging-*").Length == 0, "Safe stage cleanup was skipped");
                });
                Test("foreign empty and per-user installation roots are rejected", delegate {
                    string foreign = Root("foreign"); Directory.CreateDirectory(foreign);
                    Reject(delegate { Install(foreign, "1", one); });
                    File.WriteAllText(Path.Combine(foreign, "installation.txt"), InstallState.Marker);
                    Reject(delegate { Install(foreign, "1", one); });
                    Assert(File.ReadAllText(Path.Combine(foreign, "installation.txt")) == InstallState.Marker, "Foreign marker changed");
                });
                Test("junction destination is rejected and target files are untouched", delegate {
                    string target = Root("junction-target"), link = Root("junction-root"); Directory.CreateDirectory(target);
                    File.WriteAllText(Path.Combine(target, "keep.txt"), "keep");
                    ProcessStartInfo start = new ProcessStartInfo("cmd.exe", "/d /c mklink /J \"" + link + "\" \"" + target + "\"") {
                        UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
                    };
                    using (Process process = Process.Start(start))
                    {
                        Assert(process.WaitForExit(5000) && process.ExitCode == 0, "Could not create isolated junction fixture");
                    }
                    Reject(delegate { Install(link, "1", one); });
                    Assert(File.ReadAllText(Path.Combine(target, "keep.txt")) == "keep", "Junction target changed");
                });
                Test("machine payload requires helper and forbids installer metadata", delegate {
                    Reject(delegate { MachineInstallState.CheckPackage(new Package(Hash(one) + "\tprivateCloud.exe\n")); });
                    Reject(delegate { MachineInstallState.CheckPackage(new Package(Payload(one).Manifest + Hash(one) + "\tMachine-Installation.txt\n")); });
                });
                Test("previous record cannot select an arbitrary directory", delegate {
                    string root = Root("bad-previous"); Install(root, "1", one);
                    File.WriteAllText(Path.Combine(root, MachineInstallState.RecordName), MachineInstallState.Marker + "\n1\n../outside\n");
                    Reject(delegate { MachineInstallerCore.OwnedPrevious(MachineInstallPaths.ForTestRoot(root), VerifyFixtureDirectory); });
                });
                Test("uninstall removes only verified manifest files and retains previous and unrelated files", delegate {
                    string root = Root("uninstall"); Install(root, "1", one); Install(root, "2", two);
                    MachineInstallPaths paths = MachineInstallPaths.ForTestRoot(root);
                    string previous = MachineInstallerCore.OwnedPrevious(paths, VerifyFixtureDirectory);
                    string profile = Root("profile-fixture.txt"); File.WriteAllText(profile, "profile");
                    File.WriteAllText(Path.Combine(root, "unknown.txt"), "keep"); bool removed = false;
                    MachineInstallerCore.Uninstall(paths, VerifyFixtureDirectory, delegate {
                        Assert(File.Exists(Path.Combine(root, "privateCloud.exe")), "Program removed before integration stopped"); removed = true;
                    });
                    Assert(removed && !File.Exists(Path.Combine(root, "privateCloud.exe")), "Owned files retained");
                    Assert(File.ReadAllText(Path.Combine(root, "unknown.txt")) == "keep" && File.ReadAllText(profile) == "profile", "Unrelated data changed");
                    AssertVersion(previous, "1");
                });
                Test("interrupted uninstall can remove remaining files", delegate {
                    string root = Root("partial-uninstall"); Install(root, "1", one);
                    File.Delete(Path.Combine(root, "privateCloud.exe"));
                    MachineInstallerCore.Uninstall(MachineInstallPaths.ForTestRoot(root), VerifyFixtureDirectory, delegate { });
                    Assert(!Directory.Exists(root), "Partial uninstall was not recoverable");
                });
                Test("uninstall rejects changed program files before removing integration", delegate {
                    string root = Root("changed-uninstall"); Install(root, "1", one); bool removed = false;
                    File.WriteAllText(Path.Combine(root, "privateCloud.exe"), "changed");
                    Reject(delegate { MachineInstallerCore.Uninstall(MachineInstallPaths.ForTestRoot(root), VerifyFixtureDirectory, delegate { removed = true; }); });
                    Assert(!removed && File.ReadAllText(Path.Combine(root, "privateCloud.exe")) == "changed", "Changed files removed");
                });
                Test("uninstall integration failure leaves program files intact", delegate {
                    string root = Root("uninstall-stop-fails"); Install(root, "1", one);
                    Reject(delegate { MachineInstallerCore.Uninstall(MachineInstallPaths.ForTestRoot(root), VerifyFixtureDirectory,
                        delegate { throw new IOException("stop and unregister failed"); }); });
                    AssertVersion(root, "1"); Payload(one).Verify(root);
                });
                Console.WriteLine(passed + " machine installer tests passed. Isolated artifacts: " + sandbox);
                return 0;
            }
            catch (Exception failure) { Console.Error.WriteLine(failure); Console.Error.WriteLine("Artifacts: " + sandbox); return 1; }
        }
    }
}
