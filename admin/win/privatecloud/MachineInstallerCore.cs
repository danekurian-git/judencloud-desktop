// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace PrivateCloud.Setup
{
    internal static class MachineInstallerCore
    {
        // All platform changes are injected. Creation must apply the protected DACL
        // atomically; verification must reject untrusted owners, writable ACLs and
        // reparse points. No executable is written before that verification succeeds.
        // The caller holds a machine-wide maintenance lock and snapshots integration.
        internal static void Install(MachineInstallPaths paths, string version, Package package, Stream archive,
            Action<string> createProtectedDirectory, Action<string> verifyProtectedDirectory,
            Action stopForChange, Action activateIntegration, Action stopFailedActivation,
            Action restoreIntegration, Action<string> progress)
        {
            if (!InstallState.IsVersionName(version)) throw new IOException("Invalid machine installation version.");
            if (createProtectedDirectory == null || verifyProtectedDirectory == null || stopForChange == null ||
                activateIntegration == null || stopFailedActivation == null || restoreIntegration == null || progress == null)
                throw new ArgumentNullException("Machine installation integration callbacks are required.");
            paths.CheckRootPath();
            MachineInstallState.CheckPackage(package);
            MachineInstallState old = MachineInstallState.Read(paths, paths.Root);
            if (old != null)
            {
                verifyProtectedDirectory(paths.Root);
                Package previousPackage = MachineInstallState.ReadPackage(paths.Root);
                MachineInstallState.CheckPackage(previousPackage);
                previousPackage.Verify(paths.Root);
                if (old.Version == version)
                {
                    if (previousPackage.Manifest != package.Manifest)
                        throw new IOException("This version is already installed with a different payload. Use a new version identifier.");
                    Reconfigure(stopForChange, activateIntegration, stopFailedActivation, restoreIntegration);
                    return;
                }
            }

            string id = Guid.NewGuid().ToString("N");
            string staged = paths.Sibling("staging", id);
            string previous = paths.Sibling("previous", id);
            bool stageAdmitted = false, stopAttempted = false, oldMoved = false, promoted = false, activationAttempted = false;
            try
            {
                if (Directory.Exists(staged) || File.Exists(staged) || Directory.Exists(previous) || File.Exists(previous))
                    throw new IOException("A recovery directory already exists. Installation was not changed.");
                createProtectedDirectory(staged);
                verifyProtectedDirectory(staged);
                if (!Directory.Exists(staged) || Directory.GetFileSystemEntries(staged).Length != 0)
                    throw new IOException("The protected staging directory is not empty.");
                stageAdmitted = true;
                progress("Checking the application and connection helper files…");
                package.Extract(archive, staged);
                InstallState.AtomicWrite(InstallState.Child(staged, MachineInstallState.ManifestName), Encoding.UTF8.GetBytes(package.Manifest));
                new MachineInstallState { Version = version, Previous = old == null ? "" : Path.GetFileName(previous) }.Write(paths, staged);
                verifyProtectedDirectory(staged);
                stopAttempted = true;
                stopForChange();
                paths.CheckRootPath();
                if (old != null)
                {
                    verifyProtectedDirectory(paths.Root);
                    Directory.Move(paths.Root, previous);
                    oldMoved = true;
                }
                else if (Directory.Exists(paths.Root) || File.Exists(paths.Root))
                    throw new IOException("The installation destination appeared while setup was preparing files.");
                Directory.Move(staged, paths.Root);
                promoted = true;
                verifyProtectedDirectory(paths.Root);
                progress("Activating the protected privateCloud installation…");
                activationAttempted = true;
                activateIntegration();
                // Previous program roots remain verified recovery copies. The GUI
                // is launched later by the original unelevated bootstrap process.
            }
            catch (Exception installFailure)
            {
                List<Exception> failures = new List<Exception> { installFailure };
                bool safeToRestore = true;
                if (activationAttempted)
                {
                    try { stopFailedActivation(); }
                    catch (Exception failure) { failures.Add(failure); safeToRestore = false; }
                }
                bool pathsRestored = !promoted && !oldMoved;
                if (safeToRestore)
                {
                    try
                    {
                        if (promoted)
                        {
                            verifyProtectedDirectory(paths.Root);
                            Directory.Move(paths.Root, staged);
                            promoted = false;
                        }
                        if (oldMoved)
                        {
                            verifyProtectedDirectory(previous);
                            Directory.Move(previous, paths.Root);
                            oldMoved = false;
                        }
                        pathsRestored = true;
                    }
                    catch (Exception failure) { failures.Add(failure); }
                }
                if (safeToRestore && pathsRestored)
                {
                    if (stopAttempted)
                        try { restoreIntegration(); } catch (Exception failure) { failures.Add(failure); }
                    if (stageAdmitted && Directory.Exists(staged))
                    {
                        try { RemoveStage(staged, package, verifyProtectedDirectory); }
                        catch (Exception failure) { failures.Add(failure); }
                    }
                }
                if (failures.Count > 1)
                    throw new AggregateException("Installation failed and recovery was incomplete. Keep the current and previous program directories for repair: " +
                        paths.Root + "; " + previous + "; " + staged, failures);
                throw;
            }
        }

        internal static string OwnedPrevious(MachineInstallPaths paths, Action<string> verifyProtectedDirectory)
        {
            paths.CheckRootPath();
            verifyProtectedDirectory(paths.Root);
            MachineInstallState state = MachineInstallState.Read(paths, paths.Root);
            if (state == null) throw new IOException("The machine installation is missing.");
            if (String.IsNullOrEmpty(state.Previous)) return null;
            string previous = paths.ResolveSibling(state.Previous, "previous");
            verifyProtectedDirectory(previous);
            if (MachineInstallState.Read(paths, previous) == null)
                throw new IOException("The previous program installation is missing.");
            Package package = MachineInstallState.ReadPackage(previous);
            MachineInstallState.CheckPackage(package);
            package.Verify(previous);
            return previous;
        }

        internal static void Uninstall(MachineInstallPaths paths, Action<string> verifyProtectedDirectory, Action stopAndRemoveIntegration)
        {
            paths.CheckRootPath();
            verifyProtectedDirectory(paths.Root);
            if (MachineInstallState.Read(paths, paths.Root) == null)
                throw new IOException("The machine installation is missing.");
            Package package = MachineInstallState.ReadPackage(paths.Root);
            MachineInstallState.CheckPackage(package);
            // Missing entries can result from an interrupted uninstall. Verify
            // remaining files before deleting only the explicit manifest entries.
            foreach (KeyValuePair<string, string> file in package.Files)
            {
                string path = InstallState.Child(paths.Root, file.Key);
                if (!File.Exists(path)) continue;
                using (SHA256 sha = SHA256.Create())
                using (FileStream input = File.OpenRead(path))
                    if (BitConverter.ToString(sha.ComputeHash(input)).Replace("-", "").ToLowerInvariant() != file.Value)
                        throw new IOException("An installed program file has changed: " + file.Key);
            }
            stopAndRemoveIntegration();
            verifyProtectedDirectory(paths.Root);
            package.RemoveOwnedFiles(paths.Root);
            File.Delete(InstallState.Child(paths.Root, MachineInstallState.ManifestName));
            File.Delete(InstallState.Child(paths.Root, MachineInstallState.RecordName));
            if (Directory.GetFileSystemEntries(paths.Root).Length == 0) Directory.Delete(paths.Root);
            // Previous program roots and every per-user profile/cache are retained.
        }

        static void Reconfigure(Action stop, Action activate, Action stopFailed, Action restore)
        {
            bool activating = false;
            try { stop(); activating = true; activate(); }
            catch (Exception failure)
            {
                List<Exception> failures = new List<Exception> { failure };
                bool safeToRestore = true;
                if (activating)
                    try { stopFailed(); } catch (Exception stopFailure) { failures.Add(stopFailure); safeToRestore = false; }
                if (safeToRestore)
                    try { restore(); } catch (Exception restoreFailure) { failures.Add(restoreFailure); }
                if (failures.Count > 1) throw new AggregateException("Machine installation integration recovery was incomplete.", failures);
                throw;
            }
        }

        static void RemoveStage(string directory, Package package, Action<string> verifyProtectedDirectory)
        {
            verifyProtectedDirectory(directory);
            package.RemoveOwnedFiles(directory);
            foreach (string name in new[] { MachineInstallState.ManifestName, MachineInstallState.RecordName })
            {
                string file = InstallState.Child(directory, name);
                if (File.Exists(file)) File.Delete(file);
            }
            if (Directory.GetFileSystemEntries(directory).Length == 0) Directory.Delete(directory);
        }
    }
}
