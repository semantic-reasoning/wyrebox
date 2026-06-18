# 0015: System Validation Report

## Purpose

Record the current Linux-only WyreBox prototype validation state for the release-readiness
spike.

This document is a validation report, not new feature design. It summarizes the evidence
available from the repository test harness and the local build environment.

## Validation Scope

- Meson build and test health.
- Postfix integration coverage in the repository test suite.
- Dovecot integration coverage in the repository test suite.
- Ingestion, schema, journal, wirelog, and object-store coverage.
- Operational readiness notes and remaining gaps.

## Environment

- Commit: `81c7928`
- Host: Linux `u6332-v2`
- Kernel: `7.0.11-arch1-1`
- Distribution: Arch Linux rolling
- Meson: `1.11.1`
- Cap'n Proto: `1.4.0`
- Socket path: `/run/wyrebox/wyrebox.sock`

## Build Evidence

`meson compile -C build`

Result:

- Completed successfully.
- All 296 build steps finished.
- Final targets included `wyrebox_plugin.so`, `wyrebox-postfix-pipe`,
  `wyrebox-postfix-lmtp`, `wyrebox-postfix-lmtp-listener`, and the test binaries.

## Test Evidence

`meson test -C build --print-errorlogs`

Result:

- `115/115 OK`
- `Fail: 0`

Representative targeted suites that passed before and during the current validation pass:

- Schema migration and metadata store tests.
- Dovecot source, build, daemon client, and mailbox smoke tests.
- Documentation validation tests for the Postfix and Dovecot integration spikes.

## Operational Readiness Notes

- The repository test harness confirms the current prototype can build and run its full
  automated suite successfully in this environment.
- The canonical socket path is already fixed to `/run/wyrebox/wyrebox.sock`.
- The implementation surface remains Linux-only and uses Meson-based build orchestration.

## Known Limitations

- This container does not provide live `postfix` or `dovecot` service binaries for a
  real systemd-backed end-to-end deployment check.
- This report does not claim a production deployment smoke test outside the repository
  harness.
- Backup and restore remain a validation note in the spike scope; no new backup system is
  introduced here.
- Packaging smoke tests for distro-specific post-install flows are still a follow-up
  concern if required for release packaging.

## Release-Readiness Conclusion

The current prototype is buildable and the repository test suite is green. The remaining
release-readiness gaps are not failing core tests; they are the lack of a live,
host-managed Postfix/Dovecot deployment check in this container plus the still-explicit
backup/restore and packaging smoke test follow-ups from the spike scope. Those gaps should
stay explicit until a system-level validation environment is available.
