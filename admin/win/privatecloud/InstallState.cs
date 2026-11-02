// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace PrivateCloud.Setup
{
    internal sealed class InstallState
    {
        internal const string Marker = "privateCloud installation v1";
        internal string Current;
        internal string Previous;

        internal static string Child(string parent, string relative)
        {
            if (String.IsNullOrEmpty(relative) || Path.IsPathRooted(relative) || relative.Contains(":"))
                throw new IOException("Invalid package path.");
            string root = Path.GetFullPath(parent).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            string path = Path.GetFullPath(Path.Combine(root, relative));
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                throw new IOException("Package path escapes its directory.");
            for (string at = path; at != null; at = Path.GetDirectoryName(at))
                if ((File.Exists(at) || Directory.Exists(at)) && (File.GetAttributes(at) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("An installation path points to another location: " + at);
            return path;
        }

        internal static string VersionPath(string root, string version)
        {
            if (!IsVersionName(version))
                throw new IOException("Invalid installed version.");
            return Child(root, "versions/" + version);
        }

        internal static bool IsVersionName(string version)
        {
            return version != null && Regex.IsMatch(version, @"\A[0-9][A-Za-z0-9._-]{0,79}\z");
        }

        internal static InstallState Read(string root)
        {
            string path = Child(root, "active-version.txt");
            if (!File.Exists(path)) return new InstallState();
            string[] lines = File.ReadAllLines(path);
            if (lines.Length != 2) throw new IOException("The installation record is damaged.");
            VersionPath(root, lines[0]);
            if (lines[1].Length != 0) VersionPath(root, lines[1]);
            return new InstallState { Current = lines[0], Previous = lines[1] };
        }

        internal void Write(string root)
        {
            VersionPath(root, Current);
            if (!String.IsNullOrEmpty(Previous)) VersionPath(root, Previous);
            AtomicWrite(Child(root, "active-version.txt"), Encoding.UTF8.GetBytes(Current + "\n" + (Previous ?? "") + "\n"));
        }

        internal static void AtomicWrite(string path, byte[] bytes)
        {
            string pending = path + ".pending-" + Guid.NewGuid().ToString("N");
            try
            {
                using (FileStream stream = new FileStream(pending, FileMode.CreateNew, FileAccess.Write))
                { stream.Write(bytes, 0, bytes.Length); stream.Flush(true); }
                if (File.Exists(path)) File.Replace(pending, path, null);
                else File.Move(pending, path);
            }
            finally { if (File.Exists(pending)) File.Delete(pending); }
        }

        internal static void CheckOwnedRoot(string root)
        {
            string marker = Child(root, "installation.txt");
            if (Directory.Exists(root) && Directory.GetFileSystemEntries(root).Length != 0 &&
                (!File.Exists(marker) || File.ReadAllText(marker) != Marker))
                throw new IOException("The installation directory contains files that do not belong to privateCloud.");
        }
    }
}
