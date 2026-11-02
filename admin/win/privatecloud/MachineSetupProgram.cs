// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Principal;
using System.Windows.Forms;
using Microsoft.Win32;

namespace PrivateCloud.Setup
{
    internal static class MachineSetupProgram
    {
        static Stream Resource(string name) { return Assembly.GetExecutingAssembly().GetManifestResourceStream(name); }
        static string Text(string name) { using (StreamReader reader = new StreamReader(Resource(name))) return reader.ReadToEnd(); }
        static bool Elevated { get { using (WindowsIdentity identity = WindowsIdentity.GetCurrent()) return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator); } }
        static string Self { get { return Assembly.GetExecutingAssembly().Location; } }
        static string App(MachineInstallPaths paths) { return InstallState.Child(paths.Root, "privateCloud.exe"); }

        [STAThread]
        static int Main(string[] args)
        {
            try
            {
                bool elevated = Elevated;
                using (SetupLog log = SetupLog.Create(elevated))
                {
                    log.Write("Setup started: " + Self);
                    log.Write("OS=" + Environment.OSVersion + "; CLR=" + Environment.Version +
                        "; x64=" + Environment.Is64BitProcess + "; elevated=" + elevated);
                    string resultMessage = "";
                    return SetupPresentation.Run(args, delegate(Action<string> update) {
                        Action<string> report = delegate(string status) { log.Write(status); update(status); };
                        int result = RunSetup(args, elevated, log, report, out resultMessage);
                        log.Write("Setup finished with exit code " + result);
                        return result;
                    }, delegate(Func<Action<string>, int> work) {
                        return SetupProgressWindow.Run(work, delegate { return resultMessage; }, log.FilePath);
                    });
                }
            }
            catch (Exception failure)
            {
                if (!args.Contains("--silent"))
                    MessageBox.Show("privateCloud setup could not start.\n\n" + failure, "privateCloud setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
        }

        internal static int RunSetup(string[] args, bool elevatedUser, SetupLog setupLog, Action<string> report, out string resultMessage,
            Func<MachineInstallPaths> createPaths = null)
        {
            bool silent = args.Contains("--silent");
            MachineInstallPaths paths = null;
            resultMessage = "";
            try
            {
                report("Checking this computer...");
                paths = (createPaths ?? MachineInstallPaths.ForMachine)();
                if (args.Contains("--verify-package"))
                {
                    if (args.Length != 2 || args[0] != "--verify-package")
                        throw new IOException("Package verification requires exactly: --verify-package <new destination folder>.");
                    report("Verifying the embedded installation files...");
                    string destination = Path.GetFullPath(args[1]);
                    if (Directory.Exists(destination) || File.Exists(destination)) throw new IOException("The verification destination already exists.");
                    Package verify = new Package(Text("package.sha256"));
                    Directory.CreateDirectory(destination);
                    using (Stream archive = Resource("payload.zip")) verify.Extract(archive, destination);
                    return 0;
                }
                bool childInstall = args.Contains("--elevated-install");
                bool childUninstall = args.Contains("--elevated-uninstall");
                if (childInstall || childUninstall)
                {
                    if (!elevatedUser || childInstall == childUninstall) throw new IOException("Administrator setup is required.");
                    report(childUninstall ? "Removing privateCloud..." : "Verifying and installing privateCloud...");
                    MachineWindowsIntegration.VerifySecurity(Directory.GetAccessControl(paths.Parent));
                    string lockPath = InstallState.Child(paths.Parent, "privateCloud.maintenance.lock");
                    using (FileStream maintenanceLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
                    {
                        if (Directory.Exists(paths.Root)) MachineWindowsIntegration.VerifyProtectedDirectory(paths.Root);
                        using (MachineWindowsIntegration integration = new MachineWindowsIntegration(paths))
                        {
                            if (childUninstall)
                            {
                                EnsureNoInstalledApplication(paths);
                                MachineInstallerCore.Uninstall(paths, MachineWindowsIntegration.VerifyProtectedDirectory, integration.Remove);
                            }
                            else
                            {
                                string version = Text("version.txt").Trim();
                                Package package = new Package(Text("package.sha256"));
                                using (Stream archive = Resource("payload.zip"))
                                    MachineInstallerCore.Install(paths, version, package, archive,
                                        MachineWindowsIntegration.CreateProtectedDirectory, MachineWindowsIntegration.VerifyProtectedDirectory,
                                        delegate { report("Stopping the connection helper for maintenance..."); EnsureNoInstalledApplication(paths); integration.StopForChange(); },
                                        delegate { report("Configuring the privateCloud connection helper..."); integration.Activate(version); },
                                        integration.StopFailedActivation, integration.Restore, report);
                            }
                        }
                    }
                    return 0;
                }
                if (args.Contains("--configure-user"))
                {
                    if (elevatedUser) throw new IOException("Configure privateCloud from your normal Windows account.");
                    MachineWindowsIntegration.VerifyProtectedDirectory(paths.Root);
                    ConfigureUser(paths, false);
                    return 0;
                }
                bool uninstall = args.Contains("--uninstall") || args.Contains("--uninstall-worker");
                if (args.Contains("--uninstall"))
                {
                    CloseUserApplication(paths);
                    // Run maintenance outside the program root so its own image
                    // does not prevent deletion. Only a fixed uninstall mode is forwarded.
                    bool protectedMaintenance = elevatedUser;
                    string temporary = MaintenanceDirectory(paths.Parent, Path.GetTempPath(), protectedMaintenance, Guid.NewGuid());
                    if (protectedMaintenance)
                    {
                        temporary = InstallState.Child(paths.Parent, Path.GetFileName(temporary));
                        MachineWindowsIntegration.CreateProtectedDirectory(temporary);
                    }
                    else Directory.CreateDirectory(temporary);
                    string copy = Path.Combine(temporary, "privateCloudMaintenance.exe");
                    File.Copy(Self, copy, false);
                    if (protectedMaintenance) MachineWindowsIntegration.VerifyProtectedDirectory(temporary);
                    Process.Start(new ProcessStartInfo(copy, "--uninstall-worker" + (silent ? " --silent" : "")) { UseShellExecute = false, WorkingDirectory = temporary });
                    return 0;
                }
                if (!uninstall)
                {
                    report("Preparing privateCloud for installation...");
                    CloseUserApplication(paths);
                }
                string mode = uninstall ? "--elevated-uninstall" : "--elevated-install";
                report("Waiting for Windows administrator approval...");
                using (Process elevated = Process.Start(new ProcessStartInfo(Self, mode + " --silent")
                    { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = Path.GetDirectoryName(Self) }))
                {
                    report(uninstall ? "Removing privateCloud. Please wait..." : "Installing privateCloud and its connection helper. Please wait...");
                    setupLog.Write("Administrator worker PID=" + elevated.Id);
                    if (!elevated.WaitForExit(300000)) throw new IOException("Setup is still running. Wait for it to finish before trying again.");
                    setupLog.Write("Administrator worker exit code=" + elevated.ExitCode);
                    if (elevated.ExitCode != 0) throw new IOException("privateCloud setup failed (worker exit code " + elevated.ExitCode + "). Details: " + ErrorPath(paths));
                }
                if (!elevatedUser && !args.Contains("--no-register-user"))
                {
                    report("Updating your Windows startup entry...");
                    ConfigureUser(paths, uninstall);
                }
                if (!uninstall && !args.Contains("--no-launch") && !elevatedUser)
                {
                    report("Opening privateCloud...");
                    using (Process application = Process.Start(WindowsIntegration.ApplicationStart(paths.Root, new string[0])))
                    {
                        setupLog.Write("privateCloud launch PID=" + application.Id);
                        if (application.WaitForExit(2000) && application.ExitCode != 0)
                            throw new IOException("Installation completed, but privateCloud stopped while opening (exit code " + application.ExitCode +
                                "). Open privateCloud from the Start menu; if it fails again, share the setup log.");
                    }
                }
                resultMessage = uninstall ? "privateCloud was removed. Your account settings and cloud files were kept." :
                    "privateCloud is installed in " + paths.Root + ".\n\n" +
                    (elevatedUser || args.Contains("--no-launch") ? "Open privateCloud from the Start menu using your normal Windows account." : "The privateCloud app has been started.");
                return 0;
            }
            catch (Exception failure)
            {
                setupLog.Write(failure.ToString());
                resultMessage = failure.Message;
                report("Setup could not complete. " + failure.Message);
                if (elevatedUser && paths != null)
                {
                    try
                    {
                        MachineWindowsIntegration.VerifySecurity(Directory.GetAccessControl(paths.Parent));
                        string log = ErrorPath(paths);
                        if (File.Exists(log)) MachineWindowsIntegration.VerifySecurity(File.GetAccessControl(log));
                        File.WriteAllText(log, DateTime.UtcNow.ToString("o") + "\n" + failure);
                    }
                    catch (Exception logFailure) { setupLog.Write("Could not write the maintenance error report: " + logFailure); }
                }
                if (!silent && args.Contains("--uninstall")) MessageBox.Show(failure.Message, "privateCloud setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
        }
        internal static string MaintenanceDirectory(string programParent, string temporaryRoot, bool elevated, Guid uniqueId)
        {
            return Path.GetFullPath(Path.Combine(elevated ? programParent : temporaryRoot,
                (elevated ? "privateCloud.maintenance-" : "privateCloud-maintenance-") + uniqueId.ToString("N")));
        }
        internal static void ValidateInstalledApplicationImage(string installedImage, string processImage)
        {
            if (String.IsNullOrEmpty(installedImage) || String.IsNullOrEmpty(processImage) ||
                !String.Equals(processImage, installedImage, StringComparison.OrdinalIgnoreCase))
                throw new IOException("Please close the other privateCloud copy before installing this version.");
        }
        static string ErrorPath(MachineInstallPaths paths) { return InstallState.Child(paths.Parent, "privateCloud.maintenance-error.txt"); }
        static void ConfigureUser(MachineInstallPaths paths, bool remove)
        {
            string command = WindowsIntegration.Quote(App(paths)) + " --background";
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run"))
            {
                string current = key.GetValue("privateCloud") as string;
                if (remove)
                { if (String.Equals(current, command, StringComparison.OrdinalIgnoreCase)) key.DeleteValue("privateCloud", false); }
                else
                {
                    if (current != null && current.IndexOf("privateCloud.exe", StringComparison.OrdinalIgnoreCase) < 0)
                        throw new IOException("The privateCloud startup entry belongs to another application.");
                    key.SetValue("privateCloud", command);
                    string legacy = key.GetValue("NextcloudDev") as string;
                    if (legacy != null && legacy.IndexOf("\\privateCloud.exe", StringComparison.OrdinalIgnoreCase) >= 0) key.DeleteValue("NextcloudDev", false);
                }
            }
        }
        static void CloseUserApplication(MachineInstallPaths paths)
        {
            if (Elevated) { EnsureNoInstalledApplication(paths); return; }
            foreach (Process process in Process.GetProcessesByName("privateCloud"))
                using (process)
                {
                    if (process.HasExited) continue;
                    if (process.SessionId != Process.GetCurrentProcess().SessionId) continue;
                    string image = process.MainModule.FileName;
                    ValidateInstalledApplicationImage(App(paths), image);
                    using (Process request = Process.Start(new ProcessStartInfo(image, "--quit") { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(image), CreateNoWindow = true }))
                        if (!request.WaitForExit(10000)) throw new IOException("privateCloud did not acknowledge the quit request.");
                    if (!process.WaitForExit(45000)) throw new IOException("privateCloud is still finishing file operations. Try setup again after it closes.");
                }
        }
        static void EnsureNoInstalledApplication(MachineInstallPaths paths)
        {
            foreach (Process process in Process.GetProcessesByName("privateCloud"))
                using (process)
                    if (!process.HasExited && String.Equals(process.MainModule.FileName, App(paths), StringComparison.OrdinalIgnoreCase))
                        throw new IOException("An installed privateCloud process is still running. Close it before maintenance.");
        }
    }
}
