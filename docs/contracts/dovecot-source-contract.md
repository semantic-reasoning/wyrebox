# Dovecot Source Contract

## Status

Accepted for issue #8 before any storage plugin implementation.

## Scope

This contract defines the minimal Dovecot source requirements WyreBox checks in CI
before writing storage backend code. It validates that the provided source tree
matches the expected Dovecot 2.3.21.1 storage ABI surface and does not attempt to
build or link against Dovecot.

## Pinned Source

WyreBox pins source-contract validation to Dovecot `2.3.21.1` before plugin code.
The checker requires that:

- `AC_INIT([Dovecot],[2.3.21.1],...)` is present.
- `DOVECOT_ABI_VERSION` is declared.

## Required Files

The following files must exist in a validated source tree:

- `configure.ac`
- `src/lib-storage/mail-storage.h`
- `src/lib-storage/mail-storage-private.h`
- `src/lib-storage/mail-storage-hooks.h`

## Required Types And Symbols

Checker validation requires these ABI/storage names to be present:

- `DOVECOT_ABI_VERSION`
- `mail_storage_hooks_add`
- `struct mail_storage`
- `struct mailbox`
- `struct mail`
- `struct mailbox_status` with `uidvalidity` and `uidnext`
- `struct mail_storage_vfuncs`
- `struct mailbox_vfuncs`
- `struct mail_vfuncs`

## Required Storage Vfunc Coverage

To keep plugin work inside the expected API surface, required methods are
checked in the corresponding vfunc structs:

- LIST-related:
  - `mail_storage_vfuncs::add_list`
- SELECT/status-related:
  - `mailbox_vfuncs::open`
  - `mailbox_vfuncs::get_status`
- FETCH-related:
  - `mail_vfuncs::get_stream`
- UPDATE-related:
  - `mail_vfuncs::update_flags`
  - `mail_vfuncs::update_keywords`
- SEARCH-related:
  - `mailbox_vfuncs::search_init`

The checker only validates that these symbols are present in source declarations.
It does not verify semantics or plugin behavior.

## Verification

The contract is exercised by a focused Meson test named
`dovecot source contract` with synthetic fixtures for pass/fail behavior.
No Dovecot binary/package dependency is required by the test suite.
