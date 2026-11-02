// SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors
// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Drawing;
using System.Threading;
using System.Windows.Forms;

namespace PrivateCloud.Setup
{
    internal sealed class SetupProgressWindow : Form
    {
        const int MaximumDetailLength = 16384;
        readonly Func<Action<string>, int> work;
        readonly Func<int, string> completionText;
        readonly Label status;
        readonly TextBox details;
        readonly TextBox log;
        readonly ProgressBar progress;
        readonly Button close;
        bool started;
        bool complete;
        int result = 1;

        internal SetupProgressWindow(Func<Action<string>, int> action, Func<int, string> completedText, string logPath)
        {
            if (action == null) throw new ArgumentNullException("action");
            if (completedText == null) throw new ArgumentNullException("completedText");
            work = action;
            completionText = completedText;
            Text = "privateCloud setup";
            Font = new Font("Segoe UI", 10F);
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(620, 360);
            MinimumSize = new Size(560, 350);
            StartPosition = FormStartPosition.CenterScreen;
            MaximizeBox = false;
            SizeGripStyle = SizeGripStyle.Hide;
            AccessibleName = "privateCloud setup";

            TableLayoutPanel layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(22), ColumnCount = 1, RowCount = 6 };
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
            layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
            layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
            status = new Label { Text = "Preparing privateCloud setup…", Dock = DockStyle.Fill, AutoEllipsis = true,
                Font = new Font(Font.FontFamily, 14F, FontStyle.Bold), AccessibleName = "Setup status" };
            progress = new ProgressBar { Dock = DockStyle.Fill, Style = ProgressBarStyle.Marquee, MarqueeAnimationSpeed = 30,
                Margin = new Padding(3, 0, 3, 10), AccessibleName = "Setup in progress" };
            details = new TextBox { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, BorderStyle = BorderStyle.None,
                BackColor = SystemColors.Control, ScrollBars = ScrollBars.Vertical, TabStop = true,
                Text = "Please keep this window open while setup runs.", AccessibleName = "Setup details" };
            Label logLabel = new Label { Text = "Setup log (select the path to copy)", Dock = DockStyle.Fill, TextAlign = ContentAlignment.BottomLeft };
            log = new TextBox { ReadOnly = true, Dock = DockStyle.Fill, Text = logPath ?? "A setup log could not be created. Copy the details above.", AccessibleName = "Setup log path" };
            FlowLayoutPanel buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.RightToLeft,
                Padding = new Padding(0, 10, 0, 0) };
            close = new Button { Text = "Close", Enabled = false, AutoSize = true, MinimumSize = new Size(95, 30), AccessibleName = "Close setup" };
            close.Click += delegate { Close(); };
            buttons.Controls.Add(close);
            layout.Controls.Add(status, 0, 0); layout.Controls.Add(progress, 0, 1); layout.Controls.Add(details, 0, 2);
            layout.Controls.Add(logLabel, 0, 3); layout.Controls.Add(log, 0, 4); layout.Controls.Add(buttons, 0, 5);
            Controls.Add(layout);
            AcceptButton = close;
            CancelButton = close;
        }

        internal static int Run(Func<Action<string>, int> work, Func<int, string> completionText, string logPath)
        {
            if (Thread.CurrentThread.GetApartmentState() != ApartmentState.STA)
                throw new InvalidOperationException("The setup window must run on the original STA thread.");
            Application.EnableVisualStyles();
            using (SetupProgressWindow window = new SetupProgressWindow(work, completionText, logPath))
            {
                Application.Run(window);
                return window.ResultCode;
            }
        }

        internal bool WorkComplete { get { return complete; } }
        internal bool CloseEnabled { get { return close.Enabled; } }
        internal int ResultCode { get { return result; } }
        internal string StatusText { get { return status.Text; } }
        internal string DetailText { get { return details.Text; } }
        internal string LogPathText { get { return log.Text; } }

        protected override void OnShown(EventArgs args)
        {
            base.OnShown(args);
            // Paint meaningful content before queuing the installer worker.
            Refresh();
            BeginInvoke(new Action(StartWork));
        }

        void StartWork()
        {
            if (started || IsDisposed) return;
            started = true;
            Thread worker = new Thread(delegate()
            {
                int exitCode;
                string failureDetail = null;
                try { exitCode = work(ReportProgress); }
                catch (Exception failure) { exitCode = 1; failureDetail = Limit(failure.ToString()); }
                Post(delegate { Complete(exitCode, failureDetail); });
            });
            worker.IsBackground = true;
            worker.Name = "privateCloud setup worker";
            worker.SetApartmentState(ApartmentState.STA);
            try { worker.Start(); }
            catch (Exception failure) { Complete(1, Limit(failure.ToString())); }
        }

        static string Limit(string value)
        {
            value = value ?? "";
            return value.Length <= MaximumDetailLength ? value : value.Substring(0, MaximumDetailLength) + "\r\n[Details truncated]";
        }

        void Post(Action action)
        {
            if (IsDisposed || Disposing || !IsHandleCreated) return;
            try { BeginInvoke(action); }
            catch (InvalidOperationException) { } // A late reporter may arrive after the completed window closed.
        }

        void ReportProgress(string message)
        {
            string text = Limit(message);
            Post(delegate
            {
                if (complete || String.IsNullOrWhiteSpace(text)) return;
                status.Text = text;
                status.AccessibleDescription = text;
            });
        }

        void Complete(int exitCode, string failureDetail)
        {
            if (complete) return;
            result = exitCode;
            complete = true;
            progress.MarqueeAnimationSpeed = 0;
            progress.Style = ProgressBarStyle.Blocks;
            progress.Value = exitCode == 0 ? 100 : 0;
            status.Text = exitCode == 0 ? "Setup complete" : "Setup could not finish";
            status.AccessibleDescription = status.Text;
            Text = exitCode == 0 ? "privateCloud setup — complete" : "privateCloud setup — needs attention";
            try { details.Text = Limit(failureDetail ?? completionText(exitCode)); }
            catch (Exception failure)
            {
                // A presentation failure must not replace the actual installer result.
                details.Text = Limit("Setup finished with result " + exitCode + ".\r\n" + failure.Message);
            }
            if (String.IsNullOrWhiteSpace(details.Text))
                details.Text = exitCode == 0 ? "privateCloud setup finished successfully." : "Read the setup log for details.";
            details.SelectionStart = 0;
            details.SelectionLength = 0;
            close.Enabled = true;
            close.Focus();
        }

        protected override void OnFormClosing(FormClosingEventArgs args)
        {
            if (!complete) args.Cancel = true;
            base.OnFormClosing(args);
            // Event subscribers cannot turn an active installation into a cancelled-looking exit.
            if (!complete) args.Cancel = true;
        }
    }
}
