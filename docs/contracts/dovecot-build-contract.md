# Dovecot Build Contract

## Status

Accepted for issue #8 before any storage plugin implementation. Amended by
issue #354: the baseline is Dovecot `2.4.2` (ADR 0004). Amended by issue
#355: fixture mode no longer compiles the plugin (ADR 0005). Amended by issue
#315: adds the real-daemon Postfix-to-Dovecot IMAP e2e test, which runs in
system mode.

## Scope

WyreBox validates the Dovecot build headers before enabling the Dovecot backend.
This avoids compiling module code against unconfigured source trees.

The configured build contract requires a concrete build directory containing a
generated `config.h` and validates that the headers used by WyreBox build checks
can be included with the source-tree headers.

## Build Inputs

- `-Ddovecot_backend=enabled` must be paired with a configured source tree and
  build directory.
- `-Ddovecot_source_dir=/path/to/dovecot` points at the Dovecot source checkout.
- `-Ddovecot_build_dir=/path/to/dovecot/build` points at the corresponding
  configured build directory.

System mode (issue #354) builds against an installed `dovecot-dev` instead:

- `-Ddovecot_backend=enabled -Ddovecot_config=/usr/lib/dovecot/dovecot-config`
  reads the single `-I` directory from `LIBDOVECOT_INCLUDE` via
  `tools/dovecot-config-include-dir.py` and runs both contract checkers in
  `--include-dir` mode against it.
- `-Ddovecot_config` cannot be combined with `-Ddovecot_source_dir` or
  `-Ddovecot_build_dir`.
- Issue #355 (ADR 0005): only system mode compiles the plugin and registers
  the `dovecot plugin symbols` test. `dovecot imap real daemon` is registered
  whenever Cap'n Proto serialization is available. It drives the installed
  `imap` binary against a live `wyreboxd`, and skips unless the plugin was
  built (system mode), `imap` is installed, and the user is non-root. Fixture
  mode runs the contract checks only. The fixture-based mailbox and loader smoke tests and the
  `dovecot_loader_*` options are retired.
- Issue #315: `postfix dovecot imap e2e`
  (`tests/dovecot/test-postfix-dovecot-imap-e2e.c`) runs the whole delivery
  path against real processes, under the same Cap'n Proto condition.
  - It delivers one message with `wyrebox-postfix-pipe` and one through the
    `wyrebox-postfix-lmtp-delivery` listener, using the argv and LMTP dialog
    that Postfix `pipe(8)` and `lmtp(8)` use. The test does not start Postfix
    `master(8)`, which must run as root.
  - It starts a rootless Dovecot master (`dovecot -F`) with the WyreBox plugin,
    logs in with `LOGIN` over an `imap-login` Unix listener, and checks LIST,
    SELECT, and a byte-exact UID FETCH of both messages.
  - It then restarts `wyreboxd` and the Dovecot master and requires the same
    UIDVALIDITY, UIDNEXT, EXISTS, and UID list, and the same message bytes.
  - It skips unless the plugin was built (system mode), `/usr/sbin/dovecot`,
    `/usr/lib/dovecot/imap-login`, and `/usr/lib/dovecot/imap` are installed,
    and the user is non-root.
    With `WYREBOX_E2E_REQUIRE_RUNTIME=1` in the environment it fails instead
    of skipping.

In both modes Meson additionally requires `cc.get_define('DOVECOT_ABI_VERSION')`
to equal `"2.4.ABIv2"`. The `dovecot system setup` test configures system mode
in a scratch build directory, builds the plugin, loads it next to the installed
`libdovecot` and `libdovecot-storage`, and asserts the exported
`wyrebox_plugin_version`; it is skipped when `/usr/lib/dovecot/dovecot-config`
is absent.

## Required Build Config Header Items

`dovecot_build_dir/config.h` must define
`DOVECOT_ABI_VERSION` as exactly `2.4.ABIv2`.

It must also define at least:

- `OFF_T_MAX`
- `PRIuUOFF_T`
- `SIZEOF_INT`
- `SIZEOF_LONG`
- `SIZEOF_VOID_P`
- `SSIZE_T_MAX`
- `UOFF_T_MAX`

Dovecot `2.4.2` no longer defines `HAVE__BOOL` or `HAVE_SOCKLEN_T`, so the
contract does not require them.

It must also define one `uoff_t` selector macro:

- `HAVE_UOFF_T`
- `UOFF_T_INT`
- `UOFF_T_LONG`
- `UOFF_T_LONG_LONG`

These mirror the configured Dovecot headers used by `lib.h` and related
compatibility headers. A raw Dovecot source checkout is not enough, because the
plugin compile surface depends on the generated `config.h` from the matching
configured build.

## Installed Header Mode

Issue #354 adds `--include-dir <dir>` for the installed `dovecot-dev` layout,
where `config.h` and every Dovecot header live in one flat directory such as
`/usr/include/dovecot`. It cannot be combined with source/build directories.
The same `config.h` macro checks apply to `<dir>/config.h`, and the mailbox
vfunc probe compiles with `-I<dir>` as its only Dovecot include path.

## Verification

WyreBox checks this contract in two phases when the backend is enabled:

1. `tools/check-dovecot-build-contract.py` validates that the configured build
   directory exists and exposes `config.h` with required macro definitions.
2. A Meson `compiles()` probe includes `config.h`, `lib.h`, and
   the Dovecot module and storage hook headers from the configured source/build
   paths to confirm compilation compatibility. The probe uses `gnu11`, matching
   the C dialect expected by the pinned Dovecot 2.4.x headers on modern
   compilers, and includes the Dovecot `src/lib-index`, `src/lib`,
   `src/lib-mail`, and `src/lib-storage` header directories.

This contract is intentionally compile-only; it does not link or run Dovecot.
