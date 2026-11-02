// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Threading;

namespace PrivateCloud.Setup
{
    internal sealed class InstallLock : IDisposable
    {
        readonly Mutex mutex = new Mutex(false, "Local\\privateCloudSetup");
        bool owned;
        internal InstallLock()
        {
            try { owned = mutex.WaitOne(0); }
            catch (AbandonedMutexException) { owned = true; }
            if (!owned) { mutex.Dispose(); throw new IOException("privateCloud setup or maintenance is already running."); }
        }
        public void Dispose()
        {
            if (owned) { mutex.ReleaseMutex(); owned = false; mutex.Dispose(); }
        }
    }
}
