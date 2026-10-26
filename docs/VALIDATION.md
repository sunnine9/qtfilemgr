# Validation record — 2026-10-26

Candidate: QtFileMgr 0.1.0 source candidate, including the visual refinements,
standalone CMake build, optional SSH implementation and two complete integration
examples, guarded undo, recoverable remote overwrite, P1 lifecycle fixes and the
five follow-up P2 sequencing/lifetime fixes. Tests were executed
before packaging; this record distinguishes local execution from pending
repository-hosted CI.

## Reproducible public-dependency environment

The main checks ran as an ordinary non-root user in a fresh Debian 12 container
with packages installed from the public distribution repositories. Only the
source tree and a disposable build directory were mounted. Build/test execution
used no external network; the SSH fixture used loopback inside that container.
No application-specific includes, libraries, configuration tree or environment
were required.

Toolchain: GCC 12.2, CMake 3.25.1, Python 3.11.2; Qt 5.15.8 / 6.4.2 and
libssh 0.10.6 (distribution packages). Shared libraries are the default.

| Configuration | Build | Regression / integration | Install + independent consumer |
| --- | --- | --- | --- |
| Qt 5.15.8, SSH OFF | Pass | 11 CTest tests, including all four P2 regression suites; 0 failures | Pass |
| Qt 5.15.8, SSH ON | Pass | 13 CTest tests, including all four P2 suites and 55 real SSH checks; 0 failures | Pass |
| Qt 6.4.2, SSH OFF | Pass | 11 CTest tests, including all four P2 regression suites; 0 failures | Pass |
| Qt 6.4.2, SSH ON | Pass | 13 CTest tests, including all four P2 suites and 55 real SSH checks; 0 failures | Pass |

All four builds enabled `QTFILEMGR_BUILD_EXAMPLES=ON`. Each example was then
rebuilt independently from the **installed example source**, using only
`find_package(QtFileMgr)` and the installation prefix; both installed consumers
passed their behavioral smoke checks in the earlier example validation. The
updated safety candidate again passed installation plus the independent package
consumer. The current candidate again passed the full CTest matrix and installed
consumer using the existing build directories. CTest now has eleven tests with
SSH OFF, thirteen with SSH ON. The custom-protocol regression uses `catalog:///`, independently
of the example's `memory:///` implementation, and exercises multi-level parent
navigation, root handling, address editing, Places and allowlist identity/bounds.

The memory backend tests cover strict URL/path handling, metadata, bounded text,
complete decodable images, cancellation, request/backend deletion and rejection
of mutations. The embedded window smoke check uses actual selection and Enter
events, checks all three host signals, UTF-8 text, the 64 KiB limit, binary/missing
file errors and recovery. The memory window verifies selection, displayed content
and navigation to the root and back. Both final example screenshots were reviewed;
label sizing and initial-root warning-banner issues found during review were
fixed, regression checked and recaptured.

Both OFF libraries were inspected for dynamic dependencies: neither contains a
libssh dependency, and their installation omits the SSH-only public headers.
The ON tests used explicit local import and export APIs and a disposable sshd.
They cover normal transfer, upload cancellation, import move, active-export
`disconnectFromHost()`, existing destination preservation, disconnected requests
and host-key handling described in [SSH_E2E.md](../tests/SSH_E2E.md). The new
checks execute no-replace rename and successful overwrite on real SFTP, then
inject install/restore failures while continuing to use real SFTP transfers,
backups and successful rollback. Original and replacement contents and reported
recovery paths are checked directly in the disposable server fixture.

The local suite exercised copy, overwrite staging/recovery, move, cross-filesystem
move, cancellation, undo/retry, navigation and async backend contracts. No
second-filesystem skip was reported in these runs. The fixtures used temporary
storage and `/dev/shm`; this does not certify every filesystem or permission mode.

Additional checks:

- Qt 5.12.9 / CMake 3.17.5 / GCC 8.3.1 in the existing Qt5 development container:
  standalone CMake build, 291 local regression checks, 60 guarded undo checks,
  44 overwrite fault checks, 161 memory IO checks, both example window checks
  and the previously verified installed consumer
  passed. This is a minimum-version compatibility check; the clean-environment
  claim above is based on Debian, not this existing development environment.
- Static QtFileMgr library with Qt 5.15.8 and SSH ON: installation and independent
  consumer passed during the preceding release-preparation work, including
  embedded default-resource availability and exported libssh link dependencies.
  This static combination was not repeated for the current safety change.
- Portable visual suite: 13 scenarios, 21 PNG captures saved, 0 failures.
  Representative default, details, icons, split panes, narrow layout, filtering,
  settings and rename images were visually inspected. A filter-bar sizing issue
  found during review was corrected and its screenshot rechecked. The repository
  images use disposable sample data and no production connections.

## Safety checks added on 2026-10-26

The 60-check undo suite exercises identity/content/tree changes, occupied original
move sources, failure and retry, explicit partial completion, filename encoding
collisions, unsupported backend refusal and guarded-call reentry. Its final
execution-time test holds GUI notification delivery until the local worker has
finished, modifies the destination, then verifies that the old proof still
refuses undo. This last test addition was rerun separately on all four Qt/SSH
combinations and Qt 5.12 after the full matrix passed.

