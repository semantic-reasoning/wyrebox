# Dovecot Build Contract

## Status

Accepted for issue #8 before any storage plugin implementation. Amended by
issue #354: the baseline is Dovecot `2.4.2` (ADR 0004).

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
