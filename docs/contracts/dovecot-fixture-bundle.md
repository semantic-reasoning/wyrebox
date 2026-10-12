# Dovecot Fixture Bundle Contract

## Status

Accepted for issue #298 as the reproducible input contract for the IMAP fetch
verification recipe. Amended by issue #355 (ADR 0005): fixture mode no longer
compiles the plugin, and the recipe builds against an installed Dovecot.

## Scope

This contract defines the repository-managed Dovecot inputs that the backend
and the IMAP fetch recipe discover by default.

It does not define new Dovecot behavior. In fixture mode the bundle feeds
contract checks only: the source and build contract checkers and the
configured header probe. The plugin itself is compiled and run only against an
installed Dovecot `2.4.2`.

## Repository Managed Inputs

The backend and the recipe use these default paths when no explicit
environment overrides are supplied:

- `tests/dovecot/fixtures/valid-2.4.2`
- `tests/dovecot/fixtures/valid-2.4.2/build-config-valid`
- `/usr/lib/dovecot/dovecot-config`

The first two paths are checked-in fixtures. They provide the Dovecot source
tree contract and configured build tree contract checked in fixture mode.

The `dovecot-config` path comes from the installed `dovecot-dev` package. The
recipe builds the plugin against it in system mode, treats it as a required
prerequisite, and fails explicitly if it is missing.

## Recipe Discovery Rules

1. `WYREBOX_DOVECOT_CONFIG` overrides the default installed `dovecot-config`.
2. If the `dovecot-config` path is missing, the recipe fails with a
   prerequisite error.
3. The recipe does not silently skip when its prerequisites are absent.

## Verification

The bundle is considered valid when the following checks pass:

- `tests/dovecot/test-dovecot-source-contract.py`
- `tests/dovecot/test-dovecot-build-contract.py`
- `tests/dovecot/test-dovecot-fixture-setup.py`, which configures fixture mode
  and asserts that it registers no plugin build or plugin tests

The bundle is intentionally minimal. It exists only to keep the Dovecot
contract checks reproducible from documented inputs instead of ad hoc manual
environment setup.
