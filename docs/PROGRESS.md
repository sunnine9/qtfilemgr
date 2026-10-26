# Release preparation status

This document describes the current source candidate. Validation results and
remaining limitations are recorded separately below.

- Standalone CMake build, explicit Qt major selection and optional SSH backend.
- Installable `QtFileMgr::QtFileMgr` target with an independent consumer check.
- Independent application configuration namespace and standard configuration paths.
- Root English/Chinese README, demonstration screenshots, MIT license and source notices.
- CTest regression, opt-in real SSH fixture and portable visual tools.
- Linux CI matrix for Qt 5/6 and SSH ON/OFF.

Actual results and deliberately unverified scenarios are recorded in
[VALIDATION.md](VALIDATION.md), [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) and
[RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).

Two complete educational examples are available: an embedded host window and a
read-only memory IO backend. Both build in-tree or against an installed package;
behavioral smoke checks and backend contract tests are integrated with CTest.
See [EXAMPLES.md](EXAMPLES.md).

## 2026-10-26 safety update

Guarded undo validates execution-time identity/content/tree proofs and tracks
partial retries. Remote overwrite now backs up old targets and attempts safe
rollback; uncertain outcomes retain recovery nodes. New deterministic and real
loopback SFTP tests are part of CTest. See [SAFETY.md](SAFETY.md) and the updated
[validation record](VALIDATION.md) for the bounded proof and retained-backup limits.
