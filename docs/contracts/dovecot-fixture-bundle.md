# Dovecot Fixture Bundle Contract

## Status

Accepted for issue #298 as the reproducible input contract for the IMAP fetch
verification recipe. Amended by issue #355 (ADR 0005): fixture mode no longer
compiles the plugin, and the recipe builds against an installed Dovecot.
Amended by issue #315: adds the Postfix-to-Dovecot IMAP e2e recipe.

## Scope

This contract defines the repository-managed Dovecot inputs that the backend
and the IMAP fetch recipe discover by default.

It does not define new Dovecot behavior. In fixture mode the bundle feeds
contract checks only: the source and build contract checkers and the
configured header probe. The plugin itself is compiled and run only against an
installed Dovecot `2.4.2`.

## Repository Managed Inputs

The fixture-mode contract checks use the first two paths, and the IMAP recipes
use the installed `dovecot-config`:

- `tests/dovecot/fixtures/valid-2.4.2`
- `tests/dovecot/fixtures/valid-2.4.2/build-config-valid`
- `/usr/lib/dovecot/dovecot-config`

The first two paths are checked-in fixtures. They provide the Dovecot source
tree contract and configured build tree contract checked in fixture mode.

The `dovecot-config` path comes from the installed `dovecot-dev` package. The
recipes build the plugin against it in system mode and run the
`dovecot imap real daemon` test, so they also require the `dovecot-imapd`
binary at `/usr/lib/dovecot/imap` and a non-root user.

The `postfix dovecot imap e2e` test also starts its own Dovecot master, so its
recipe additionally requires `/usr/sbin/dovecot` from `dovecot-core` and
`/usr/lib/dovecot/imap-login` from `dovecot-imapd`, which depends on
`dovecot-core`.

## Recipe Discovery Rules

1. The IMAP fetch recipe accepts `WYREBOX_DOVECOT_CONFIG` to override the
   default installed `dovecot-config`. The Postfix-to-Dovecot recipe always
   uses the default path.
2. If `dovecot-config` or the `imap` binary is missing, or the recipe runs as
   root, the recipe fails with a prerequisite error.
3. The recipe does not silently skip when its prerequisites are absent.
4. `docs issue 0315 postfix dovecot imap e2e recipe` runs only
   `postfix dovecot imap e2e` with `WYREBOX_E2E_REQUIRE_RUNTIME=1`, so the
   test fails instead of skipping when its runtime is missing. Missing
   `/usr/sbin/dovecot`, `/usr/lib/dovecot/imap-login`, or
   `/usr/lib/dovecot/imap` is a prerequisite error.

## Verification

The bundle is considered valid when the following checks pass:

- `tests/dovecot/test-dovecot-source-contract.py`
- `tests/dovecot/test-dovecot-build-contract.py`
- `tests/dovecot/test-dovecot-fixture-setup.py`, which configures fixture mode
  and asserts that it registers no plugin build or plugin tests

The bundle is intentionally minimal. It exists only to keep the Dovecot
contract checks reproducible from documented inputs instead of ad hoc manual
environment setup.
