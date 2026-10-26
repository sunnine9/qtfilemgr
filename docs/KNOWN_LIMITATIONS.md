# Known limitations

This document describes the current implementation and the intent of the tests.
It is not a claim that every scenario has passed on every release. Record the
environment, results and any skipped cases in the [release checklist](RELEASE_CHECKLIST.md).

## File operations and cancellation

| Area | Implemented behavior | Boundary to understand |
| --- | --- | --- |
| Local copy | Chunked transfer with cancellation and byte progress; rejects copying a directory into itself; removes a partial destination only when its recorded filesystem identity still matches. | Cancellation is cooperative. Cleanup can fail, and successful earlier items in a batch remain. This is not a batch transaction. |
| Local move | Tries a direct rename; on Unix, a failed rename falls back to cancellable copy followed by source removal. | Moving across filesystems is not atomic. Source removal can partially succeed. On failure the complete destination is retained and its path is reported for manual recovery. |
| Local overwrite | Copies to a sibling staging path, renames the old target to a backup, then installs the replacement. It attempts rollback on cancellation or commit failure. | A crash, storage error or concurrent filesystem change can interrupt rollback. A diagnostic may identify retained `.qtfilemgr-*` backup or replacement paths that need manual inspection. There is no crash-recovery journal. |
| Remote copy/upload | Transfers through SFTP with progress and cancellation checks. Regular-file creation rejects an existing remote target; partial-file removal is attempted on failure. | Blocking network calls cannot be interrupted immediately. Cleanup may fail after disconnection. Recursive operations can leave completed children and directories behind. |
| Remote overwrite job | Transfers to staging, backs up the old target, then installs with no-replace renames; a failed install attempts one safe restore. | This is not a crash-atomic transaction. Failed or uncertain calls preserve recovery paths. SSH retains the old backup after successful commit because it lacks a strong revision proof for cleanup. Other protocols must implement the no-replace contract. |

Cancel means “stop further work as soon as the implementation can cooperate.” It
does not mean that previously completed items are reversed. A cancellation flag
can arrive after a rename or transfer has already completed, so inspect the
reported success counts and actual source/destination state before retrying.

Filesystem metadata preservation is limited to what the implementation explicitly
copies, including ordinary permission bits and regular-file modification time.
Do not assume archival preservation of ownership, ACLs, extended attributes,
hard-link relationships or sparse-file layout. Remote copy on one server streams
data through the client connection; it is not a server-side clone operation.

Evidence: [`QtFileMgrLocalIO.cpp`](../src/QtFileMgrLocalIO.cpp)
(`copyCancellable`, `moveCancellable`, `cleanPartialTransferIfOwned`),
[`QtFileMgrFileOperationJob.cpp`](../src/QtFileMgrFileOperationJob.cpp)
(`replaceLocalItem`, `startRemoteNext`) and
[`QtFileMgrSSHIO.cpp`](../src/QtFileMgrSSHIO.cpp)
(`transfer`, `uploadRecursive`, `downloadRecursive`, `copyRemoteRecursive`).

## Undo is guarded, limited recovery

- The file view keeps at most 20 operation records in memory, with explicit
  per-item completion state for retries. Changing backend clears this history.
- Copy and move undo validate an execution-time identity/revision proof. A
  replaced file, an edited original, changed directory descendants, a new object
  at the original move source, or an unverifiable backend causes safe refusal.
- The Linux local implementation uses complete SHA-256/metadata manifests and
  no-replace quarantine/restore. It is bounded to 8 MiB total contents, 4096
  nodes, depth 64 and a 50 ms soft budget per scan pass. Unsupported, changing,
  unreadable or over-limit items are excluded from undo with a UI warning and
  path-specific diagnostics; the transfer itself is still successful.
- Custom backends must implement the guarded undo contract. Default IO methods
  refuse it. The built-in SSH backend does not provide automatic undo because
  SFTP metadata alone is not a strong object revision.
- A guarded move back across filesystems is refused (`EXDEV`); the reverse
  operation does not fall back to copy/delete.
- Copy undo excludes overwritten targets; it does not restore their old
  contents. A move undo does not restore a target that had been overwritten.
- Partial/skipped/cancelled move batches and cross-backend import moves have no
  matching automatic move-undo record. Permanent deletion has no operation undo.
- Validation/quarantine is not an exclusive cross-process transaction. A writer
  holding an open descriptor may still mutate data after a scan. Slow storage
  calls are not subject to a hard 50 ms timeout. A failed restore reports any
  retained quarantine path for manual inspection.

See [SAFETY.md](SAFETY.md) for the precise contracts and recovery process.

## SSH connection and credential boundaries

The SSH backend checks known hosts, requests a decision for an unknown key,
rejects a changed known key, and rejects a key that changes between confirmation
and the next connection attempt. A fatal transport error marks the backend
disconnected and invalidates caches; later disconnected requests return an
explicit error. This does not provide automatic reconnect, resumable transfers
or exactly-once remote mutations. After a broken connection the server may have
completed an operation whose response the client never received. Reconnect,
refresh and inspect both endpoints before retrying a destructive operation.

The public backend object belongs on the GUI thread. It serializes libssh work
on an internal worker. Some synchronous compatibility operations, explicit
disconnect and destruction wait for the worker; shutdown can wait on a slow or
stalled server. Request cancellation is cooperative rather than a guarantee of
an immediate shutdown deadline.

