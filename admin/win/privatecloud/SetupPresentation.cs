// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Linq;

namespace PrivateCloud.Setup
{
    internal static class SetupPresentation
    {
        internal static int Run(string[] args, Func<Action<string>, int> work,
            Func<Func<Action<string>, int>, int> present)
        {
            if (args.Any(arg => arg == "--silent" || arg == "--elevated-install" || arg == "--elevated-uninstall" ||
                arg == "--verify-package" || arg == "--configure-user" || arg == "--uninstall"))
                return work(delegate { });
            return present(work);
        }
    }
}
