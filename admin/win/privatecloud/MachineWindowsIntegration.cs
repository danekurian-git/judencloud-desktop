// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32;

namespace PrivateCloud.Setup
{
    internal sealed class MachineWindowsIntegration : IDisposable
    {
        const string KeyName = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\privateCloudMachine";
        readonly MachineInstallPaths paths;
        readonly MachineWindowsService service;
        readonly string shortcut;
        readonly Dictionary<string, object> previousValues = new Dictionary<string, object>();
        readonly Dictionary<string, RegistryValueKind> previousKinds = new Dictionary<string, RegistryValueKind>();
        readonly byte[] previousShortcut;
        internal MachineWindowsIntegration(MachineInstallPaths installPaths)
        {
            paths = installPaths;
            paths.CheckRootPath();
            shortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), "privateCloud.lnk");
            if (File.Exists(shortcut))
            {
                if (!String.Equals(WindowsIntegration.ReadShortcut(shortcut), AppPath, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("The existing privateCloud shortcut belongs to another installation.");
                previousShortcut = File.ReadAllBytes(shortcut);
            }
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(KeyName))
                if (key != null)
                {
                    if (!String.Equals(key.GetValue("InstallLocation") as string, paths.Root, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("The existing privateCloud registration belongs to another installation.");
                    foreach (string name in key.GetValueNames()) { previousValues[name] = key.GetValue(name); previousKinds[name] = key.GetValueKind(name); }
                }
            service = new MachineWindowsService(paths.Root);
        }
        string AppPath { get { return InstallState.Child(paths.Root, "privateCloud.exe"); } }
        internal void StopForChange() { service.StopForChange(); }
        internal void StopFailedActivation() { service.StopFailedActivation(); }
        internal void Activate(string version)
        {
            VerifyProtectedDirectory(paths.Root);
            service.Activate();
            WindowsIntegration.CreateShortcut(shortcut, AppPath);
            using (RegistryKey key = Registry.LocalMachine.CreateSubKey(KeyName))
            {
                key.SetValue("DisplayName", "privateCloud"); key.SetValue("DisplayVersion", version);
                key.SetValue("Publisher", "privateCloud"); key.SetValue("InstallLocation", paths.Root);
                key.SetValue("DisplayIcon", AppPath);
                key.SetValue("UninstallString", WindowsIntegration.Quote(InstallState.Child(paths.Root, "privateCloudMaintenance.exe")) + " --uninstall");
                key.SetValue("NoModify", 1, RegistryValueKind.DWord); key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
        }
        internal void Restore()
        {
            RestoreAll(service.Restore, delegate
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(KeyName))
                    if (key != null && !String.Equals(key.GetValue("InstallLocation") as string, paths.Root, StringComparison.OrdinalIgnoreCase))
                        throw new IOException("The uninstall registration changed ownership during rollback.");
                Registry.LocalMachine.DeleteSubKeyTree(KeyName, false);
                if (previousValues.Count != 0)
                    using (RegistryKey key = Registry.LocalMachine.CreateSubKey(KeyName))
                        foreach (string name in previousValues.Keys) key.SetValue(name, previousValues[name], previousKinds[name]);
            }, delegate
            {
                if (File.Exists(shortcut) && !String.Equals(WindowsIntegration.ReadShortcut(shortcut), AppPath, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("The shortcut changed ownership during rollback.");
                if (previousShortcut != null) InstallState.AtomicWrite(shortcut, previousShortcut);
                else if (File.Exists(shortcut)) File.Delete(shortcut);
            });
        }
        internal static void RestoreAll(params Action[] actions)
        {
            List<Exception> failures = new List<Exception>();
            foreach (Action action in actions) try { action(); } catch (Exception failure) { failures.Add(failure); }
            if (failures.Count != 0) throw new AggregateException("Some privateCloud integration recovery steps failed.", failures);
        }
        internal void Remove()
        {
            service.Remove();
            using (RegistryKey key = Registry.LocalMachine.OpenSubKey(KeyName))
                if (key != null && !String.Equals(key.GetValue("InstallLocation") as string, paths.Root, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("The uninstall registration changed ownership.");
            Registry.LocalMachine.DeleteSubKeyTree(KeyName, false);
            if (File.Exists(shortcut) && String.Equals(WindowsIntegration.ReadShortcut(shortcut), AppPath, StringComparison.OrdinalIgnoreCase)) File.Delete(shortcut);
        }
        internal static DirectorySecurity ProtectedSecurity()
        {
            DirectorySecurity security = new DirectorySecurity();
            security.SetAccessRuleProtection(true, false);
            SecurityIdentifier admin = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
            security.SetOwner(admin);
            InheritanceFlags inheritance = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
            foreach (WellKnownSidType type in new[] { WellKnownSidType.LocalSystemSid, WellKnownSidType.BuiltinAdministratorsSid })
                security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(type, null), FileSystemRights.FullControl, inheritance, PropagationFlags.None, AccessControlType.Allow));
            security.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null), FileSystemRights.ReadAndExecute, inheritance, PropagationFlags.None, AccessControlType.Allow));
            return security;
        }
        internal static void CreateProtectedDirectory(string path)
        {
            if (Directory.Exists(path) || File.Exists(path)) throw new IOException("The staged installation directory already exists.");
            Directory.CreateDirectory(path, ProtectedSecurity());
            VerifyProtectedDirectory(path);
        }
        internal static bool TrustedOwner(SecurityIdentifier sid)
        {
            return sid != null && (sid.IsWellKnown(WellKnownSidType.LocalSystemSid) || sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) ||
                sid.Value == "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
        }
        internal static void VerifySecurity(FileSystemSecurity security)
        {
            RawSecurityDescriptor raw = new RawSecurityDescriptor(security.GetSecurityDescriptorBinaryForm(), 0);
            if (!TrustedOwner(raw.Owner) || raw.DiscretionaryAcl == null) throw new IOException("The program location is not administrator-protected.");
            const int write = 0x00000156 | 0x00010000 | 0x00040000 | 0x00080000 | 0x40000000 | 0x10000000 | 0x02000000;
            foreach (GenericAce ace in raw.DiscretionaryAcl)
            {
                if ((ace.AceFlags & AceFlags.InheritOnly) != 0) continue;
                CommonAce common = ace as CommonAce;
                if (common == null || common.IsCallback) throw new IOException("The program location has unsupported security rules.");
                if (common.AceQualifier == AceQualifier.AccessDenied) continue;
                if (common.AceQualifier != AceQualifier.AccessAllowed || ((common.AccessMask & write) != 0 && !TrustedOwner(common.SecurityIdentifier)))
                    throw new IOException("The program location can be changed by a non-administrator.");
            }
        }
        internal static void VerifyProtectedDirectory(string root)
        {
            Stack<string> pending = new Stack<string>(); pending.Push(root);
            while (pending.Count != 0)
            {
                string at = pending.Pop();
                FileAttributes attributes = File.GetAttributes(at);
                if ((attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("The program location redirects to another directory.");
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    VerifySecurity(Directory.GetAccessControl(at, AccessControlSections.Owner | AccessControlSections.Access));
                    foreach (string child in Directory.GetFileSystemEntries(at)) pending.Push(child);
                }
                else VerifySecurity(File.GetAccessControl(at, AccessControlSections.Owner | AccessControlSections.Access));
            }
        }
        public void Dispose() { service.Dispose(); }
    }
}
