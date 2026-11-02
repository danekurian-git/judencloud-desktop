# Service and connection requirements

The machine installer provisions `privateCloudDirectBroker` as an own-process LocalSystem service and installs the GUI under `%ProgramFiles%\privateCloud`. Installation requires administrator approval. The broker verifies the executable location, ownership, access permissions, process identity and named-pipe peer before granting a lease. Reparse points and paths writable by ordinary users are rejected.

The client requests only the fixed server on HTTPS port 443. The broker validates the endpoint and selects a suitable physical network interface. When an exit node is active, the recognized Tailscale policy must satisfy the broker's checks before its scoped exception is installed. The exception can override lower-priority blocks for the same client connection; higher-precedence blocks and other filtering layers can still prevent connectivity.

Missing or unrecognized required policy evidence causes refusal. A lease belongs to its authenticated client and expires without renewal. Watchdog and pipe-lifecycle handling remove dynamic policy on disconnect, client termination or service shutdown.

The application package must include the generated runtime-requirements manifest and the broker executable. The machine package builder checks these requirements. The legacy per-user package builder refuses a payload that requires the protected service.

Local unit and regression tests do not replace testing installation, renewal, file transfer, route changes and cleanup with a real exit node. Keep TLS verification enabled during all integration tests.
