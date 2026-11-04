<!-- SPDX-FileCopyrightText: 2026 Nextcloud GmbH and Nextcloud contributors -->
<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# privateCloud for Windows

This preview is for 64-bit Windows 11. Run the `privateCloud-*-Machine-Setup.exe`
file and approve the Windows administrator prompt. Setup installs the application
under `C:\Program Files\privateCloud` and installs its connection helper service.

Sign in with your account for `https://cloud.monocloud.online`. Choose your local
sync folder; the default is `C:\PPCsyncConfig`. The folder picker supports suitable
internal NTFS drives. USB drives are not supported. The app presents your files
through **privateCloud (J:)** and downloads file contents when opened. J: must be
available. Only files accessible to your server account can be synchronized.

To reuse an existing Nextcloud folder, quit the other sync client, sign in to the
same server account, and select that folder. Keep the previous Nextcloud account
configuration on the PC so privateCloud can verify ownership. Setup displays
"Existing sync folder verified" when it recognizes a compatible folder. It keeps
the files, sync database, selective-sync choices and existing download preferences,
and backs up the previous configuration and database before connecting the drive.
The copied database must pass an integrity check before the folder is adopted;
damaged sync history is refused without resetting the original database.
Folders belonging to another account, missing their configuration or containing
ambiguous sync databases are refused. A fresh empty folder remains an option.

The connection helper supports privateCloud's fixed-server HTTPS connection when
a Tailscale exit node is enabled. Other applications retain their existing routing.

This installer is **unsigned**. Windows may display an unrecognized-publisher
warning or prevent it from running under stricter device policies. Signing and
antivirus detection are separate concerns; a signature would not guarantee that
security software accepts a release. Do not disable Windows security to install it.

For an update, quit privateCloud and run the new installer. Updates preserve account
settings and the selected sync folder. Uninstall through Windows Settings > Apps;
uninstalling does not delete the user's synchronized files or account configuration.

The matching `.sha256` file contains the installer checksum. To check a download:

```powershell
Get-FileHash -Algorithm SHA256 .\privateCloud-*-Machine-Setup.exe
```

Compare the complete hash with the published checksum. A checksum confirms file
integrity; it is not a publisher certificate.

Source code and build instructions:
https://github.com/danekurian-git/judencloud-desktop

This application is based on the Nextcloud desktop client. Original copyright and
license notices remain applicable and are included with the runtime. The server's
remote-wipe command integration is not implemented in this preview.
