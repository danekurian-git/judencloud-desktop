# privateCloud Windows fork

privateCloud connects to `https://cloud.monocloud.online` and presents the account's permitted cloud files through the configured Windows drive letter, normally J:. File contents download on demand.

## Choosing a sync folder

After signing in, the setup screen offers a Browse button. The default is `C:\PPCsyncConfig`; users can choose another supported local folder, including a folder on D:. The selected directory is recorded for that account and retained across restarts. The drive letter is a view of this backing folder; choosing D: does not change the server address.

The client validates the chosen directory and its identity before adopting it. USB-attached storage is rejected even when Windows reports it as fixed storage. Server-side external mounts remain excluded. Access to remote files is governed by the authenticated account's server permissions.

Regression coverage includes the default location, picker selection/cancellation, account association, directory validation and the Browse/Connect drive setup page in `test/testaccountwizardcontroller.cpp` and `test/testprivateclouddrive.cpp`.

## Direct connection and managed state

The [Windows helper](../admin/win/privatecloud-direct-broker/README.md) provides the scoped exit-node exception. Its notification regression suite ensures unrelated route/interface/firewall changes do not revoke an otherwise valid connection lease.

`PrivateCloudManagedState` is retained as the remote-wipe foundation. It records a persistent account-specific lock and validates exact directory identities. A server/admin trigger and complete application orchestration are separate work; this class must not be removed as unused code.

See [Windows packaging](../admin/win/privatecloud/README.md) for installer requirements and tests. Upstream licensing and attribution remain intact.
