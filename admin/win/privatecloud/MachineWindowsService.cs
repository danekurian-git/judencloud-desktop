// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace PrivateCloud.Setup
{
    internal sealed class MachineWindowsService : IDisposable
    {
        internal const string ServiceName = "privateCloudDirectBroker";
        internal const string DisplayName = "privateCloud Direct Connection";
        internal const uint OwnProcess = 0x10, Automatic = 2, Manual = 3, Disabled = 4;
        internal const uint Stopped = 1, Running = 4;
        internal const int TransitionTimeoutMilliseconds = 30000;
        internal const int MaximumProcessEnumeration = 4096;
        readonly IBackend backend;
        readonly Configuration original;
        readonly Configuration desired;
        readonly string command;
        bool created, changed, disposed;

        internal sealed class Configuration
        {
            internal string Command, Account, Display, Group, Dependencies, RebootMessage, FailureCommand;
            internal uint Type, StartType, ErrorControl, Tag, SidType, ResetPeriod, TriggerCount, LaunchProtected;
            internal bool DelayedStart, FailureActionsOnNonCrash;
            internal string[] Privileges;
            internal ActionEntry[] Actions;
            internal uint State;

            internal Configuration Copy()
            {
                Configuration copy = (Configuration)MemberwiseClone();
                copy.Privileges = (string[])Privileges.Clone();
                copy.Actions = (ActionEntry[])Actions.Clone();
                return copy;
            }
        }

        internal struct ActionEntry
        {
            internal uint Type, Delay;
            internal ActionEntry(uint type, uint delay) { Type = type; Delay = delay; }
        }

        internal interface IBackend : IDisposable
        {
            Configuration Read();
            void CreateDisabled(Configuration configuration);
            void Apply(Configuration configuration);
            void SetStartType(uint startType);
            void Stop(int timeoutMilliseconds);
            void Start(int timeoutMilliseconds);
            void Delete();
        }

        internal interface IProcessLifetime : IDisposable
        {
            string ImagePath { get; }
            bool WaitForExit(int timeoutMilliseconds);
        }

        internal interface IProcessSource
        {
            uint[] FindNamedProcesses(string name, int maximumCount);
            IProcessLifetime Open(uint processId);
        }

        internal static void DrainExactImage(string executable, IProcessSource source, Func<int> remaining)
        {
            System.Collections.Generic.List<IProcessLifetime> matching = new System.Collections.Generic.List<IProcessLifetime>();
            try
            {
                uint[] ids = source.FindNamedProcesses(Path.GetFileName(executable), MaximumProcessEnumeration);
                if (ids.Length > MaximumProcessEnumeration) throw new IOException("Too many processes to verify broker shutdown safely.");
                foreach (uint id in ids)
                {
                    if (remaining() == 0) throw new TimeoutException("The broker process inspection timed out; installation files were retained.");
                    IProcessLifetime candidate = source.Open(id);
                    if (candidate == null) continue; // The process exited before it could be pinned.
                    try
                    {
                        if (String.Equals(Path.GetFullPath(candidate.ImagePath), executable, StringComparison.OrdinalIgnoreCase))
                        { matching.Add(candidate); candidate = null; }
                    }
                    finally { if (candidate != null) candidate.Dispose(); }
                }
                foreach (IProcessLifetime process in matching)
                    if (!process.WaitForExit(remaining()))
                        throw new TimeoutException("A broker process did not exit; installation files were retained.");
            }
            finally { foreach (IProcessLifetime process in matching) process.Dispose(); }
        }

        internal MachineWindowsService(string root) : this(ValidateMachineRoot(root), new NativeBackend()) { }

        // This overload is for isolated tests; production always validates the protected fixed root.
        internal MachineWindowsService(string root, IBackend operations)
        {
            if (operations == null) throw new ArgumentNullException("operations");
            backend = operations;
            command = CommandFor(root);
            desired = Expected(root);
            try
            {
                Configuration found = backend.Read();
                if (found != null)
                {
                    Validate(found, command);
                    if (found.State != Running && found.State != Stopped)
                        throw new IOException("privateCloud's service is changing state. Retry after it has stopped or started.");
                    original = found.Copy();
                }
            }
            catch { backend.Dispose(); throw; }
        }

        static string ValidateMachineRoot(string root)
        {
            string expected = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "privateCloud");
            if (!Environment.Is64BitProcess || !String.Equals(Path.GetFullPath(root).TrimEnd('\\'), expected, StringComparison.OrdinalIgnoreCase))
                throw new IOException("The broker service requires the fixed 64-bit Program Files installation.");
            return expected;
        }

        internal static string CommandFor(string root)
        {
            return "\"" + Path.Combine(Path.GetFullPath(root), "privateCloudDirectBroker.exe") + "\" --service";
        }

        internal static bool ProcessIdIsValid(uint state)
        {
            // QueryServiceStatusEx does not promise a valid PID during start/stop transitions.
            return state == Running || state == 5 || state == 6 || state == 7;
        }

        internal static Configuration Expected(string root)
        {
            return new Configuration {
                Command = CommandFor(root), Account = "LocalSystem", Display = DisplayName,
                Type = OwnProcess, StartType = Automatic, ErrorControl = 1, SidType = 1,
                Group = "", Dependencies = "BFE", RebootMessage = "", FailureCommand = "",
                Privileges = new[] { "SeChangeNotifyPrivilege" }, ResetPeriod = 86400,
                Actions = new[] { new ActionEntry(1, 5000), new ActionEntry(1, 30000), new ActionEntry(0, 0) },
                FailureActionsOnNonCrash = true, State = Stopped
            };
        }

        internal static void Validate(Configuration value, string expectedCommand)
        {
            if (value == null || !String.Equals(value.Command, expectedCommand, StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(value.Account, "LocalSystem", StringComparison.OrdinalIgnoreCase) || value.Type != OwnProcess ||
                value.Display != DisplayName || value.ErrorControl != 1 || value.Tag != 0 ||
                !String.IsNullOrEmpty(value.Group) || value.Dependencies != "BFE" ||
                (value.StartType != Automatic && value.StartType != Manual && value.StartType != Disabled) ||
                value.DelayedStart || value.SidType != 1 || value.TriggerCount != 0 || value.LaunchProtected != 0 ||
                value.Privileges == null || value.Privileges.Length != 1 || value.Privileges[0] != "SeChangeNotifyPrivilege" ||
                !value.FailureActionsOnNonCrash || value.ResetPeriod != 86400 ||
                !String.IsNullOrEmpty(value.RebootMessage) || !String.IsNullOrEmpty(value.FailureCommand) ||
                value.Actions == null || value.Actions.Length != 3 ||
                value.Actions[0].Type != 1 || value.Actions[0].Delay != 5000 ||
                value.Actions[1].Type != 1 || value.Actions[1].Delay != 30000 ||
                value.Actions[2].Type != 0 || value.Actions[2].Delay != 0)
                throw new IOException("The existing privateCloudDirectBroker service has an unexpected configuration. It will not be replaced.");
        }

        Configuration CheckCurrent(bool allowPartiallyCreated)
        {
            if (disposed) throw new ObjectDisposedException("MachineWindowsService");
            Configuration current = backend.Read();
            if (current == null)
            {
                if (original != null || created) throw new IOException("The broker service disappeared during maintenance.");
                return null;
            }
            if (original == null && !created) throw new IOException("Another broker service appeared during installation.");
            // Partial optional configuration must not prevent cleanup, but the fixed process
            // identity is required even for the SCM object whose handle we retained at creation.
            if (!String.Equals(current.Command, command, StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(current.Account, "LocalSystem", StringComparison.OrdinalIgnoreCase) || current.Type != OwnProcess)
                throw new IOException("The broker service changed process identity during maintenance.");
            if (!allowPartiallyCreated || !created) Validate(current, command);
            return current;
        }

        internal void StopForChange()
        {
            if (CheckCurrent(false) == null) return;
            changed = true;
            backend.SetStartType(Disabled);
            backend.Stop(TransitionTimeoutMilliseconds);
        }

        internal void Activate()
        {
            Configuration current = CheckCurrent(false);
            changed = true;
            if (current == null)
            {
                backend.CreateDisabled(desired);
                created = true;
            }
            else
            {
                backend.SetStartType(Disabled);
                backend.Stop(TransitionTimeoutMilliseconds);
            }
            // Do not enable automatic starts until every privilege/recovery setting is installed.
            Configuration disabled = desired.Copy(); disabled.StartType = Disabled;
            backend.Apply(disabled);
            Configuration checkedConfiguration = CheckCurrent(false);
            if (checkedConfiguration.State != Stopped || checkedConfiguration.StartType != Disabled)
                throw new IOException("The broker service was not safely stopped before activation.");
            backend.SetStartType(Automatic);
            backend.Start(TransitionTimeoutMilliseconds);
            Configuration active = CheckCurrent(false);
            if (active.State != Running || active.StartType != Automatic)
                throw new IOException("The broker service did not reach the required running state.");
        }

        internal void StopFailedActivation()
        {
            if (!changed) return;
            if (CheckCurrent(true) == null) return;
            backend.SetStartType(Disabled);
            backend.Stop(TransitionTimeoutMilliseconds);
            if (created) { backend.Delete(); created = false; }
        }

        internal void Restore()
        {
            if (!changed) return;
            if (original == null)
            {
                StopFailedActivation();
                changed = false;
                return;
            }
            CheckCurrent(false);
            backend.SetStartType(Disabled);
            backend.Stop(TransitionTimeoutMilliseconds);
            Configuration disabled = original.Copy(); disabled.StartType = Disabled;
            backend.Apply(disabled);
            if (original.State == Running)
            {
                // A running service may originally have been disabled administratively.
                backend.SetStartType(Manual);
                backend.Start(TransitionTimeoutMilliseconds);
            }
            backend.SetStartType(original.StartType);
            Configuration restored = CheckCurrent(false);
            if (restored.StartType != original.StartType || restored.State != original.State)
                throw new IOException("The original broker service state could not be restored.");
            changed = false;
        }

        internal void Remove()
        {
            if (CheckCurrent(false) == null) return;
            changed = true;
            backend.SetStartType(Disabled);
            backend.Stop(TransitionTimeoutMilliseconds);
            backend.Delete();
            created = false;
            changed = false;
        }

        public void Dispose()
        {
            if (!disposed) { disposed = true; backend.Dispose(); }
        }

        internal sealed class NativeBackend : IBackend
        {
            const uint QueryRights = 0x1 | 0x4;
            const uint ChangeRights = QueryRights | 0x2 | 0x10 | 0x20 | 0x10000;
            const int DoesNotExist = 1060, InsufficientBuffer = 122, NotActive = 1062, AlreadyRunning = 1056;
            ScHandle manager, service;
            SafeWaitHandle process;
            uint processId;
            bool writable;

            internal NativeBackend()
            {
                manager = OpenSCManagerW(null, null, 1);
                if (manager.IsInvalid) { manager.Dispose(); throw Error("Open service manager"); }
                service = OpenServiceW(manager, ServiceName, QueryRights);
                if (service.IsInvalid)
                {
                    int error = Marshal.GetLastWin32Error(); service.Dispose(); service = null;
                    if (error != DoesNotExist) { manager.Dispose(); throw Error("Open broker service", error); }
                }
            }

            static Win32Exception Error(string operation) { return Error(operation, Marshal.GetLastWin32Error()); }
            static Win32Exception Error(string operation, int error) { return new Win32Exception(error, operation + " failed (" + error + ")."); }
            static void Check(bool success, string operation) { if (!success) throw Error(operation); }

            void Writable()
            {
                if (writable) return;
                if (service == null) throw new IOException("The broker service is missing.");
                ScHandle next = OpenServiceW(manager, ServiceName, ChangeRights);
                if (next.IsInvalid) { next.Dispose(); throw Error("Open broker service for maintenance"); }
                service.Dispose(); service = next; writable = true;
            }

            IntPtr Query(uint level)
            {
                uint needed;
                bool result = level == 0 ? QueryServiceConfigW(service, IntPtr.Zero, 0, out needed) : QueryServiceConfig2W(service, level, IntPtr.Zero, 0, out needed);
                int error = Marshal.GetLastWin32Error();
                if (result || error != InsufficientBuffer || needed == 0 || needed > 65536)
                    throw Error("Read broker service configuration size", error);
                IntPtr memory = Marshal.AllocHGlobal((int)needed);
                try
                {
                    Check(level == 0 ? QueryServiceConfigW(service, memory, needed, out needed) : QueryServiceConfig2W(service, level, memory, needed, out needed), "Read broker service configuration");
                    return memory;
                }
                catch { Marshal.FreeHGlobal(memory); throw; }
            }

            uint QueryNumber(uint level)
            {
                IntPtr memory = Query(level);
                try { return unchecked((uint)Marshal.ReadInt32(memory)); }
                finally { Marshal.FreeHGlobal(memory); }
            }

            static string Text(IntPtr pointer) { return pointer == IntPtr.Zero ? "" : Marshal.PtrToStringUni(pointer); }
            static string[] MultiString(IntPtr pointer)
            {
                System.Collections.Generic.List<string> values = new System.Collections.Generic.List<string>();
                if (pointer != IntPtr.Zero)
                    for (int offset = 0; ; )
                    {
                        string value = Text(IntPtr.Add(pointer, offset));
                        if (value.Length == 0) break;
                        values.Add(value); offset += checked((value.Length + 1) * 2);
                        if (offset > 65536) throw new IOException("Invalid service multi-string.");
                    }
                return values.ToArray();
            }

            public Configuration Read()
            {
                if (service == null)
                {
                    // A fresh installer must notice another installer creating this fixed name.
                    ScHandle found = OpenServiceW(manager, ServiceName, QueryRights);
                    if (found.IsInvalid)
                    {
                        int error = Marshal.GetLastWin32Error(); found.Dispose();
                        if (error == DoesNotExist) return null;
                        throw Error("Check broker service existence", error);
                    }
                    service = found;
                }
                Configuration value = new Configuration();
                IntPtr memory = Query(0);
                try
                {
                    QueryConfiguration data = (QueryConfiguration)Marshal.PtrToStructure(memory, typeof(QueryConfiguration));
                    value.Command = Text(data.BinaryPath); value.Account = Text(data.Account); value.Display = Text(data.Display);
                    value.Type = data.Type; value.StartType = data.StartType; value.ErrorControl = data.ErrorControl;
                    value.Group = Text(data.Group); value.Tag = data.Tag;
                    value.Dependencies = String.Join("\0", MultiString(data.Dependencies));
                }
                finally { Marshal.FreeHGlobal(memory); }
                memory = Query(2);
                try
                {
                    FailureActions data = (FailureActions)Marshal.PtrToStructure(memory, typeof(FailureActions));
                    if (data.Count > 16 || (data.Count != 0 && data.Actions == IntPtr.Zero)) throw new IOException("Unexpected broker recovery actions.");
                    value.ResetPeriod = data.Reset; value.RebootMessage = Text(data.RebootMessage); value.FailureCommand = Text(data.Command);
                    value.Actions = new ActionEntry[data.Count];
                    for (int i = 0; i < value.Actions.Length; ++i)
                    {
                        NativeAction action = (NativeAction)Marshal.PtrToStructure(IntPtr.Add(data.Actions, i * Marshal.SizeOf(typeof(NativeAction))), typeof(NativeAction));
                        value.Actions[i] = new ActionEntry(action.Type, action.Delay);
                    }
                }
                finally { Marshal.FreeHGlobal(memory); }
                memory = Query(6);
                try { value.Privileges = MultiString(Marshal.ReadIntPtr(memory)); }
                finally { Marshal.FreeHGlobal(memory); }
                value.DelayedStart = QueryNumber(3) != 0; value.FailureActionsOnNonCrash = QueryNumber(4) != 0;
                value.SidType = QueryNumber(5); value.TriggerCount = QueryNumber(8); value.LaunchProtected = QueryNumber(12);
                ServiceStatusProcess status = Status();
                TrackProcess(status);
                value.State = status.State;
                return value;
            }

            public void CreateDisabled(Configuration configuration)
            {
                if (service != null) throw new IOException("A broker service already exists.");
                using (ScHandle createManager = OpenSCManagerW(null, null, 2))
                {
                    if (createManager.IsInvalid) throw Error("Open service manager for creation");
                    ScHandle next = CreateServiceW(createManager, ServiceName, DisplayName, ChangeRights, OwnProcess, Disabled, 1,
                        configuration.Command, null, IntPtr.Zero, "BFE\0\0", "LocalSystem", null);
                    if (next.IsInvalid) { next.Dispose(); throw Error("Create broker service"); }
                    service = next; writable = true;
                }
            }

            void ChangeObject(uint level, object value)
            {
                IntPtr memory = Marshal.AllocHGlobal(Marshal.SizeOf(value));
                try { Marshal.StructureToPtr(value, memory, false); Check(ChangeServiceConfig2W(service, level, memory), "Set broker service configuration " + level); }
                finally { Marshal.FreeHGlobal(memory); }
            }

            public void Apply(Configuration configuration)
            {
                Writable();
                Check(ChangeServiceConfigW(service, configuration.Type, configuration.StartType, configuration.ErrorControl,
                    configuration.Command, "", IntPtr.Zero, configuration.Dependencies + "\0\0", configuration.Account, null, configuration.Display), "Set broker service configuration");
                ChangeObject(3, new Number { Value = configuration.DelayedStart ? 1U : 0U });
                ChangeObject(5, new Number { Value = configuration.SidType });
                IntPtr privileges = Marshal.StringToHGlobalUni(String.Join("\0", configuration.Privileges) + "\0\0");
                try { ChangeObject(6, new Pointer { Value = privileges }); }
                finally { Marshal.FreeHGlobal(privileges); }
                IntPtr actions = Marshal.AllocHGlobal(configuration.Actions.Length * Marshal.SizeOf(typeof(NativeAction)));
                IntPtr empty = Marshal.StringToHGlobalUni("");
                try
                {
                    for (int i = 0; i < configuration.Actions.Length; ++i)
                        Marshal.StructureToPtr(new NativeAction { Type = configuration.Actions[i].Type, Delay = configuration.Actions[i].Delay },
                            IntPtr.Add(actions, i * Marshal.SizeOf(typeof(NativeAction))), false);
                    ChangeObject(2, new FailureActions { Reset = configuration.ResetPeriod, RebootMessage = empty, Command = empty,
                        Count = (uint)configuration.Actions.Length, Actions = actions });
                }
                finally { Marshal.FreeHGlobal(actions); Marshal.FreeHGlobal(empty); }
                ChangeObject(4, new Number { Value = configuration.FailureActionsOnNonCrash ? 1U : 0U });
            }

            public void SetStartType(uint startType)
            {
                Writable();
                Check(ChangeServiceConfigW(service, UInt32.MaxValue, startType, UInt32.MaxValue, null, null, IntPtr.Zero, null, null, null, null), "Change broker service start type");
            }

            ServiceStatusProcess Status()
            {
                ServiceStatusProcess status; uint needed;
                Check(QueryServiceStatusEx(service, 0, out status, (uint)Marshal.SizeOf(typeof(ServiceStatusProcess)), out needed), "Read broker service state");
                return status;
            }

            void TrackProcess(ServiceStatusProcess status)
            {
                if (!ProcessIdIsValid(status.State) || status.ProcessId == 0) return;
                if (process != null)
                {
                    uint previous = WaitForSingleObject(process, 0);
                    if (previous == 258 && processId == status.ProcessId) return;
                    if (previous != 0) throw new IOException("The previous broker process has not exited.");
                    process.Dispose(); process = null;
                }
                SafeWaitHandle next = OpenProcess(0x00100000, false, status.ProcessId);
                if (next.IsInvalid)
                {
                    int error = Marshal.GetLastWin32Error(); next.Dispose();
                    if (error == 87 && Status().State == Stopped) return;
                    throw Error("Pin broker process for shutdown", error);
                }
                process = next; processId = status.ProcessId;
            }

            static int Remaining(Stopwatch timer, int timeout)
            {
                return (int)Math.Max(0L, timeout - timer.ElapsedMilliseconds);
            }

            public void Stop(int timeoutMilliseconds)
            {
                Writable();
                Stopwatch timer = Stopwatch.StartNew();
                ServiceStatusProcess status = Status();
                // Keep the handle across Read/Start/Stop: SCM may clear its PID when the broker
                // reports STOPPED, just before the own-process executable actually exits.
                TrackProcess(status);
                bool requested = false;
                for (;;)
                {
                    status = Status();
                    TrackProcess(status);
                    if (status.State == Stopped) break;
                    if (!requested && status.State != 2 && status.State != 3)
                    {
                        ServiceStatus ignored;
                        if (!ControlService(service, 1, out ignored))
                        {
                            int error = Marshal.GetLastWin32Error();
                            if (error != NotActive) throw Error("Stop broker service", error);
                        }
                        requested = true;
                    }
                    if (Remaining(timer, timeoutMilliseconds) == 0) throw new TimeoutException("The broker service did not stop; its installation files were retained.");
                    Thread.Sleep(Math.Min(100, Remaining(timer, timeoutMilliseconds)));
                }
                if (process != null)
                {
                    uint wait = WaitForSingleObject(process, (uint)Remaining(timer, timeoutMilliseconds));
                    if (wait == 258) throw new TimeoutException("The broker process did not exit; its installation files were retained.");
                    if (wait != 0) throw Error("Wait for broker process exit");
                }
                // Failed startup may never expose a valid SCM PID. Pin every remaining instance
                // of this fixed protected image after STOPPED, without controlling any process.
                string executable = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "privateCloud", "privateCloudDirectBroker.exe");
                DrainExactImage(executable, new NativeProcessSource(), delegate { return Remaining(timer, timeoutMilliseconds); });
            }

            public void Start(int timeoutMilliseconds)
            {
                Writable();
                Stopwatch timer = Stopwatch.StartNew();
                if (!StartServiceW(service, 0, IntPtr.Zero))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != AlreadyRunning) throw Error("Start broker service", error);
                }
                for (;;)
                {
                    ServiceStatusProcess status = Status();
                    TrackProcess(status);
                    if (status.State == Running) return;
                    if (status.State == Stopped) throw Error("Broker service stopped during startup", (int)status.Win32ExitCode);
                    if (Remaining(timer, timeoutMilliseconds) == 0) throw new TimeoutException("The broker service did not start within 30 seconds.");
                    Thread.Sleep(Math.Min(100, Remaining(timer, timeoutMilliseconds)));
                }
            }

            public void Delete()
            {
                Writable();
                Check(DeleteService(service), "Delete broker service");
                service.Dispose(); service = null; writable = false;
            }

            public void Dispose()
            {
                if (service != null) { service.Dispose(); service = null; }
                if (manager != null) { manager.Dispose(); manager = null; }
                if (process != null) { process.Dispose(); process = null; }
            }

            internal sealed class ScHandle : SafeHandleZeroOrMinusOneIsInvalid
            {
                public ScHandle() : base(true) { }
                protected override bool ReleaseHandle() { return CloseServiceHandle(handle); }
            }

            sealed class NativeProcessSource : IProcessSource
            {
                public uint[] FindNamedProcesses(string name, int maximumCount)
                {
                    System.Collections.Generic.List<uint> ids = new System.Collections.Generic.List<uint>();
                    using (SafeFileHandle snapshot = CreateToolhelp32Snapshot(2, 0))
                    {
                        if (snapshot.IsInvalid) throw Error("Read process snapshot for broker shutdown");
                        ProcessEntry entry = new ProcessEntry(); entry.Size = (uint)Marshal.SizeOf(typeof(ProcessEntry));
                        bool found = Process32FirstW(snapshot, ref entry);
                        int count = 0;
                        while (found)
                        {
                            if (++count > maximumCount) throw new IOException("Too many processes to verify broker shutdown safely.");
                            if (String.Equals(entry.Executable, name, StringComparison.OrdinalIgnoreCase)) ids.Add(entry.ProcessId);
                            entry.Size = (uint)Marshal.SizeOf(typeof(ProcessEntry));
                            found = Process32NextW(snapshot, ref entry);
                        }
                        int error = Marshal.GetLastWin32Error();
                        if (error != 18) throw Error("Enumerate processes for broker shutdown", error);
                    }
                    return ids.ToArray();
                }

                public IProcessLifetime Open(uint processId)
                {
                    SafeWaitHandle handle = OpenProcess(0x00101000, false, processId);
                    if (handle.IsInvalid)
                    {
                        int error = Marshal.GetLastWin32Error(); handle.Dispose();
                        if (error == 87) return null;
                        throw Error("Identify a possible broker process", error);
                    }
                    try
                    {
                        StringBuilder path = new StringBuilder(32768); uint length = (uint)path.Capacity;
                        if (!QueryFullProcessImageNameW(handle, 0, path, ref length))
                        {
                            int error = Marshal.GetLastWin32Error();
                            if (WaitForSingleObject(handle, 0) == 0) return null;
                            throw Error("Read possible broker process image", error);
                        }
                        NativeProcessLifetime result = new NativeProcessLifetime(handle, path.ToString());
                        handle = null;
                        return result;
                    }
                    finally { if (handle != null) handle.Dispose(); }
                }
            }

            sealed class NativeProcessLifetime : IProcessLifetime
            {
                readonly SafeWaitHandle handle;
                readonly string path;
                internal NativeProcessLifetime(SafeWaitHandle process, string image) { handle = process; path = image; }
                public string ImagePath { get { return path; } }
                public bool WaitForExit(int timeoutMilliseconds)
                {
                    uint result = WaitForSingleObject(handle, (uint)timeoutMilliseconds);
                    if (result == 0) return true;
                    if (result == 258) return false;
                    throw Error("Wait for possible broker process exit");
                }
                public void Dispose() { handle.Dispose(); }
            }

            [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct ProcessEntry
            {
                internal uint Size, Usage, ProcessId;
                internal UIntPtr DefaultHeap;
                internal uint ModuleId, Threads, ParentProcessId;
                internal int Priority;
                internal uint Flags;
                [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] internal string Executable;
            }
            [StructLayout(LayoutKind.Sequential)] struct Number { internal uint Value; }
            [StructLayout(LayoutKind.Sequential)] struct Pointer { internal IntPtr Value; }
            [StructLayout(LayoutKind.Sequential)] struct QueryConfiguration
            {
                internal uint Type, StartType, ErrorControl;
                internal IntPtr BinaryPath, Group;
                internal uint Tag;
                internal IntPtr Dependencies, Account, Display;
            }
            [StructLayout(LayoutKind.Sequential)] struct FailureActions
            {
                internal uint Reset;
                internal IntPtr RebootMessage, Command;
                internal uint Count;
                internal IntPtr Actions;
            }
            [StructLayout(LayoutKind.Sequential)] struct NativeAction { internal uint Type, Delay; }
            [StructLayout(LayoutKind.Sequential)] struct ServiceStatus
            {
                internal uint Type, State, ControlsAccepted, Win32ExitCode, SpecificExitCode, CheckPoint, WaitHint;
            }
            [StructLayout(LayoutKind.Sequential)] struct ServiceStatusProcess
            {
                internal uint Type, State, ControlsAccepted, Win32ExitCode, SpecificExitCode, CheckPoint, WaitHint, ProcessId, Flags;
            }
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern ScHandle OpenSCManagerW(string machine, string database, uint access);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern ScHandle OpenServiceW(ScHandle manager, string name, uint access);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern ScHandle CreateServiceW(ScHandle manager, string name, string display, uint access, uint type, uint start, uint error, string binary, string group, IntPtr tag, string dependencies, string account, string password);
            [DllImport("advapi32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool CloseServiceHandle(IntPtr handle);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool QueryServiceConfigW(ScHandle service, IntPtr config, uint size, out uint needed);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool QueryServiceConfig2W(ScHandle service, uint level, IntPtr config, uint size, out uint needed);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ChangeServiceConfigW(ScHandle service, uint type, uint start, uint error, string binary, string group, IntPtr tag, string dependencies, string account, string password, string display);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ChangeServiceConfig2W(ScHandle service, uint level, IntPtr data);
            [DllImport("advapi32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool QueryServiceStatusEx(ScHandle service, uint level, out ServiceStatusProcess status, uint size, out uint needed);
            [DllImport("advapi32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ControlService(ScHandle service, uint control, out ServiceStatus status);
            [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool StartServiceW(ScHandle service, uint argc, IntPtr argv);
            [DllImport("advapi32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool DeleteService(ScHandle service);
            [DllImport("kernel32.dll", SetLastError = true)] static extern SafeWaitHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);
            [DllImport("kernel32.dll", SetLastError = true)] static extern uint WaitForSingleObject(SafeWaitHandle handle, uint milliseconds);
            [DllImport("kernel32.dll", SetLastError = true)] static extern SafeFileHandle CreateToolhelp32Snapshot(uint flags, uint processId);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool Process32FirstW(SafeFileHandle snapshot, ref ProcessEntry entry);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool Process32NextW(SafeFileHandle snapshot, ref ProcessEntry entry);
            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool QueryFullProcessImageNameW(SafeWaitHandle process, uint flags, StringBuilder path, ref uint size);
        }
    }
}
