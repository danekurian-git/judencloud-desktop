// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;

namespace PrivateCloud.Setup
{
    internal static class Launcher
    {
        [STAThread]
        static int Main(string[] args)
        {
            bool silent = args.Contains("--silent");
            InstallLock maintenance = null;
            try
            {
                maintenance = new InstallLock();
                string root = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
                if (args.Length >= 3 && args[0] == "--finish-uninstall")
                {
                    root = Path.GetFullPath(args[1]);
                    int parentId = Int32.Parse(args[2]);
                    try { using (Process parent = Process.GetProcessById(parentId)) { if (!parent.WaitForExit(15000)) throw new IOException("Uninstaller is still running."); } }
                    catch (ArgumentException) { }
                    InstallState.CheckOwnedRoot(root);
                    WindowsIntegration.CloseApplication(root);
                    WindowsIntegration integration = new WindowsIntegration(root);
                    integration.CheckOwnership();
                    InstallerCore.UninstallFiles(root);
                    integration.Remove();
                    return 0;
                }
                InstallState.CheckOwnedRoot(root);
                if (args.Contains("--uninstall"))
                {
                    if (!silent && MessageBox.Show("Remove privateCloud from this computer? Your downloaded files and account settings will be kept.", "privateCloud", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return 0;
                    WindowsIntegration.CloseApplication(root);
                    string temporary = Path.Combine(Path.GetTempPath(), "privateCloud-uninstall-" + Guid.NewGuid().ToString("N") + ".exe");
                    File.Copy(Assembly.GetExecutingAssembly().Location, temporary);
                    // Release before the independent helper acquires the same maintenance lock.
                    maintenance.Dispose(); maintenance = null;
                    Process.Start(new ProcessStartInfo(temporary, "--finish-uninstall " + WindowsIntegration.Quote(root) + " " + Process.GetCurrentProcess().Id + (silent ? " --silent" : "")) { UseShellExecute = false });
                    return 0;
                }
                if (args.Contains("--rollback"))
                {
                    WindowsIntegration.CloseApplication(root);
                    InstallerCore.Rollback(root);
                    args = args.Where(a => a != "--rollback" && a != "--silent").ToArray();
                }
                InstallState state = InstallState.Read(root);
                string version = InstallState.VersionPath(root, state.Current);
                Process.Start(WindowsIntegration.ApplicationStart(version, args));
                return 0;
            }
            catch (Exception ex)
            {
                try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "privateCloud-maintenance-error.txt"), ex.ToString()); } catch { }
                if (!silent) MessageBox.Show(ex.Message, "privateCloud", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
            finally { if (maintenance != null) maintenance.Dispose(); }
        }
    }
}