The 44-check deterministic overwrite suite covers backup/install/restore/cleanup
failures, verified and unverifiable backups, concurrent target creation and backup
replacement, lost replies with server-side effects, cancellation at each phase,
move-source preservation and backend/job/request lifetimes. It asserts that no
unguarded remove or replacing rename is used.

The local proof is deliberately bounded (8 MiB / 4096 nodes / depth 64 and a
50 ms cooperative budget per manifest). The Linux implementation requires
no-replace rename support; cross-filesystem move undo is refused. Built-in SSH
operations lack a strong undo proof and preserve old overwrite backups after
successful commit. These are disclosed safety boundaries, not skipped tests.
See [SAFETY.md](SAFETY.md).

## P1 lifecycle checks added on 2026-10-26

Four reviewed P1 issues were fixed: a custom side panel no longer dereferences a
missing built-in preview; split panes preserve caller-supplied IO ownership;
SSH transfer completion rechecks the request after `pathsChanged`; host-key
confirmation rechecks its backend after the modal dialog returns. Skipped or
discarded fallback work records contextual warnings.

The new 67-check widget/workspace suite covers built-in and custom panels,
host-owned and parentless injected backends, caller-chosen pane parents,
provider ownership transfer and replacement, and closing either side of an
adopted split session before further navigation and splitting. The 12-check SSH
provider suite destroys a backend or provider while the confirmation dialog is
open, and checks the live-backend No path. These provider signals are synthetic
GUI lifecycle fixtures; they are not network authentication results.

The real loopback SSH suite now has 55 checks, including an upload whose
`pathsChanged` callback deletes its request owner. It checks that the process
survives, the request is gone, target bytes are correct and the local source is
preserved; an ordinary upload still completes once.

Qt 5.12.9 additionally passed all eight registered CTest tests with SSH ON and
real SSH E2E disabled. Its older CTest does not support `--test-dir`; the actual
test run entered the build directory first. The earlier zero-test invocation
was not counted as validation. The new tests are registered in CMake and will
run in the existing CI matrix; no remote CI run was triggered.

The five P2 issues identified by the follow-up review were subsequently fixed
and verified as described below. Global cross-tab backend lifetime management
was not expanded by the P1 fix.

## P2 sequencing and lifetime checks added on 2026-10-26

All five reviewed P2 issues are addressed. Serial download/relay requests are
consumed once for synchronous or asynchronous results; missing or prematurely
destroyed requests terminate with diagnostics and release the queue. Deferred
relay upload rechecks cancellation and owner/backend lifetimes. Download
overwrite confirmation also rechecks its progress owner after the modal loop.

Consecutive local undo uses complete execution-time source and destination
proofs. An older record is renewed only through a verified same-object move and
an exact match to its old proof and path; external changes remain refused.
Physically completed undo is not retried if a final proof cannot be established.
Directory requests use guarded references, fixed request URLs and generation
checks after reentrant notifications. Pending connections likewise belong to a
specific provider and attempt; obsolete results cannot clear current attempts.
Provider destruction callbacks are disconnected before Widget members are torn
down, including the default child SSH provider.

Four new suites passed on all four Qt/SSH combinations: `undo_history` has 85
checks, `directory_lifetime` 54, `provider_replacement` 24 and `serial_transfer`
138. They cover file/directory histories and external edits, partial retries,
synchronous/partial-synchronous listings, request/model deletion, navigation
during notifications, late provider replies and child teardown, both relay
legs, missing/dropped requests and owners, cancellation between legs, progress
destruction during overwrite confirmation and a 128-item synchronous batch.

Qt 5.12.9 passed all twelve registered tests with SSH ON and real SSH E2E off.
Four Linux matrix builds and independent installed consumers passed; remote CI,
static builds and other operating systems were not rerun. The extra lineage signal
and changed private object state require hosts/custom plugins to rebuild with
the updated headers and library. No virtual IO signature was changed in this
P2 update.

## Public source cleanup checked on 2026-10-26

The publication copy identifies its author as Spike and uses the MIT license.
Internal host configuration, private paths and host-only project files were
removed. The resulting standalone source again passed all four Qt 5.15.8 /
Qt 6.4.2 and SSH ON/OFF builds, the complete 11/13-test CTest matrix, installation
and an independent installed-package consumer. These checks used the same
public-dependency container and no external network described above.

## CI status

`.github/workflows/ci.yml` defines Ubuntu Qt 5/6 × SSH ON/OFF, real SSH tests for
ON builds, both example/backend checks and independent installed-package example
builds. This record captures local execution before initial repository
publication; it does not claim a remote CI result. Check the repository Actions
tab for the status of individual commits.

## Explicit deferrals

- Windows/macOS, other compilers and non-Linux filesystem behavior are unverified.
- Independent remote-server namespaces, production password/agent auth, abrupt
  network partitions, server stalls and full disks remain targeted follow-up work.
  Deterministic commit/restore fault injection is now covered; actual lost
  transport responses and production-server failures are not equivalent to these
  controlled injections. Explicit disconnect is not a network
  partition test.
- The source candidate does not provide crash-recovery journaling, secure OS
  keychain integration, atomic cross-server moves or restoration of overwritten
  old content through undo. See [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).
- This validation record covers source builds. It does not certify a tagged
  release or a binary distribution.
