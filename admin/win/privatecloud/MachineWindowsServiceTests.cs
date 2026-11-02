// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;

namespace PrivateCloud.Setup
{
    internal static class MachineWindowsServiceTests
    {
        static int passed;
        static readonly string Root = Path.Combine(Path.GetTempPath(), "privateCloud-scm-fixture");
        static void Assert(bool condition, string message) { if (!condition) throw new Exception(message); }
        static void Reject(Action action) { bool failed = false; try { action(); } catch { failed = true; } Assert(failed, "Expected refusal"); }
        static void Test(string name, Action action) { action(); ++passed; Console.WriteLine("PASS " + name); }

        sealed class FakeBackend : MachineWindowsService.IBackend
        {
            internal MachineWindowsService.Configuration Current;
            internal readonly List<string> Calls = new List<string>();
            internal bool FailApply, FailStart, FailStop, FailDisable, Disposed;
            internal FakeBackend(bool existing, uint state)
            {
                if (existing) { Current = MachineWindowsService.Expected(Root); Current.State = state; }
            }
            public MachineWindowsService.Configuration Read() { Calls.Add("read"); return Current == null ? null : Current.Copy(); }
            public void CreateDisabled(MachineWindowsService.Configuration configuration)
            {
                Calls.Add("create-disabled"); Assert(Current == null, "Replaced another service");
                Current = configuration.Copy(); Current.StartType = MachineWindowsService.Disabled;
                // Native CreateService starts with no RequiredPrivileges/recovery contract.
                Current.Privileges = new string[0]; Current.Actions = new MachineWindowsService.ActionEntry[0];
            }
            public void Apply(MachineWindowsService.Configuration configuration)
            {
                Calls.Add("apply"); Assert(Current.State == MachineWindowsService.Stopped, "Applied configuration while running");
                Assert(configuration.StartType == MachineWindowsService.Disabled, "Enabled before completing configuration");
                if (FailApply) throw new IOException("injected partial configuration failure");
                Current = configuration.Copy(); Current.State = MachineWindowsService.Stopped;
            }
            public void SetStartType(uint startType)
            {
                Calls.Add("start-type-" + startType);
                if (FailDisable && startType == MachineWindowsService.Disabled) throw new IOException("injected disable failure");
                Current.StartType = startType;
            }
            public void Stop(int timeout)
            {
                Calls.Add("stop"); Assert(timeout == 30000, "Stop was not bounded");
                Assert(Current.StartType == MachineWindowsService.Disabled, "Service was not disabled before stop");
                if (FailStop) throw new TimeoutException("injected blocked process exit");
                Current.State = MachineWindowsService.Stopped;
            }
            public void Start(int timeout)
            {
                Calls.Add("start"); Assert(timeout == 30000, "Start was not bounded");
                Assert(Current.StartType != MachineWindowsService.Disabled, "Started disabled service");
                MachineWindowsService.Validate(Current, MachineWindowsService.CommandFor(Root));
                Current.State = MachineWindowsService.Running;
                if (FailStart) throw new TimeoutException("injected startup completion failure");
            }
            public void Delete()
            {
                Calls.Add("delete"); Assert(Current.State == MachineWindowsService.Stopped && Current.StartType == MachineWindowsService.Disabled, "Deleted active service");
                Current = null;
            }
            public void Dispose() { Disposed = true; }
        }

