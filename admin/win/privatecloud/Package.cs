// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.RegularExpressions;

namespace PrivateCloud.Setup
{
    internal sealed class Package
    {
        internal readonly Dictionary<string, string> Files = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        internal readonly string Manifest;
        internal Package(string manifest)
        {
            Manifest = manifest;
            using (StringReader reader = new StringReader(manifest))
            {
                string line;
                while ((line = reader.ReadLine()) != null)
                {
                    if (line.Length < 66 || line[64] != '\t' || !Regex.IsMatch(line.Substring(0, 64), "\\A[0-9a-f]{64}\\z"))
                        throw new IOException("Invalid package manifest.");
                    string path = line.Substring(65);
                    InstallState.Child(Path.GetTempPath(), path);
                    if (path.Split('/', '\\').Length == 0 || path.Contains("..") || Files.ContainsKey(path))
                        throw new IOException("Duplicate or invalid package entry.");
                    Files.Add(path, line.Substring(0, 64));
                }
            }
            if (!Files.ContainsKey("privateCloud.exe")) throw new IOException("The package has no application.");
        }

        internal void Extract(Stream stream, string destination)
        {
            HashSet<string> extracted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (ZipArchive archive = new ZipArchive(stream, ZipArchiveMode.Read, true))
            {
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    if (!Files.ContainsKey(entry.FullName) || !extracted.Add(entry.FullName))
                        throw new IOException("An unexpected file is present in the package.");
                    string path = InstallState.Child(destination, entry.FullName);
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    using (Stream input = entry.Open())
                    using (FileStream output = new FileStream(path, FileMode.CreateNew, FileAccess.Write))
                        input.CopyTo(output);
                }
            }
            Verify(destination);
            if (extracted.Count != Files.Count) throw new IOException("The package is incomplete.");
        }

        internal void Verify(string root)
        {
            foreach (KeyValuePair<string, string> file in Files)
            {
                string path = InstallState.Child(root, file.Key);
                using (SHA256 sha = SHA256.Create())
                using (FileStream input = File.OpenRead(path))
                    if (BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "").ToLowerInvariant() != file.Value)
                        throw new IOException("Package verification failed: " + file.Key);
            }
        }

        internal void RemoveOwnedFiles(string root)
        {
            // Exact manifest entries only. Never recursively remove a user-selected directory.
            foreach (string relative in Files.Keys)
            {
                string path = InstallState.Child(root, relative);
                if (File.Exists(path)) File.Delete(path);
                string parent = Path.GetDirectoryName(path);
                while (!String.Equals(parent, Path.GetFullPath(root), StringComparison.OrdinalIgnoreCase) &&
                       Directory.Exists(parent) && Directory.GetFileSystemEntries(parent).Length == 0)
                { Directory.Delete(parent); parent = Path.GetDirectoryName(parent); }
            }
        }
    }
}
