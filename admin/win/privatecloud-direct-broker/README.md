# privateCloud direct-connection helper

The Windows helper provides a scoped physical-interface connection for the installed privateCloud client to `cloud.monocloud.online` on TCP port 443. It uses a protected LocalSystem service and authenticated named-pipe leases. Other applications retain their existing routing.

The exception is limited to the verified privateCloud executable, resolved server endpoint, physical source address/interface and HTTPS port. It can override lower-priority filtering rules for that connection. HTTPS certificate validation remains in the client. Tailscale configuration and global routes are not changed.

## Build and test

Run in an x64 MSVC developer shell, from the repository root. Run the tests without administrator elevation: the pipe ACL suite deliberately checks the rights of a standard interactive user. GitHub Actions runs that suite under a temporary standard account on its disposable runner.

```powershell
cmake -S admin/win/privatecloud-direct-broker -B build/direct-broker -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/direct-broker
ctest --test-dir build/direct-broker --output-on-failure
```

The eight suites cover broker input/protocol rules, policy recognition, fixed-server resolution, lease lifetime, dynamic policy ownership, process identity, named-pipe lifecycle and notification regression behavior. Test success does not establish packet-level bypass on a particular network; that requires an active exit-node integration test.

## Network changes

Route notifications are checked against the leased server destination. Interface notifications are limited to the relevant physical and tunnel interfaces. Firewall changes are checked for overlap with the leased connection and the policy objects on which it depends. Unrelated changes keep the lease alive. Relevant changes and loss of required security evidence revoke the lease; the broker records the revocation reason.

The helper removes its dynamic filtering objects when a lease ends or the owning session closes. The client renews leases and retries failed connections. The helper is packaged with the machine installer; a user-writable portable installation does not satisfy its protected-client contract.

See [service requirements](SERVICE-PREREQUISITES.md) and [installer tooling](../privatecloud/README.md).
