// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;

namespace PrivateCloud.Setup
{
    internal static class LauncherTestApplication
    {
        static int Main(string[] args)
        {
            if (args.Length != 3 || args[1] != "--record") return 1;
            File.WriteAllLines(args[2], new[] { Environment.GetEnvironmentVariable("PRIVATECLOUD_MANAGED_STARTUP"), args[0], Environment.CurrentDirectory });
            return 0;
        }
    }
}
