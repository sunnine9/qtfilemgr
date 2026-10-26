# Architecture and integration contracts

## Public build boundary

The standalone CMake target `QtFileMgr` exports `QtFileMgr::QtFileMgr` and links
Qt Core/Gui/Widgets. `QTFILEMGR_WITH_SSH=ON` adds the libssh implementation and
connection UI; OFF excludes those translation units and SSH-only installed
headers. Generic IO and protocol-provider APIs remain available. Build products
live in a separate build directory. CMake is the supported build entry.

The guarded undo/no-replace APIs add virtual methods and internal state. Rebuild
all hosts and custom IO plugins from source against the updated headers and
library; this early source candidate does not promise binary ABI compatibility
with previously built consumers.

## Main components

```text
Host application / standalone main
  ├─ QtFileMgrWorkspace: tabs, split panes, active pane, shared Places
  └─ QtFileMgrWidget: URL navigation, history, config and combined UI
       ├─ BreadcrumbBar / FilterBar / status bar
       ├─ PlacesPanel / preview and dialogs
       └─ QtFileMgrFileView: interaction, selection, operation scheduling
            ├─ FlatModel: directory snapshot, filter and sorting
            ├─ ItemDelegate: icon/compact/details rendering
            └─ QtFileMgrIO
                 ├─ LocalIO
                 ├─ optional SSHIO facade → worker → libssh
                 └─ caller-supplied backend
```

Embed `QtFileMgrWidget` for one file panel. Use `QtFileMgrWorkspace` when the
host wants the complete tab/split-pane shell. Business meaning and file opening
remain in host callbacks; the component does not assume a document editor or
application service.

## URLs and storage identity

The widget's external location is a `QUrl`. IO methods consume backend-specific
paths. Implement `acceptsUrl()`, `urlForPath()` and `pathFromUrl()` consistently;
remote identity includes scheme, host, user and port where applicable. A bare
path is not enough to identify a remote session. Use widget `setUrl()`/`setIO()`
as integration entry points so navigation history and child controls agree.

`allowedPaths` is a widget navigation policy, not a filesystem sandbox. Enforce
real authorization in the backend and operating system/service permissions.

## Requests, cancellation and ownership

The backend facade and views belong to the same GUI thread. Local work and SSH
calls use worker threads; request delivery must occur on the request object's
owning thread. Directory requests deliver batches and finish once; the model
cancels old requests and ignores stale generations when navigation changes.
Preview and transfer requests also provide cooperative cancellation. Completed
request objects schedule deletion; hold guarded references rather than keeping
raw pointers after completion.

A caller-supplied IO is not adopted by `setIO()`: parent it to a host object that
outlives consumers. Protocol-provider callbacks run on the GUI thread and use
`QtFileMgrConnectionResult::transferOwnership` to explicitly transfer a result.
Default compatibility adapters defer synchronous calls to the next event-loop
turn; implement real asynchronous methods for slow storage.

## Operations and recovery

FileView schedules ordinary copy/move jobs. Local jobs execute in worker threads;
remote jobs consume backend transfer requests. Download/relay, deletion, rename,
trash and undo also have their own workflows. This is not one global transaction
queue across all tabs and panes. Conflict, cancellation and recovery semantics
must be checked at each operation entry point.

Undo stores limited in-memory path records. It is not version history, and old
overwritten content is not retained for undo. Remote-to-remote relay downloads
to local staging before uploading, requiring local disk space and a live source
backend registered in the process.

See [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) for the precise boundaries of
overwrite commit, rollback, disconnect and partial failure. Tests are catalogued
in [TESTING.md](TESTING.md), with actual release evidence in
[VALIDATION.md](VALIDATION.md).

## Configuration and appearance

The standalone build uses standard user configuration directories and an
independent QSettings namespace. JSON controls places, view defaults, permissions
and visible UI parts; an explicit `QTFILEMGR_CONFIG_FILE` selects another file.
See the root README for configuration locations.

Visual colors are derived from the host palette. A small internal style helper
provides line icons and observes Qt palette updates; file and place icons come
from the system theme or Qt standard icons. No desktop icon theme is bundled.

## Guarded mutations

`QtFileMgrFileOperationJob::itemCompleted` supplies the execution-time undo proof.
Custom IO implementations opt into guarded undo with object identity/version
semantics; the base methods refuse it. `renameNoReplace` must enforce its contract
in the backend, without an exists-then-replacing-rename adapter. The operation job
uses this contract to back up/install/restore remote overwrite targets. See
[SAFETY.md](SAFETY.md) for limits, retained backups and manual recovery.