        sealed class ProcessFixture : MachineWindowsService.IProcessLifetime
        {
            internal string Path;
            internal bool Disposed, Exits = true;
            internal Action OnWait;
            internal readonly List<int> Waits = new List<int>();
            public string ImagePath { get { return Path; } }
            public bool WaitForExit(int timeout) { Waits.Add(timeout); if (OnWait != null) OnWait(); return Exits; }
            public void Dispose() { Disposed = true; }
        }
        sealed class ProcessSourceFixture : MachineWindowsService.IProcessSource
        {
            internal uint[] Ids = new uint[0];
            internal bool RefuseOpen;
            internal readonly Dictionary<uint, ProcessFixture> Processes = new Dictionary<uint, ProcessFixture>();
            public uint[] FindNamedProcesses(string name, int maximum)
            {
                Assert(name == "privateCloudDirectBroker.exe" && maximum == 4096, "Process scan scope widened");
                return Ids;
            }
            public MachineWindowsService.IProcessLifetime Open(uint id)
            {
                if (RefuseOpen) throw new IOException("injected ambiguous process identity");
                ProcessFixture process; return Processes.TryGetValue(id, out process) ? process : null;
            }
        }

        static void RejectConfiguration(Action<MachineWindowsService.Configuration> mutation)
        {
            FakeBackend backend = new FakeBackend(true, MachineWindowsService.Stopped);
            mutation(backend.Current);
            Reject(delegate { new MachineWindowsService(Root, backend); });
            Assert(backend.Calls.Count == 1 && backend.Calls[0] == "read" && backend.Disposed, "Foreign service was changed or leaked");
        }

