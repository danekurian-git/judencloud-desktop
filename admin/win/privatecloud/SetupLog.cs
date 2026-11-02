// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace PrivateCloud.Setup
{
    internal sealed class SetupLog : IDisposable
    {
        readonly StreamWriter writer;
        internal readonly string FilePath;

        SetupLog(string path, StreamWriter stream) { FilePath = path; writer = stream; }

        internal static SetupLog Create(bool elevated)
        {
            return TryCreate(delegate
            {
                return Open(elevated);
            });
        }

        internal static SetupLog TryCreate(Func<SetupLog> create)
        {
            try { return create(); }
            catch { return new SetupLog(null, null); }
        }

        static SetupLog Open(bool elevated)
        {
            string directory;
            if (elevated)
            {
                // Never let a privileged process follow a caller-supplied log path.
                string parent = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
                if ((File.GetAttributes(parent) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("The setup log parent redirects to another directory.");
                MachineWindowsIntegration.VerifySecurity(Directory.GetAccessControl(parent));
                directory = InstallState.Child(parent, "privateCloud.SetupLogs");
                if (!Directory.Exists(directory)) MachineWindowsIntegration.CreateProtectedDirectory(directory);
                MachineWindowsIntegration.VerifyProtectedDirectory(directory);
            }
            else
            {
                directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "privateCloud", "SetupLogs");
                Directory.CreateDirectory(directory);
            }
            return CreateInDirectory(directory);
        }

        internal static SetupLog CreateInDirectory(string directory)
        {
            string name = "setup-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" +
                Process.GetCurrentProcess().Id + "-" + Guid.NewGuid().ToString("N") + ".log";
            string path = Path.Combine(directory, name);
            StreamWriter stream = new StreamWriter(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read), new UTF8Encoding(false));
            stream.AutoFlush = true;
            return new SetupLog(path, stream);
        }

        internal void Write(string message)
        {
            if (writer == null) return;
            try { writer.WriteLine(DateTime.UtcNow.ToString("o") + " " + message); }
            catch { }
        }

        public void Dispose()
        {
            if (writer == null) return;
            try { writer.Dispose(); } catch { }
        }
    }
}
