# privateCloud Windows packaging

The machine installer deploys the application to `%ProgramFiles%\privateCloud` and provisions its protected direct-connection service. Administrator approval is required. The generated `privatecloud-runtime-requirements.json` must accompany the runtime; a helper-required package must also include `privateCloudDirectBroker.exe`.

From this directory, package an already staged runtime into a new output directory:

```powershell
python build-machine-installer.py --package <runtime-directory> --output <new-output-directory> --version <four-part-version> --icon <application-icon>
```

The builder verifies admitted payload bytes, generates the maintenance executable, and refuses to overwrite an existing output directory. Test binaries, test fixtures and private-key material are excluded from shipped packages. Certificates containing only public material are handled separately from private keys. The source tree includes disposable TLS fixtures for isolated tests; never copy the test directory into a runtime package.

The installer preserves the existing account configuration and selected sync folder during updates. Its maintenance and rollback behavior is covered by the installer suites. Do not delete a user's sync data as part of an ordinary update or uninstall.

## Tests

```powershell
python -B -m unittest discover -s . -p "test_*.py" -v
.\test-installer.ps1
.\test-machine-installer.ps1
.\test-machine-windows-integration.ps1
.\test-setup-bootstrap.ps1
.\test-setup-presentation.ps1
.\test-setup-progress.ps1
```

The PowerShell tests compile disposable test executables using the Windows .NET Framework compiler. Runtime-package admission checks can be enabled with `PRIVATECLOUD_PACKAGE_UNDER_TEST`. Tests for an external runtime-staging entry point require `PRIVATECLOUD_PACKAGE_SCRIPT_UNDER_TEST`; these are skipped when that entry point is not supplied.

`test-packaged-network.ps1` exercises an explicitly supplied staged runtime and Qt test dependencies. Inspect its parameters before use. Live Tailscale bypass and file hydration require a separate integration run against the configured server.

## Legacy per-user installer

`build-installer.py` retains the earlier versioned per-user installer for payloads that do not require the protected helper. It rejects helper-required packages. Do not remove or alter the runtime-requirements manifest to bypass this guard.

Signing is not configured by these scripts. Building or testing an installer does not establish publisher trust or Windows SmartScreen reputation.
