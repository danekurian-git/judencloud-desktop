// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Text;

namespace PrivateCloud.Setup
{
    internal sealed class MachineInstallState
    {
        internal const string Marker = "privateCloud machine installation v1";
        internal const string RecordName = "machine-installation.txt";
        internal const string ManifestName = "package.sha256";
        internal string Version;
        internal string Previous;

        internal static MachineInstallState Read(MachineInstallPaths paths, string directory)
        {
            InstallState.Child(directory, RecordName);
            if (File.Exists(directory)) throw new IOException("The installation directory is a file.");
            if (!Directory.Exists(directory)) return null;
            string record = InstallState.Child(directory, RecordName);
            if (!File.Exists(record)) throw new IOException("The machine installation directory is not owned by privateCloud.");
            string[] lines = File.ReadAllLines(record);
            if (lines.Length != 3 || lines[0] != Marker || !InstallState.IsVersionName(lines[1]))
                throw new IOException("The machine installation record is damaged or belongs to another installer.");
            if (lines[2].Length != 0) paths.ResolveSibling(lines[2], "previous");
            return new MachineInstallState { Version = lines[1], Previous = lines[2] };
        }

        internal void Write(MachineInstallPaths paths, string directory)
        {
            if (!InstallState.IsVersionName(Version)) throw new IOException("Invalid machine installation version.");
            if (!String.IsNullOrEmpty(Previous)) paths.ResolveSibling(Previous, "previous");
            InstallState.AtomicWrite(InstallState.Child(directory, RecordName),
                Encoding.UTF8.GetBytes(Marker + "\n" + Version + "\n" + (Previous ?? "") + "\n"));
        }

        internal static Package ReadPackage(string directory)
        {
            return new Package(File.ReadAllText(InstallState.Child(directory, ManifestName)));
        }

        internal static void CheckPackage(Package package)
        {
            if (!package.Files.ContainsKey("privateCloud.exe") || !package.Files.ContainsKey("privateCloudDirectBroker.exe"))
                throw new IOException("The machine package requires both privateCloud.exe and privateCloudDirectBroker.exe at its root.");
            if (package.Files.ContainsKey(RecordName) || package.Files.ContainsKey(ManifestName) || package.Files.ContainsKey("installation.txt"))
                throw new IOException("The machine package contains installer-owned metadata.");
        }
    }
}
