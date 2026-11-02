// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PrivateCloud.Setup
{
    internal static class SetupProgram
    {
        internal static Stream Resource(string name) { return Assembly.GetExecutingAssembly().GetManifestResourceStream(name); }
        internal static string TextResource(string name) { using (StreamReader reader = new StreamReader(Resource(name))) return reader.ReadToEnd(); }
        [STAThread]
        static int Main(string[] args)
        {
            bool silent = args.Contains("--silent");
            string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "privateCloud");
            try
            {
                using (InstallLock installLock = new InstallLock())
                {
                    if (Environment.OSVersion.Version < new Version(10, 0, 19041))
                        throw new IOException("privateCloud requires Windows 10 version 2004 or later.");
                    Package package = new Package(TextResource("package.sha256"));
                    // This read-only-to-profile path checks the actual embedded payload for packaging verification.
                    if (args.Length == 2 && args[0] == "--verify-package")
                    {
                        string verifyRoot = Path.GetFullPath(args[1]);
                        if (Directory.Exists(verifyRoot)) throw new IOException("The verification destination must not exist.");
                        Directory.CreateDirectory(verifyRoot);
                        using (Stream payload = Resource("payload.zip")) package.Extract(payload, verifyRoot);
                        File.WriteAllText(Path.Combine(verifyRoot, "verified.txt"), "Every embedded payload file passed SHA-256 verification.");
                        return 0;
                    }
                    byte[] launcher;
                    using (MemoryStream bytes = new MemoryStream()) { using (Stream stream = Resource("launcher.exe")) stream.CopyTo(bytes); launcher = bytes.ToArray(); }
                    string version = TextResource("version.txt").Trim();
                    Action<Action<string>> install = delegate(Action<string> progress)
                    {
                        WindowsIntegration.CloseApplication(root);
                        WindowsIntegration integration = new WindowsIntegration(root);
                        using (Stream payload = Resource("payload.zip"))
                            InstallerCore.Install(root, version, package, payload, launcher, delegate { integration.Activate(version); }, integration.Restore, progress);
                    };
                    if (silent) install(delegate { });
                    else RunProgress(install);
                    installLock.Dispose();
                    if (!args.Contains("--no-launch")) Process.Start(InstallState.Child(root, "privateCloud.exe"));
                    return 0;
                }
            }
            catch (Exception ex)
            {
                string log = Path.Combine(Path.GetTempPath(), "privateCloud-setup-error.txt");
                try { File.WriteAllText(log, ex.ToString()); } catch { }
                if (!silent) MessageBox.Show(ex.Message + "\n\nDetails: " + log, "privateCloud setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
        }

        static void RunProgress(Action<Action<string>> install)
        {
            Application.EnableVisualStyles();
            Exception failure = null;
            using (Form form = new Form { Text = "Install privateCloud", Width = 430, Height = 155, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false, StartPosition = FormStartPosition.CenterScreen })
            using (Label label = new Label { Left = 22, Top = 20, Width = 370, Text = "Preparing privateCloud…" })
            using (ProgressBar bar = new ProgressBar { Left = 22, Top = 55, Width = 370, Style = ProgressBarStyle.Marquee })
            {
                bool finished = false;
                form.Controls.Add(label); form.Controls.Add(bar);
                form.FormClosing += delegate(object sender, FormClosingEventArgs e) { e.Cancel = !finished; };
                form.Shown += delegate
                {
                    Task.Run(delegate
                    {
                        try { install(delegate(string message) { form.BeginInvoke(new Action(delegate { label.Text = message; })); }); }
                        catch (Exception ex) { failure = ex; }
                        form.BeginInvoke(new Action(delegate { finished = true; form.Close(); }));
                    });
                };
                Application.Run(form);
            }
            if (failure != null) throw failure;
        }
    }
}
