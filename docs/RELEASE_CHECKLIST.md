# Release checklist

Use this for a public source release. Check a box only when there is evidence for
the exact release candidate. Record an intentional deferral and link to the
corresponding [known limitation](KNOWN_LIMITATIONS.md) rather than marking an
unrun check as passed.

Release candidate: ______  Date: ______  Reviewer: ______

Environment: OS/filesystem ______  Compiler ______  Qt ______  libssh ______

## Packaging and first use

- [ ] Build from a clean public checkout using only the documented public
  dependencies. Verify source-tree builds are not required and generated files,
  local credentials, user settings and private fixtures are excluded.
- [ ] Build the standalone application and the embedded-host and read-only
  custom-IO examples. Follow the README in a clean shell without private
  environment variables or developer checkout paths.
- [ ] Verify supported build options, including disabled optional features when
  offered, install/export behavior and linking a separate consumer application.
- [ ] Include the MIT license and correct copyright holder/year. Record that
  Dolphin/KIO were design references; check the actual provenance and license of
  every bundled code fragment, icon and other asset. Document the licenses and
  distribution requirements of dependencies separately.
- [ ] Review public README, API docs, example comments, scripts, logs and images
  for private project names, absolute developer paths, passwords and hostnames.
- [ ] State the OS/Qt combinations actually validated. Keep Windows, macOS or
  other untested combinations labeled as unverified.

## Automated local checks

- [ ] Build and run [`test_localio_safety.cpp`](../tests/test_localio_safety.cpp)
  as an unprivileged user. Save the full output, return code and skip list.
- [ ] Ensure the cross-filesystem fixture really uses a different filesystem.
  If unavailable, record the skip and run the move scenarios on a second mount
  before claiming cross-filesystem validation.
- [ ] Confirm mid-file cancellation removes its own partial target and preserves
  the source; pre-start cancellation performs no transfer. Check the existing
  racing-destination cases as well as directory cancellation.
- [ ] Confirm local overwrite replaces via staging, source-removal failure
  preserves a complete recovery copy, and diagnostics identify retained paths.
- [ ] Confirm copy undo, mixed overwrite-copy undo, failed move-undo retry and
  exclusion of import-move undo. Verify the documentation says that overwritten
  old content is not recoverable through undo.
- [ ] Run the visual scenarios and inspect images for icons, compact/details
  views, tabs, split panes, narrow windows, filtering, dialogs and rename editors.
  Inspect light/dark palettes and representative scaling/font settings. Record
  screenshot review separately from behavioral test results.

## Real SSH/SFTP verification

- [ ] Review the E2E runner and run it against its disposable local fixture with
  isolated keys, known-host storage, settings and data. Save complete output and
  verify nonzero exit on setup or assertion failure.
- [ ] Audit upload test entry points: use `importFromLocalRequest()` for local
  upload and upload cancellation. Use `copyRequest()` only for same-node remote
  copy. Prevent shared client/server filesystem visibility from hiding an
  incorrect transfer direction; use distinct fixtures where practical.
- [ ] Verify unknown-key rejection/acceptance, known-key reconnect, changed-key
  rejection and invalid configured known-host storage. Check that no fallback
  writes into the user's real SSH configuration.
- [ ] Verify listing, mkdir, explicit upload, download, same-node remote
  copy/move/delete, refused existing regular-file targets, bounded text/image
  preview and atomic replacement of a local downloaded file. Run the import-move
  source-removal and in-flight export-disconnect target-preservation cases.
- [ ] Cancel regular-file upload, download and remote copy after byte progress;
  inspect partial files and diagnostics. Repeat on a recursive directory and
  record which completed children remain.
- [ ] Test password, agent and explicit private-key authentication separately on
  a representative service. Confirm saved network places contain no credentials.
- [ ] Break transport during listing, preview and transfer; also terminate the
  server while a mutation is in flight. Verify disconnected state, explicit
  errors, stale-result suppression and the actual endpoint contents before retry.
- [ ] Exercise remote overwrite commit failure between old-target removal and
  staging rename. Confirm the retained staging path is reported and the
  non-atomic recovery boundary is disclosed.
- [ ] Test slow/stalled connections during cancel, disconnect and window/backend
  destruction. Record observed wait time; do not claim immediate cancellation or
  a bounded shutdown time without evidence.
- [ ] Verify remote-to-local paste/drop and two-node relay with live source
  backends. Cover temporary-storage exhaustion, collisions, failure of the second
  hop, source-deletion failure and cancellation. Verify source retention and
  local temporary-data cleanup.

## Integration and disclosure

- [ ] Verify an injected IO backend stays on the GUI thread, is owned by the
  intended host and outlives consumers. Verify provider ownership transfer,
  completion, request deletion and cancellation against the public contracts.
- [ ] Check that the read-only example disables writes in its backend as well as
  in its UI. Explain that `allowedPaths` and UI flags are not a security sandbox.
- [ ] Explain saved-password obfuscation, lack of an OS keychain and settings-file
  permissions. Inspect logs and issue templates for accidental secret exposure.
- [ ] Review cancellation and undo wording against actual behavior: completed
  items remain after batch cancellation; old overwritten content is not retained
  for undo; cross-backend moves are not transactions.
- [ ] Summarize remaining unverified scenarios in the release notes and
  [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md). Do not infer their status from
  historical review notes or a previous release's test result.

## Evidence and decision

| Check or intentional deferral | Result | Evidence / issue | Environment |
| --- | --- | --- | --- |
| Clean build and examples | Pending | | |
| Local safety suite, including skips | Pending | | |
| Visual inspection | Pending | | |
| Real SSH/SFTP fixture | Pending | | |
| Fault injection / production-like service checks | Pending | | |
| Platform coverage | Pending | | |

Release decision and disclosed limitations: ______
