# Dovecot Fixture Bundle Contract

## Status

Accepted for issue #298 as the reproducible input contract for the IMAP fetch
verification recipe.

## Scope

This contract defines the repository-managed Dovecot inputs that the IMAP fetch
recipe discovers by default.

It does not define new Dovecot behavior. It only describes where WyreBox looks
for the source, build, and loader inputs that already gate the recipe.

## Repository Managed Inputs

The recipe uses these default repository-managed paths when no explicit
environment overrides are supplied:

- `tests/dovecot/fixtures/valid-2.3.21.1`
- `tests/dovecot/fixtures/valid-2.3.21.1/build-config-valid`
- `/usr/lib/dovecot/libdovecot-storage.so`

The first two paths are checked-in fixtures. They provide the Dovecot source
tree contract and configured build tree contract used by the backend and loader
smoke setup.

The loader path is the local system Dovecot storage module used to prove plugin
loading behavior. The recipe treats it as a required prerequisite and fails
explicitly if it is missing.

## Recipe Discovery Rules

1. `WYREBOX_DOVECOT_SOURCE_DIR` overrides the repository default source fixture.
2. `WYREBOX_DOVECOT_BUILD_DIR` overrides the repository default build fixture.
3. `WYREBOX_DOVECOT_LOADER_ARCHIVE` overrides the repository default loader
   path.
4. If any required path is missing, the recipe fails with a prerequisite error.
5. The recipe does not silently skip when the default fixture bundle is absent.

## Verification

The bundle is considered valid when the following checks pass:

- `tests/dovecot/test-dovecot-source-contract.py`
- `tests/dovecot/test-dovecot-build-contract.py`
- the Dovecot loader smoke setup used by the IMAP fetch recipe

The bundle is intentionally minimal. It exists only to make the Dovecot IMAP
fetch recipe reproducible from documented inputs instead of ad hoc manual
environment setup.
