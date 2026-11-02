// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;

namespace PrivateCloud.Setup
{
    internal sealed class InstallerCore
    {
        // Integration is injected so failure/rollback tests never touch the user's registry or shortcuts.
        internal static void Install(string root, string version, Package package, Stream archive,
            byte[] launcher, Action activateIntegration, Action rollbackIntegration, Action<string> progress)
        {
            InstallState.CheckOwnedRoot(root);
            InstallState old = InstallState.Read(root);
            string destination = InstallState.VersionPath(root, version);
            bool reuse = Directory.Exists(destination);
            string staged = InstallState.Child(root, "versions/staging-" + Guid.NewGuid().ToString("N"));
            string launcherPath = InstallState.Child(root, "privateCloud.exe");
            byte[] oldLauncher = File.Exists(launcherPath) ? File.ReadAllBytes(launcherPath) : null;
            bool integrationAttempted = false;
            bool promoted = false;
            bool launcherChanged = false;
            try
            {
                // Claim only the already-checked empty or owned program directory.
                // This also makes an interrupted first installation safely retryable.
                Directory.CreateDirectory(root);
                File.WriteAllText(InstallState.Child(root, "installation.txt"), InstallState.Marker);
                progress("Checking the application files…");
                if (reuse)
                {
                    package.Verify(destination);
                    File.WriteAllText(InstallState.Child(destination, "package.sha256"), package.Manifest, Encoding.UTF8);
                }
                else
                {
                    Directory.CreateDirectory(staged);
                    package.Extract(archive, staged);
                    File.WriteAllText(InstallState.Child(staged, "package.sha256"), package.Manifest, Encoding.UTF8);
                    Directory.Move(staged, destination);
                    promoted = true;
                }
                InstallState.AtomicWrite(launcherPath, launcher);
                launcherChanged = true;
                progress("Finishing installation…");
                integrationAttempted = true;
                activateIntegration();
                // A single atomic file replacement activates the fully verified version.
                new InstallState { Current = version, Previous = old.Current == version ? old.Previous : old.Current }.Write(root);
            }
            catch (Exception installFailure)
            {
                List<Exception> failures = new List<Exception> { installFailure };
                Action<Action> repair = delegate(Action action)
                {
                    try { action(); } catch (Exception failure) { failures.Add(failure); }
                };
                if (integrationAttempted) repair(rollbackIntegration);
                repair(delegate {
                    if (!launcherChanged) return;
                    if (oldLauncher != null) InstallState.AtomicWrite(launcherPath, oldLauncher);
                    else if (File.Exists(launcherPath)) File.Delete(launcherPath);
                });
                repair(delegate {
                    string incomplete = promoted ? destination : staged;
                    if (Directory.Exists(incomplete))
                    {
                        package.RemoveOwnedFiles(incomplete);
                        string manifest = InstallState.Child(incomplete, "package.sha256");
                        if (File.Exists(manifest)) File.Delete(manifest);
                        if (Directory.GetFileSystemEntries(incomplete).Length == 0) Directory.Delete(incomplete);
                    }
                });
                if (failures.Count > 1) throw new AggregateException("Installation failed and some recovery steps also failed. The setup error log contains the details.", failures);
                throw;
            }
        }

        internal static void Rollback(string root)
        {
            InstallState.CheckOwnedRoot(root);
            InstallState state = InstallState.Read(root);
            string previous = InstallState.VersionPath(root, state.Previous);
            new Package(File.ReadAllText(InstallState.Child(previous, "package.sha256"))).Verify(previous);
            new InstallState { Current = state.Previous, Previous = state.Current }.Write(root);
        }

        internal static void UninstallFiles(string root)
        {
            InstallState.CheckOwnedRoot(root);
            string versions = InstallState.Child(root, "versions");
            foreach (string directory in Directory.GetDirectories(versions))
            {
                string version = Path.GetFileName(directory);
                if (!InstallState.IsVersionName(version) || (File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0) continue;
                InstallState.VersionPath(root, version);
                string manifest = InstallState.Child(directory, "package.sha256");
                if (!File.Exists(manifest)) continue;
                new Package(File.ReadAllText(manifest)).RemoveOwnedFiles(directory);
                File.Delete(manifest);
                if (Directory.GetFileSystemEntries(directory).Length == 0) Directory.Delete(directory);
            }
            foreach (string name in new[] { "privateCloud.exe", "active-version.txt", "installation.txt" })
            { string path = InstallState.Child(root, name); if (File.Exists(path)) File.Delete(path); }
            if (Directory.GetFileSystemEntries(versions).Length == 0) Directory.Delete(versions);
            if (Directory.GetFileSystemEntries(root).Length == 0) Directory.Delete(root);
        }
    }
}
