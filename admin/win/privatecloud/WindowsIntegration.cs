// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Linq;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace PrivateCloud.Setup
{
    internal sealed class WindowsIntegration
    {
        const string UninstallKey = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\privateCloud";
        const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
        readonly string root;
        readonly string shortcut;
        readonly Dictionary<string, object> oldValues = new Dictionary<string, object>();
        byte[] oldShortcut;
        object oldRun;
        object oldLegacyRun;
        bool legacyRunRemoved;

        internal WindowsIntegration(string installRoot)
        {
            root = installRoot;
            shortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "privateCloud.lnk");
            if (File.Exists(shortcut)) oldShortcut = File.ReadAllBytes(shortcut);
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(UninstallKey))
                if (key != null) foreach (string name in key.GetValueNames()) oldValues[name] = key.GetValue(name);
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey))
                if (key != null) { oldRun = key.GetValue("privateCloud"); oldLegacyRun = key.GetValue("NextcloudDev"); }
        }

        internal void Activate(string version)
        {
            string launcher = InstallState.Child(root, "privateCloud.exe");
            CreateShortcut(shortcut, launcher);
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                key.SetValue("privateCloud", Quote(launcher) + " --background");
                string legacy = oldLegacyRun as string;
                if (legacy != null && System.Text.RegularExpressions.Regex.IsMatch(legacy, @"(?i)\\privateCloud\.exe(?:\""|\s|$)"))
                { key.DeleteValue("NextcloudDev", false); legacyRunRemoved = true; }
            }
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(UninstallKey))
            {
                key.SetValue("DisplayName", "privateCloud");
                key.SetValue("DisplayVersion", version);
                key.SetValue("Publisher", "privateCloud");
                key.SetValue("InstallLocation", root);
                key.SetValue("DisplayIcon", launcher);
                key.SetValue("UninstallString", Quote(launcher) + " --uninstall");
                key.SetValue("QuietUninstallString", Quote(launcher) + " --uninstall --silent");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
        }

        internal void Restore()
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKey))
            {
                if (oldRun != null) key.SetValue("privateCloud", oldRun); else key.DeleteValue("privateCloud", false);
                if (legacyRunRemoved) key.SetValue("NextcloudDev", oldLegacyRun);
            }
            Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
            if (oldValues.Count != 0)
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(UninstallKey))
                    foreach (KeyValuePair<string, object> value in oldValues) key.SetValue(value.Key, value.Value);
            if (oldShortcut != null) InstallState.AtomicWrite(shortcut, oldShortcut);
            else if (File.Exists(shortcut)) File.Delete(shortcut);
        }

        internal void CheckOwnership()
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(UninstallKey))
                if (key == null || !String.Equals(key.GetValue("InstallLocation") as string, root, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("This uninstaller does not own the current installation registration.");
        }

        internal void Remove()
        {
            CheckOwnership();
            string launcher = InstallState.Child(root, "privateCloud.exe");
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey, true))
                if (key != null && String.Equals(key.GetValue("privateCloud") as string, Quote(launcher) + " --background", StringComparison.OrdinalIgnoreCase))
                    key.DeleteValue("privateCloud", false);
            Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
            if (File.Exists(shortcut) && String.Equals(ReadShortcut(shortcut), launcher, StringComparison.OrdinalIgnoreCase)) File.Delete(shortcut);
        }

        internal static string Quote(string value)
        {
            // CommandLineToArgvW quoting, including trailing slashes and embedded quotes.
            System.Text.StringBuilder result = new System.Text.StringBuilder("\"");
            int slashes = 0;
            foreach (char ch in value)
            {
                if (ch == '\\') { ++slashes; continue; }
                result.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes);
                result.Append(ch);
                slashes = 0;
            }
            result.Append('\\', slashes * 2);
            return result.Append('"').ToString();
        }

        internal static ProcessStartInfo ApplicationStart(string versionDirectory, string[] arguments)
        {
            ProcessStartInfo start = new ProcessStartInfo(InstallState.Child(versionDirectory, "privateCloud.exe"),
                String.Join(" ", arguments.Select(Quote))) { UseShellExecute = false, WorkingDirectory = versionDirectory };
            start.EnvironmentVariables["PRIVATECLOUD_MANAGED_STARTUP"] = "1";
            return start;
        }

        internal static void CreateShortcut(string path, string target)
        {
            object shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
            string pending = path + ".pending.lnk";
            object link = null;
            try
            {
                link = shell.GetType().InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { pending });
                link.GetType().InvokeMember("TargetPath", BindingFlags.SetProperty, null, link, new object[] { target });
                link.GetType().InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, link, new object[] { Path.GetDirectoryName(target) });
                link.GetType().InvokeMember("Save", BindingFlags.InvokeMethod, null, link, null);
                if (File.Exists(path)) File.Replace(pending, path, null); else File.Move(pending, path);
            }
            finally { if (link != null) Marshal.FinalReleaseComObject(link); Marshal.FinalReleaseComObject(shell); if (File.Exists(pending)) File.Delete(pending); }
        }

        internal static string ReadShortcut(string path)
        {
            object shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell"));
            object link = null;
            try
            {
                link = shell.GetType().InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { path });
                return link.GetType().InvokeMember("TargetPath", BindingFlags.GetProperty, null, link, null) as string;
            }
            finally { if (link != null) Marshal.FinalReleaseComObject(link); Marshal.FinalReleaseComObject(shell); }
        }

        internal static void CloseApplication(string root)
        {
            foreach (Process process in Process.GetProcessesByName("privateCloud").Concat(Process.GetProcessesByName("nextclouddev")))
            {
                using (process)
                {
                    if (process.Id == Process.GetCurrentProcess().Id || process.HasExited) continue;
                    string path = process.MainModule.FileName;
                    if (!path.StartsWith(Path.GetFullPath(root).TrimEnd('\\') + "\\versions\\", StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Please quit the other copy of privateCloud before installing or removing this one.");
                    using (Process request = Process.Start(new ProcessStartInfo(path, "--quit") { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(path) }))
                        if (!request.WaitForExit(10000)) throw new IOException("privateCloud did not acknowledge the quit request.");
                    if (!process.WaitForExit(45000)) throw new IOException("privateCloud is still finishing file operations. Please try again after it closes.");
                }
            }
        }
    }
}
