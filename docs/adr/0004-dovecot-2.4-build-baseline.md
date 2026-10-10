# ADR 0004: Dovecot 2.4 Build Baseline On Ubuntu 26.04

## Status

Accepted for issue #354. `.internal-docs/` is not part of the repository, so
this ADR together with the Dovecot source, build, and fixture-bundle contracts
is the decision record. The decision is implemented by follow-up changes under
issue #354; until each lands, the contracts it touches still describe the
`2.3.21.1` fixture baseline.

## Context

ADR 0002 left the supported Dovecot version open; this ADR pins it. The
Dovecot source and build contracts pinned the storage plugin to Dovecot
`2.3.21.1` with the ABI template `2.3.ABIv21($PACKAGE_VERSION)`. The plugin
was only ever compiled against the simplified header fixture in
`tests/dovecot/fixtures/`, which declares some types where real Dovecot
`2.4.2` declares them elsewhere (`struct message_size`) or not at all (a
mailbox LIST child state enum). No CI job or development container installed a real Dovecot or
Postfix, so no test could run the plugin in a real `imap` process.

The project baseline is now Ubuntu 26.04. Its packages ship Dovecot `2.4.2`
(`dovecot-dev`, `dovecot-imapd`), whose `config.h` defines
`DOVECOT_ABI_VERSION "2.4.ABIv2"`, and Postfix `3.10.6`. Dovecot 2.3 is not
packaged there.

## Decision

- The Dovecot baseline is Dovecot `2.4.2` as packaged by Ubuntu 26.04. The
  plugin's `wyrebox_plugin_version` must equal that build's
  `DOVECOT_ABI_VERSION`, `2.4.ABIv2`.
- The source, build, and fixture-bundle contracts and their checkers validate
  the `2.4.2` / `2.4.ABIv2` baseline. Dovecot `2.4.2` no longer defines
  `HAVE__BOOL` or `HAVE_SOCKLEN_T`, so the configured-header contract stops
  requiring them.
- The plugin can be built in two modes with `-Ddovecot_backend=enabled`:
  - System mode builds against the installed `dovecot-dev` headers, located
    through `/usr/lib/dovecot/dovecot-config`. This is the baseline mode.
  - Fixture mode keeps building against the header fixture under
    `tests/dovecot/fixtures/`. The fixture mirrors where Dovecot `2.4.2`
    declares each type the plugin uses, so code that compiles in fixture mode
    also compiles in system mode. It stays until the fixture-based mailbox
    smoke test is migrated or retired.
- A checked-in container definition based on `ubuntu:26.04` installs the full
  toolchain, including Cap'n Proto, `dovecot-dev`, `dovecot-imapd`, and
  `postfix`. A CI job builds that image and runs the full test suite in it in
  both Dovecot modes. That job is the Dovecot baseline gate.

## Alternatives Considered

- Keep Dovecot `2.3.21.1` and build it from source inside the Ubuntu 26.04
  image. Rejected: it pins WyreBox to a release line that the target
  distribution no longer ships, and a source build would be a project-owned
  Dovecot that operators do not run.
- Use a container job (`container:` in GitHub Actions) with a prebuilt
  registry image. Deferred: building the image inside the job needs no
  registry or publishing credentials.

## Consequences And Known Gaps

- Compiling against Dovecot `2.4.2` does not make the plugin work inside a real
  `imap` process. The plugin still lacks lib-index integration; issue #355
  ports it to the real storage runtime and migrates or retires the fixture
  mailbox smoke test.
- The loader smoke test still links against a Dovecot `liblib.a` archive,
  which the distribution does not ship, so it is not available in system
  mode. Loading the plugin through the installed Dovecot libraries belongs to
  issue #355.
- The existing `ubuntu-24.04` CI jobs never enabled the Dovecot backend, so
  they are not Dovecot baselines. They stay for the clang matrix and the arm64
  sanitizer until equivalent coverage exists in the Ubuntu 26.04 image.
  Retiring them is a follow-up.
