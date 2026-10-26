# Build and release validation

Run these commands from the source root using the public dependencies listed in
the README. Linux is the validation baseline; test as an ordinary user rather
than root.

## Local regression suite

```sh
cmake -S . -B build -G Ninja -DQTFILEMGR_QT_MAJOR=5 \
  -DQTFILEMGR_WITH_SSH=OFF -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
```

Repeat with Qt 6 and SSH ON/OFF. `qtfilemgr.localio_safety` wraps the executable
in `tests/run_isolated.py`, using disposable HOME, XDG config/data/cache/runtime,
JSON config and known-host paths. Run through CTest or the wrapper; do not run a
legacy test command against your normal settings directory.

The suite covers copy/move, subdirectory/symlink guards, cancellation and partial
cleanup, local overwrite staging and recovery, copy/move undo and retry,
selection/navigation, injected backends and asynchronous lifetimes. SSH builds
also exercise connection identity and explicit failure without contacting a
production server. Read the output for skipped second-filesystem fixtures;
`/dev/shm` must be available on a filesystem different from the temporary source.
A passing summary does not convert a skipped case into a verified case.

## Widget, workspace and SSH callback lifetimes

`qtfilemgr.widget_workspace_lifetime` exercises custom-side-panel menu toggles,
caller-owned remote IO shared by split panes and component-owned session
survival after closing its original pane. It is included with `BUILD_TESTING=ON`
in both SSH configurations.

SSH builds additionally run `qtfilemgr.ssh_provider_lifetime`: a modal host-key
confirmation is closed after its backend is destroyed, verifying safe return
and a contextual warning. This is a controlled GUI lifetime test, not a network
authentication test. The opt-in real SSH suite also completes an upload while a
`pathsChanged` slot deletes its request owner, checking that the transfer data
remains correct and result delivery does not dereference the destroyed request.

## Undo and remote-overwrite safety

Additional always-on regression suites:

- `qtfilemgr.undo_history` checks three-level file/directory copy/move history,
  strict external-change refusal, partial completion/retry and proof renewal
  only through a verified same-object move.
- `qtfilemgr.directory_lifetime` checks request destruction, synchronous and
  partial-synchronous result replay, navigation during model/batch/loading
  signals and destruction during notifications.
- `qtfilemgr.provider_replacement` checks pending-provider replacement,
  registration/destruction, stale completions and ownership, current duplicate
  coalescing and child-provider teardown.
- `qtfilemgr.serial_transfer` checks synchronous/asynchronous download and both
  relay legs, missing/destroyed requests or owners, cut-source preservation,
  cancellation between legs, overwrite-confirmation owner destruction, queue
  recovery and a 128-item synchronous batch without recursive request creation.

`qtfilemgr.undo_guard` and `qtfilemgr.remote_overwrite` are always included with
`BUILD_TESTING=ON`; they do not depend on enabling SSH or the examples.

The undo suite uses real temporary Linux files and checks normal undo, in-place
editing, identical-content replacement, directory descendant changes, symbolic
links, occupied original move sources, verified retry proofs, explicit partial
completion, invalid filename encodings, bounds and virtual-call reentry. It also
holds GUI delivery until the worker finishes, edits the destination, then checks
that undo still uses the execution-time proof.

The overwrite suite uses a deterministic backend with real node contents and
identities to inject backup/commit/restore/cleanup failures, competing targets,
lost replies with side effects, cancellation at each phase and object/request
lifecycle changes. It asserts that the job never calls unguarded deletion or a
replacing rename. Both tests run through the existing isolated runner.

SSH-enabled integration additionally checks the actual no-replace operation and
successful overwrite against a disposable sshd. Its failure-injection wrapper
delegates all transfers, backups and successful restoration to real SFTP, while
explicitly failing selected install/restore calls. It verifies original/new
contents and the recovery paths reported to the caller. These controlled failures
are distinct from abrupt transport loss or a stalled server, which remain
separate deferred tests. See [SAFETY.md](SAFETY.md).

## Real SSH/SFTP fixture

The integration test is opt-in and needs Python 3, OpenSSH server/client tools,
SFTP server and libnss-wrapper. On Debian/Ubuntu:

```sh
sudo apt-get install pkg-config libssh-dev openssh-server libnss-wrapper
cmake -S . -B build-ssh -G Ninja -DQTFILEMGR_QT_MAJOR=5 \
  -DQTFILEMGR_WITH_SSH=ON -DBUILD_TESTING=ON \
  -DQTFILEMGR_TEST_SSH_E2E=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-ssh --parallel 2
(cd build-ssh && ctest --output-on-failure)
```

Some minimal containers need `/run/sshd` created by their image setup before
running the unprivileged fixture. Missing tools, permissions or failed server
startup are errors, not successful skips. See [SSH_E2E.md](../tests/SSH_E2E.md)
for the runner's isolation and exact coverage. The server binds loopback only;
tests create disposable keys and mutate only fixture files.

## Installed-package consumer

Enable `-DQTFILEMGR_BUILD_EXAMPLES=ON` in the main build to add
`qtfilemgr.memory_io`, `qtfilemgr.example_embedded` and
`qtfilemgr.example_memory_io` to CTest. These check backend contracts and actual
window navigation/selection/preview, using disposable settings and sample data.
Each example also has an independent installed-package CMake build; see
[EXAMPLES.md](EXAMPLES.md) for commands. CI builds and runs both forms in every
Qt/SSH matrix combination.

```sh
cmake --install build --prefix "$PWD/install"
cmake -S tests/installed_consumer -B consumer-build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/install" -DQTFILEMGR_EXPECT_SSH=OFF
cmake --build consumer-build --parallel 2
python3 tests/run_isolated.py --binary consumer-build/qtfilemgr_installed_smoke
```

Use `QTFILEMGR_EXPECT_SSH=ON` for an SSH-enabled package. This separate CMake
project links only the installed target and tests its exported feature selection.
Static builds additionally need their dependency development packages when
linking a consumer. QtFileMgr's default resources must remain available there.

## Visual review

`QTFILEMGR_BUILD_VISUAL_TOOLS=ON` builds `qtfilemgr_grab_shot`. The screenshot
runner uses isolated sample data, accepts the built executable and stores images
in a caller-selected output directory.

```sh
cmake -S . -B build -DQTFILEMGR_BUILD_VISUAL_TOOLS=ON
cmake --build build --target qtfilemgr_grab_shot --parallel 2
python3 tests/run_visual_suite.py --binary build/tests/qtfilemgr_grab_shot \
  --output visual-results
```
 Its success means that the requested PNGs
were saved; inspect the images for layout, clipping and state feedback. See the
runner's `--help` for parameters. Visual review is separate from assertions in
the regression suite. The repository images are demonstration data, not a claim
of production service connectivity.

## CI and release evidence

[The workflow](../.github/workflows/ci.yml) runs Qt 5/6 × SSH ON/OFF, CTest and
installed-consumer checks on Ubuntu. The SSH ON jobs explicitly enable the real
fixture. GitHub execution requires pushing the repository; local results are
recorded separately in [VALIDATION.md](VALIDATION.md).

Before a source release, complete the [checklist](RELEASE_CHECKLIST.md) and
review [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md). Fault injection against
full disks, server stalls and abrupt transport loss is not implied by a normal
passing fixture. Preserve full logs and record explicit deferrals.
