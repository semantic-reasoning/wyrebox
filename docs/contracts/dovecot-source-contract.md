# Dovecot Source Contract

## Status

Accepted for issue #8 before any storage plugin implementation. Amended by
issue #354: the baseline is Dovecot `2.4.2` (ADR 0004).

## Scope

This contract defines the minimal Dovecot source requirements WyreBox checks in CI
before writing storage backend code. It validates that the provided source tree
matches the expected Dovecot 2.4.2 storage ABI surface and does not attempt to
build or link against Dovecot.

## Pinned Source

WyreBox pins source-contract validation to Dovecot `2.4.2`.
The checker requires that:

- `AC_INIT([Dovecot],[2.4.2],...)` is present.
- `DOVECOT_ABI_VERSION` is declared.
- `configure.ac` defines `DOVECOT_ABI_VERSION` as `2.4.ABIv2`.
- `config.h.in` exposes the `DOVECOT_ABI_VERSION` configuration placeholder.

Upstream Dovecot `2.4` generates its version and ABI strings in `configure.ac`
through m4 helper scripts, so these literal checks describe the checked-in
fixture bundle layout rather than an upstream checkout.

## Required Files

The following files must exist in a validated source tree:

- `configure.ac`
- `config.h.in`
- `src/lib-storage/mail-storage.h`
- `src/lib-storage/mail-storage-private.h`
- `src/lib-storage/mail-storage-hooks.h`
- `src/lib/module-dir.h`

For module compilation the source tree is not enough; when backend code
generation is enabled, WyreBox additionally validates a configured build
directory via the `dovecot_build_contract` checks.

## Installed Header Mode

Issue #354 adds `--include-dir <dir>` for validating an installed, flat Dovecot
include directory such as `/usr/include/dovecot` from the `dovecot-dev`
package. It cannot be combined with a source directory. In this mode:

- every required header is looked up by basename in the flat directory, and a
  missing one is reported as `missing required Dovecot header: <name>`;
- `configure.ac` and `config.h.in` are not inspected; instead the installed
  `config.h` must define `DOVECOT_VERSION "2.4.2"` and
  `DOVECOT_ABI_VERSION "2.4.ABIv2"`;
- all type, symbol, and vfunc checks below apply unchanged.

The `mailbox_list_get_storage_name` signature is checked in `mailbox-list.h`,
where Dovecot 2.4.2 declares it, in both modes.

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
- `struct module`
- `struct module_dir_load_settings` with `abi_version` and `require_init_funcs`
- `module_get_symbol`
- `module_get_plugin_name`
- `mail_storage_class_register`
- `mail_storage_class_unregister`

## Required Storage Vfunc Coverage

To keep plugin work inside the expected API surface, required methods are
checked in the corresponding vfunc structs:

- LIST-related:
  - `mail_storage_vfuncs::add_list`
- Storage lifecycle-related:
  - `mail_storage_vfuncs::alloc`
  - `mail_storage_vfuncs::create`
  - `mail_storage_vfuncs::destroy`
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

## Required Plugin Entrypoint Surface

The next backend skeleton must use the Dovecot module ABI headers directly and
define plugin symbols on that contract:

- `wyrebox_plugin_version = DOVECOT_ABI_VERSION`
- `wyrebox_plugin_init(struct module *)`
- `wyrebox_plugin_deinit(void)`

This contract forbids ad-hoc module ABI declarations in the plugin skeleton; the
module header surface from Dovecot must be used directly.

The checker validates only the Dovecot module ABI surface needed by those future
WyreBox plugin symbols and storage registration hooks. It does not validate a
WyreBox plugin implementation, storage-backend semantics, or plugin behavior.

## Required Storage Registration API

Backend registration is pinned to these Dovecot storage APIs in
`mail-storage.h`:

- `void mail_storage_class_register(struct mail_storage *storage_class);`
- `void mail_storage_class_unregister(struct mail_storage *storage_class);`

## Verification

The contract is exercised by a focused Meson test named
`dovecot source contract` with synthetic fixtures for pass/fail behavior.
No Dovecot binary/package dependency is required by the test suite.

The Meson test `dovecot installed headers contract` runs both the source and
build checkers in `--include-dir` mode against `/usr/include/dovecot` (or
`WYREBOX_DOVECOT_INCLUDE_DIR`) and is skipped when those headers are absent.