“Remember password” stores a machine-derived XOR/Base64 **obfuscation** in
QSettings and attempts to restrict the settings file to owner access. It is not
an operating-system keychain, encryption or authenticated secret storage. A
reader with access to the settings and machine identity can recover the saved
password. The format marker detects ordinary corruption, not malicious
tampering. Clearing selected QString buffers does not guarantee that every
temporary copy of a secret is erased. Hosts requiring stronger protection should
avoid saved passwords or integrate their own secret store. Never put credentials
in connection URLs, screenshots or issue reports.

Evidence: [`QtFileMgrSSHIO.cpp`](../src/QtFileMgrSSHIO.cpp)
(`openSession`, `noteSessionDead`, `disconnectFromHost`, destructor),
[`QtFileMgrConfigStore.cpp`](../src/QtFileMgrConfigStore.cpp)
(`sshCredentialKey`, `obfuscateCredential`, `saveSshCredential`) and
[`QtFileMgrProtocolProvider.h`](../include/QtFileMgrProtocolProvider.h).

## Navigation limits are not a sandbox

`allowedPaths`, `rootPath` and the create/delete/rename/drag flags guide the
component's navigation and available UI commands. They are not an operating
system security boundary. `allowedPaths` is checked by the widget navigation
layer; direct IO calls and other host code are outside that check. Enforce
authorization in the backend and use operating-system or service permissions
when untrusted input or restricted data is involved.

Evidence: [`QtFileMgrWidget.cpp`](../src/QtFileMgrWidget.cpp)
(`isUrlAllowed`, `setUrl`, `applyConfig`) and the independently callable
[`QtFileMgrIO` interface](../include/QtFileMgrIO.h).

## Remote clipboard and drag-and-drop

Remote URL transfer depends on a live source backend registered in the same
process that accepts the URL. An arbitrary remote URL copied from another
application does not establish a connection by itself. A source backend must
advertise `LocalExportCapability` to support download or cross-node relay.

Two remote nodes relay one item at a time through a local temporary directory:
download the complete item, then upload it to the destination. This needs local
space for a complete file or directory, performs two network transfers, and
temporarily writes remote data to local storage. It is not a direct server-to-
server transfer or a resumable stream. Conflict policy and cleanup in these
serial paths should not be assumed to match every policy of the normal operation
job. A cross-backend cut deletes sources only after the batch succeeds; failures
can leave destination copies while retaining sources. There is no atomic
cross-server move or undo.

Evidence: [`QtFileMgrFileView.cpp`](../src/QtFileMgrFileView.cpp)
(`remoteBackendForUrl`, `pasteFiles`, drop handling, `startSerialDownload`,
`startSerialRelay`) and [`QtFileMgrIO.h`](../include/QtFileMgrIO.h)
(`exportToLocalRequest`, `importFromLocalRequest`).

## Host and custom-backend lifetime contract

- `setIO()` does not adopt a caller-supplied backend. Keep it alive while its
  consumers and asynchronous work use it, normally by parenting it to the host.
  Backend and view must have the same QObject thread affinity. A destroyed
  backend triggers a warning and local-backend fallback; this is recovery from
  an unexpected lifetime change, not a substitute for an explicit ownership plan.
- A protocol provider and its completion callbacks run on the GUI thread. Keep
  the provider alive through outstanding callbacks and honor
  `QtFileMgrConnectionResult::transferOwnership` when returning an IO object.
- Directory, preview and transfer request objects are QObjects, support
  cooperative cancellation and schedule their own deletion after completion.
  Deliver results on their owning thread, finish once, and guard outstanding
  references. Do not keep raw request pointers after completion.
- If using `QtFileMgrFileView` directly, its `setConfig()` retains a pointer;
  the configuration must outlive the view. Injected context-menu actions and
  objects captured by host callbacks must also remain valid while in use.
- Default “async” compatibility adapters defer synchronous work to the next
  event-loop turn. A slow custom backend must implement actual asynchronous
  requests rather than claim nonblocking capabilities on these adapters.

Evidence: [`QtFileMgrFileView.h`](../include/QtFileMgrFileView.h),
[`QtFileMgrFileView.cpp`](../src/QtFileMgrFileView.cpp) (`setIO`),
[`QtFileMgrIO.cpp`](../src/QtFileMgrIO.cpp) and
[`QtFileMgrProtocolProvider.h`](../include/QtFileMgrProtocolProvider.h).

## Validation scope

Linux is the current validation baseline. Windows, macOS, other desktop styles
and filesystems need their own build and behavioral checks. In particular,
identity-based partial-copy cleanup uses Unix device/inode APIs; the non-Unix
fallback cannot provide the same cleanup behavior. Cross-filesystem testing
currently depends on an available second filesystem and can be skipped when its
fixture cannot be created. A passing test summary must retain that skip detail.

[`test_localio_safety.cpp`](../tests/test_localio_safety.cpp) covers local
cancellation, overwrite staging, cross-filesystem move recovery, undo behavior,
backend lifetime changes and simulated remote transfer routing. Simulated
backends establish routing and state behavior, not real network reliability.

[`test_ssh_e2e.cpp`](../tests/test_ssh_e2e.cpp) provides an isolated real SSH/SFTP
fixture. Its upload and upload-cancellation cases use the explicit
`importFromLocalRequest()` API; `copyRequest()` covers same-node remote copy and
`exportToLocalRequest()` covers download. It also includes import-move source
removal and explicit disconnect during an export while preserving an existing
local target. These are test cases to run for the release candidate, not a claim
of a successful run. Production authentication methods, changed-host-key
scenarios, abrupt network loss, remote recursive failures, cross-server
relay, full disks and shutdown under network stall still require targeted
verification. Screenshot generation is useful for visual review but is not an
assertion that every interaction or layout is correct.
