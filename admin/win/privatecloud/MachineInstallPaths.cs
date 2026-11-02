// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Text.RegularExpressions;

namespace PrivateCloud.Setup
{
    internal sealed class MachineInstallPaths
    {
        internal readonly string Root;
        internal readonly string Parent;
        readonly string name;

        MachineInstallPaths(string root)
        {
            Root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar);
            Parent = Path.GetDirectoryName(Root);
            name = Path.GetFileName(Root);
            if (String.IsNullOrEmpty(Parent) || String.IsNullOrEmpty(name))
                throw new IOException("Invalid machine installation directory.");
            InstallState.Child(Parent, name);
        }

        internal static MachineInstallPaths ForMachine()
        {
            if (!Environment.Is64BitProcess)
                throw new IOException("The machine installer must run as a 64-bit process.");
            return new MachineInstallPaths(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "privateCloud"));
        }

        // The setup entry point uses ForMachine; isolated tests supply an empty sandbox.
        internal static MachineInstallPaths ForTestRoot(string root) { return new MachineInstallPaths(root); }

        internal string Sibling(string kind, string id)
        {
            if ((kind != "staging" && kind != "previous") || !Regex.IsMatch(id ?? "", @"\A[0-9a-f]{32}\z"))
                throw new IOException("Invalid machine installation recovery directory.");
            return ResolveSibling(name + "." + kind + "-" + id, kind);
        }

        internal string ResolveSibling(string leaf, string kind)
        {
            if ((kind != "staging" && kind != "previous") ||
                !Regex.IsMatch(leaf ?? "", @"\A" + Regex.Escape(name + "." + kind + "-") + @"[0-9a-f]{32}\z"))
                throw new IOException("Invalid machine installation recovery directory.");
            return InstallState.Child(Parent, leaf);
        }

        internal void CheckRootPath() { InstallState.Child(Parent, name); }
    }
}
