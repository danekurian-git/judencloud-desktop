// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Security.AccessControl;
using System.Security.Principal;

namespace PrivateCloud.Setup
{
    internal static class MachineWindowsIntegrationTests
    {
        static int passed;
        static readonly SecurityIdentifier admin = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        static readonly SecurityIdentifier system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        static readonly SecurityIdentifier users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);
        static readonly SecurityIdentifier ordinary = new SecurityIdentifier("S-1-5-21-1-2-3-1001");
        static readonly SecurityIdentifier installer = new SecurityIdentifier("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
        static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }
        static void Reject(FileSystemSecurity security)
        {
            try { MachineWindowsIntegration.VerifySecurity(security); }
            catch (IOException) { return; }
            throw new Exception("Expected unsafe security descriptor rejection");
        }
        static CommonAce Allow(SecurityIdentifier sid, int mask, AceFlags flags = AceFlags.None)
        {
            return new CommonAce(flags, AceQualifier.AccessAllowed, mask, sid, false, null);
        }
        static DirectorySecurity Descriptor(SecurityIdentifier owner, params GenericAce[] entries)
        {
            RawAcl acl = null;
            if (entries != null)
            {
                acl = new RawAcl(GenericAcl.AclRevisionDS, entries.Length);
                for (int i = 0; i < entries.Length; ++i) acl.InsertAce(i, entries[i]);
            }
            RawSecurityDescriptor raw = new RawSecurityDescriptor(ControlFlags.DiscretionaryAclPresent | ControlFlags.DiscretionaryAclProtected,
                owner, admin, null, acl);
            byte[] bytes = new byte[raw.BinaryLength]; raw.GetBinaryForm(bytes, 0);
            DirectorySecurity security = new DirectorySecurity(); security.SetSecurityDescriptorBinaryForm(bytes);
            return security;
        }