        static int Main()
        {
            try
            {
                Test("constructor is read-only and captures the exact service", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                        Assert(backend.Calls.Count == 1 && backend.Current.State == MachineWindowsService.Running, "Constructor changed service");
                    Assert(backend.Disposed, "SCM backend was not disposed");
                });
                Test("only documented stable service states supply valid process IDs", delegate {
                    for (uint state = 0; state < 10; ++state)
                        Assert(MachineWindowsService.ProcessIdIsValid(state) == (state >= 4 && state <= 7), "Invalid transition PID admitted");
                });
                Test("fixed quoted executable and service switch are required", delegate {
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Command = "C:\\other\\broker.exe --service"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Command += " --console"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Command = value.Command.Replace("\"", ""); });
                });
                Test("only LocalSystem own-process identity is admitted", delegate {
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Account = "NT AUTHORITY\\LocalService"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Type = 0x20; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Type = 0x110; });
                });
                Test("privileges SID triggers and dependencies cannot expand", delegate {
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Privileges = new[] { "SeDebugPrivilege", "SeChangeNotifyPrivilege" }; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Privileges = new string[0]; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.SidType = 0; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.TriggerCount = 1; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Dependencies = "BFE\0OtherService"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Dependencies = ""; });
                });
                Test("recovery cannot run commands reboot or retry indefinitely", delegate {
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.FailureCommand = "cmd.exe"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.RebootMessage = "restart"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Actions[0] = new MachineWindowsService.ActionEntry(2, 5000); });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Actions[2] = new MachineWindowsService.ActionEntry(1, 30000); });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.ResetPeriod = 0; });
                });
                Test("other unexpected configuration is refused", delegate {
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Display = "foreign service"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Group = "foreign"; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.Tag = 1; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.ErrorControl = 3; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.DelayedStart = true; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.LaunchProtected = 1; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.StartType = 0; });
                    RejectConfiguration(delegate(MachineWindowsService.Configuration value) { value.State = 2; });
                });
                Test("fresh activation configures while disabled then starts and verifies", delegate {
                    FakeBackend backend = new FakeBackend(false, 0);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        service.StopForChange(); service.Activate();
                        Assert(backend.Current.State == MachineWindowsService.Running && backend.Current.StartType == MachineWindowsService.Automatic, "Fresh service is not active");
                        Assert(backend.Calls.IndexOf("apply") < backend.Calls.IndexOf("start-type-2") && backend.Calls.IndexOf("start-type-2") < backend.Calls.IndexOf("start"), "Configuration/start order changed");
                        Assert(backend.Current.Dependencies == "BFE", "Boot engine dependency missing");
                    }
                });
                Test("upgrade disables before stop and failed activation retains existing service", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        service.StopForChange(); service.Activate(); service.StopFailedActivation();
                        Assert(backend.Current != null && backend.Current.State == MachineWindowsService.Stopped && !backend.Calls.Contains("delete"), "Existing service deleted or running after rollback stop");
                        service.Restore();
                        Assert(backend.Current.State == MachineWindowsService.Running && backend.Current.StartType == MachineWindowsService.Automatic, "Original running state not restored");
                    }
                });
                Test("rollback restores original stopped manual service without starting", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Stopped); backend.Current.StartType = MachineWindowsService.Manual;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        service.StopForChange(); service.Activate(); service.StopFailedActivation();
                        int starts = backend.Calls.FindAll(delegate(string call) { return call == "start"; }).Count;
                        service.Restore();
                        Assert(backend.Current.State == MachineWindowsService.Stopped && backend.Current.StartType == MachineWindowsService.Manual, "Stopped manual service not restored");
                        Assert(backend.Calls.FindAll(delegate(string call) { return call == "start"; }).Count == starts, "Originally stopped service restarted");
                    }
                });
                Test("rollback restores originally running disabled service", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running); backend.Current.StartType = MachineWindowsService.Disabled;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        service.StopForChange(); service.Restore();
                        Assert(backend.Current.State == MachineWindowsService.Running && backend.Current.StartType == MachineWindowsService.Disabled, "Disabled running state not restored");
                    }
                });
                Test("partial fresh configuration failure remains safely removable", delegate {
                    FakeBackend backend = new FakeBackend(false, 0); backend.FailApply = true;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        Reject(service.Activate); service.StopFailedActivation(); service.Restore();
                        Assert(backend.Current == null && backend.Calls.Contains("delete"), "Partial created service retained");
                    }
                });
                Test("partial fresh service cleanup still rejects a foreign process identity", delegate {
                    FakeBackend backend = new FakeBackend(false, 0); backend.FailApply = true;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        Reject(service.Activate); backend.Current.Command = "C:\\foreign.exe";
                        Reject(service.StopFailedActivation);
                        Assert(!backend.Calls.Contains("stop") && !backend.Calls.Contains("delete"), "Foreign process was stopped during partial cleanup");
                    }
                });
                Test("startup failure stops and deletes only freshly created service", delegate {
                    FakeBackend backend = new FakeBackend(false, 0); backend.FailStart = true;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        Reject(service.Activate); service.StopFailedActivation(); service.Restore();
                        Assert(backend.Current == null, "Failed fresh service retained");
                    }
                });
                Test("stop timeout propagates without deleting or restoring", delegate {
                    FakeBackend backend = new FakeBackend(false, 0);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        service.Activate(); backend.FailStop = true;
                        Reject(service.StopFailedActivation);
                        Assert(backend.Current != null && backend.Current.State == MachineWindowsService.Running && !backend.Calls.Contains("delete"), "Blocked running service was deleted");
                    }
                });
                Test("disable failure does not request stop", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running); backend.FailDisable = true;
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        Reject(service.StopForChange); Assert(!backend.Calls.Contains("stop"), "Stopped before disabling restart");
                    }
                });
                Test("service appearing after initial snapshot is never overwritten", delegate {
                    FakeBackend backend = new FakeBackend(false, 0);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        backend.Current = MachineWindowsService.Expected(Root);
                        Reject(service.Activate); Assert(!backend.Calls.Contains("create-disabled") && !backend.Calls.Contains("apply"), "Concurrent service overwritten");
                    }
                });
                Test("configuration changes after capture prevent maintenance", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend))
                    {
                        backend.Current.Command = "C:\\foreign.exe";
                        Reject(service.StopForChange); Assert(!backend.Calls.Contains("stop"), "Foreign process stopped");
                    }
                });
                Test("uninstall disables stops and removes owned service only", delegate {
                    FakeBackend backend = new FakeBackend(true, MachineWindowsService.Running);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend)) service.Remove();
                    Assert(backend.Current == null && backend.Calls.IndexOf("stop") < backend.Calls.IndexOf("delete"), "Incorrect service removal order");
                });
                Test("missing service removal is idempotent", delegate {
                    FakeBackend backend = new FakeBackend(false, 0);
                    using (MachineWindowsService service = new MachineWindowsService(Root, backend)) service.Remove();
                    Assert(!backend.Calls.Contains("delete") && !backend.Calls.Contains("stop"), "Missing service changed");
                });
                Test("failed-start residual drain waits only for exact protected image", delegate {
                    string executable = Path.Combine(Root, "privateCloudDirectBroker.exe");
                    ProcessFixture owned = new ProcessFixture { Path = executable };
                    ProcessFixture foreign = new ProcessFixture { Path = Path.Combine(Root, "other", "privateCloudDirectBroker.exe") };
                    ProcessSourceFixture source = new ProcessSourceFixture { Ids = new uint[] { 1, 2, 3 } };
                    source.Processes.Add(1, owned); source.Processes.Add(2, foreign);
                    MachineWindowsService.DrainExactImage(executable, source, delegate { return 1000; });
                    Assert(owned.Waits.Count == 1 && foreign.Waits.Count == 0 && owned.Disposed && foreign.Disposed, "Drain touched foreign process or leaked handles");
                });
                Test("residual process waits share one deadline", delegate {
                    string executable = Path.Combine(Root, "privateCloudDirectBroker.exe"); int remaining = 1000;
                    ProcessFixture first = new ProcessFixture { Path = executable, OnWait = delegate { remaining -= 900; } };
                    ProcessFixture second = new ProcessFixture { Path = executable };
                    ProcessSourceFixture source = new ProcessSourceFixture { Ids = new uint[] { 1, 2 } };
                    source.Processes.Add(1, first); source.Processes.Add(2, second);
                    MachineWindowsService.DrainExactImage(executable, source, delegate { return remaining; });
                    Assert(first.Waits[0] == 1000 && second.Waits[0] == 100, "Each process received a fresh timeout");
                });
                Test("ambiguous process identity refuses cleanup", delegate {
                    ProcessSourceFixture source = new ProcessSourceFixture { Ids = new uint[] { 1 }, RefuseOpen = true };
                    Reject(delegate { MachineWindowsService.DrainExactImage(Path.Combine(Root, "privateCloudDirectBroker.exe"), source, delegate { return 1000; }); });
                });
                Test("residual timeout releases every pinned handle and refuses cleanup", delegate {
                    string executable = Path.Combine(Root, "privateCloudDirectBroker.exe");
                    ProcessFixture first = new ProcessFixture { Path = executable, Exits = false };
                    ProcessFixture second = new ProcessFixture { Path = executable };
                    ProcessSourceFixture source = new ProcessSourceFixture { Ids = new uint[] { 1, 2 } };
                    source.Processes.Add(1, first); source.Processes.Add(2, second);
                    Reject(delegate { MachineWindowsService.DrainExactImage(executable, source, delegate { return 10; }); });
                    Assert(first.Disposed && second.Disposed && second.Waits.Count == 0, "Timeout leaked process handles");
                });
                Test("excess process count and expired enumeration deadline refuse cleanup", delegate {
                    string executable = Path.Combine(Root, "privateCloudDirectBroker.exe");
                    ProcessSourceFixture source = new ProcessSourceFixture { Ids = new uint[4097] };
                    Reject(delegate { MachineWindowsService.DrainExactImage(executable, source, delegate { return 1000; }); });
                    source.Ids = new uint[] { 1 };
                    Reject(delegate { MachineWindowsService.DrainExactImage(executable, source, delegate { return 0; }); });
                });
                Console.WriteLine(passed + " isolated service tests passed; no SCM operations ran.");
                return 0;
            }
            catch (Exception error) { Console.Error.WriteLine(error); return 1; }
        }
    }
}
