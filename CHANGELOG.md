# Changelog

## Unreleased — 2026-10-26

- Drain serial cross-backend queues for synchronous, missing and prematurely
  destroyed requests; observe cancellation before starting a relay's upload leg.
- Link consecutive local move undo through execution-time identity/content/tree
  proofs, preserving refusal of external edits or replacements.
- End interrupted directory loads safely and isolate reentrant navigation from
  older completion callbacks, including synchronously delivered results.
- Bind pending connections to their provider and attempt generation so provider
  replacement, destruction and explicit IO changes permit reliable retries.
- Add undo-history, directory-lifetime, provider-replacement and serial-transfer
  regression suites to the existing Qt5/Qt6 and SSH feature matrix.
- Preserve caller-owned remote backends when splitting a workspace; keep
  component-owned sessions alive while either pane remains open.
- Guard custom-side-panel toggles and SSH result/host-key callbacks against
  object destruction during direct signals or modal dialogs.
- Add sidebar/workspace and SSH-provider lifetime regressions, plus a real SFTP
  upload check that destroys the request owner from a change notification.
- Guard copy/move undo with execution-time identity and complete content/tree
  revision proofs; preserve changed or unverified data and track partial retries.
- Replace destructive remote-overwrite commit with no-replace backup, install
  and safe restoration, retaining and reporting recoverable copies on failure.
- Add bounded Linux validation, conservative custom-backend contracts and
  deterministic fault tests plus real loopback SFTP recovery checks.


## 0.1.0 source candidate — 2026-10-23

- Add a public CMake build, explicit Qt 5/6 selection and optional SSH/SFTP.
- Export/install a reusable target and validate a separate consuming application.
- Add complete embedded-host and read-only memory IO examples, independent
  installed-package builds and behavioral checks; support generic backend URLs
  throughout navigation.
- Separate public configuration from original host settings.
- Improve navigation, tabs, sidebar, icon labels, details and theme-aware visuals.
- Connect regression and real SSH/SFTP tests to CTest and a public CI matrix.
- Add a portable screenshot suite and document recovery/ownership limitations.
- Publish MIT licensing and the rights holder's source provenance statement.

See [validation evidence](docs/VALIDATION.md) for tested combinations. This is an
early source release with no cross-platform or fault-tolerance certification.