        [STAThread]
        static int Main()
        {
            try
            {
                Test("elevated maintenance selects protected program sibling and ignores user TEMP", delegate {
                    Guid id = new Guid("01234567-89ab-cdef-0123-456789abcdef");
                    string parent = @"C:\Program Files";
                    string result = MachineSetupProgram.MaintenanceDirectory(parent, @"C:\Users\example\AppData\Local\Temp", true, id);
                    Assert(result == Path.Combine(parent, "privateCloud.maintenance-0123456789abcdef0123456789abcdef"), "Elevated maintenance selected user-writable TEMP");
                    Assert(result == MachineSetupProgram.MaintenanceDirectory(parent, null, true, id), "Elevated staging depends on user TEMP");
                    Assert(!result.StartsWith(Path.Combine(parent, "privateCloud") + Path.DirectorySeparatorChar), "Maintenance remains inside the root being removed");
                });
                Test("normal maintenance selects user TEMP without requesting protected staging", delegate {
                    Guid id = new Guid("fedcba98-7654-3210-fedc-ba9876543210");
                    string temporaryRoot = @"C:\Users\example\AppData\Local\Temp";
                    string result = MachineSetupProgram.MaintenanceDirectory(@"C:\Program Files", temporaryRoot, false, id);
                    Assert(result == Path.Combine(temporaryRoot, "privateCloud-maintenance-fedcba9876543210fedcba9876543210"), "Unelevated maintenance selected Program Files");
                    Assert(result == MachineSetupProgram.MaintenanceDirectory(null, temporaryRoot, false, id), "Normal staging depends on Program Files");
                });
                Test("separate maintenance attempts receive separate sibling paths", delegate {
                    string first = MachineSetupProgram.MaintenanceDirectory(@"C:\Program Files", @"C:\Temp", true, Guid.Empty);
                    string second = MachineSetupProgram.MaintenanceDirectory(@"C:\Program Files", @"C:\Temp", true, new Guid("00000000-0000-0000-0000-000000000001"));
                    Assert(first != second, "Maintenance attempts reuse a program image path");
                });
                Test("quit request accepts only exact installed application image ignoring case", delegate {
                    string expected = @"C:\Program Files\privateCloud\privateCloud.exe";
                    MachineSetupProgram.ValidateInstalledApplicationImage(expected, expected);
                    MachineSetupProgram.ValidateInstalledApplicationImage(expected, expected.ToUpperInvariant());
                    foreach (string image in new[] { null, "", @"D:\privateCloud\App\privateCloud.exe",
                        @"C:\Program Files\privateCloud.other\privateCloud.exe", @"C:\Program Files\privateCloud\other.exe",
                        expected + ".old", "\"" + expected + "\"", expected + " --quit" })
                    {
                        bool rejected = false;
                        try { MachineSetupProgram.ValidateInstalledApplicationImage(expected, image); }
                        catch (IOException) { rejected = true; }
                        Assert(rejected, "Foreign process image was accepted: " + image);
                    }
                });
                Test("rollback attempts later repairs and aggregates every failure", delegate {
                    string order = "";
                    IOException first = new IOException("service restore failed"), last = new IOException("shortcut restore failed");
                    AggregateException caught = null;
                    try
                    {
                        MachineWindowsIntegration.RestoreAll(delegate { order += "service;"; throw first; },
                            delegate { order += "registration;"; }, delegate { order += "shortcut;"; throw last; });
                    }
                    catch (AggregateException failure) { caught = failure; }
                    Assert(order == "service;registration;shortcut;", "Later recovery steps were skipped");
                    Assert(caught != null && caught.InnerExceptions.Count == 2 &&
                        Object.ReferenceEquals(caught.InnerExceptions[0], first) && Object.ReferenceEquals(caught.InnerExceptions[1], last), "Recovery failures were lost");
                });
                Test("successful rollback runs each repair once", delegate {
                    int count = 0;
                    MachineWindowsIntegration.RestoreAll(delegate { ++count; }, delegate { ++count; }, delegate { ++count; });
                    Assert(count == 3, "Unexpected rollback action count");
                });
                Test("generated DACL is protected with administrator ownership and inheritable grants", delegate {
                    DirectorySecurity security = MachineWindowsIntegration.ProtectedSecurity();
                    Assert(security.AreAccessRulesProtected, "Staging DACL permits unwanted inheritance");
                    Assert(((SecurityIdentifier)security.GetOwner(typeof(SecurityIdentifier))).Equals(admin), "Wrong protected owner");
                    AuthorizationRuleCollection rules = security.GetAccessRules(true, true, typeof(SecurityIdentifier));
                    Assert(rules.Count == 3, "Unexpected staging grants");
                    foreach (FileSystemAccessRule rule in rules)
                    {
                        Assert(rule.AccessControlType == AccessControlType.Allow && !rule.IsInherited, "Unexpected grant type");
                        Assert(rule.InheritanceFlags == (InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit) &&
                            rule.PropagationFlags == PropagationFlags.None, "Children do not inherit protection");
                        SecurityIdentifier sid = (SecurityIdentifier)rule.IdentityReference;
                        if (sid.Equals(users)) Assert((rule.FileSystemRights & FileSystemRights.Modify) == (FileSystemRights.ReadAndExecute & FileSystemRights.Modify), "Users can modify payload");
                        else Assert((sid.Equals(admin) || sid.Equals(system)) && rule.FileSystemRights == FileSystemRights.FullControl, "Unexpected privileged grant");
                    }
                    MachineWindowsIntegration.VerifySecurity(security);
                });
                Test("only SYSTEM administrators and TrustedInstaller are trusted owners", delegate {
                    foreach (SecurityIdentifier sid in new[] { system, admin, installer })
                    {
                        Assert(MachineWindowsIntegration.TrustedOwner(sid), "Trusted owner rejected");
                        MachineWindowsIntegration.VerifySecurity(Descriptor(sid, Allow(admin, (int)FileSystemRights.FullControl)));
                    }
                    foreach (SecurityIdentifier sid in new[] { users, ordinary, new SecurityIdentifier(WellKnownSidType.AuthenticatedUserSid, null) })
                    {
                        Assert(!MachineWindowsIntegration.TrustedOwner(sid), "Ordinary principal trusted");
                        Reject(Descriptor(sid, Allow(admin, (int)FileSystemRights.FullControl)));
                    }
                    Assert(!MachineWindowsIntegration.TrustedOwner(null), "Null owner trusted");
                });
                Test("null owner is rejected even with privileged-only grants", delegate { Reject(Descriptor(null, Allow(admin, (int)FileSystemRights.FullControl))); });
                Test("null DACL cannot grant unrestricted access", delegate { Reject(Descriptor(admin, null)); });
                Test("ordinary read and execute access remains permitted", delegate {
                    MachineWindowsIntegration.VerifySecurity(Descriptor(admin, Allow(users, (int)FileSystemRights.ReadAndExecute),
                        Allow(ordinary, unchecked((int)0x80000000)), Allow(ordinary, 0x20000000)));
                });
                int[] dangerous = { 0x2, 0x4, 0x10, 0x40, 0x100, 0x10000, 0x40000, 0x80000, 0x40000000, 0x10000000, 0x02000000 };
                foreach (int bits in dangerous)
                {
                    int rights = bits;
                    Test("ordinary mutation right 0x" + rights.ToString("x") + " is rejected", delegate {
                        Reject(Descriptor(admin, Allow(ordinary, rights)));
                        Reject(Descriptor(admin, Allow(users, rights, AceFlags.Inherited)));
                    });
                }
                Test("trusted service and administrator mutation grants are accepted", delegate {
                    MachineWindowsIntegration.VerifySecurity(Descriptor(admin, Allow(system, (int)FileSystemRights.FullControl),
                        Allow(admin, 0x10000000), Allow(installer, 0x40000000)));
                });
                Test("deny rule cannot hide an ordinary writable allow rule", delegate {
                    Reject(Descriptor(admin, new CommonAce(AceFlags.None, AceQualifier.AccessDenied, (int)FileSystemRights.FullControl, ordinary, false, null),
                        Allow(ordinary, (int)FileSystemRights.WriteData)));
                });
                Test("deny-only restriction plus ordinary read access is accepted", delegate {
                    MachineWindowsIntegration.VerifySecurity(Descriptor(admin,
                        new CommonAce(AceFlags.None, AceQualifier.AccessDenied, (int)FileSystemRights.WriteData, users, false, null),
                        Allow(users, (int)FileSystemRights.ReadAndExecute)));
                });
                Test("inherit-only grants are skipped for parent but rejected when effective on a child", delegate {
                    MachineWindowsIntegration.VerifySecurity(Descriptor(admin, Allow(ordinary, (int)FileSystemRights.WriteData,
                        AceFlags.ContainerInherit | AceFlags.ObjectInherit | AceFlags.InheritOnly)));
                    Reject(Descriptor(admin, Allow(ordinary, (int)FileSystemRights.WriteData, AceFlags.Inherited)));
                });
                Test("conditional callback allow cannot bypass the trust check", delegate {
                    Reject(Descriptor(admin, new CommonAce(AceFlags.None, AceQualifier.AccessAllowed, (int)FileSystemRights.Read,
                        ordinary, true, new byte[0])));
                });
                Test("object-specific ACEs fail closed instead of being misinterpreted", delegate {
                    Reject(Descriptor(admin, new ObjectAce(AceFlags.None, AceQualifier.AccessAllowed, (int)FileSystemRights.Read,
                        ordinary, ObjectAceFlags.ObjectAceTypePresent, Guid.NewGuid(), Guid.Empty, false, null)));
                });
                Console.WriteLine(passed + " in-memory machine security tests passed; no ACL, profile, registry or service was changed.");
                return 0;
            }
            catch (Exception failure) { Console.Error.WriteLine(failure); return 1; }
        }
    }
}
