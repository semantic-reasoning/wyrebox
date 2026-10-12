# ADR 0005: Dovecot Index-Backed Storage Runtime

## Status

Accepted for issue #355. Follow-up changes under issue #355 implement it. Until
each lands, the plugin keeps the pre-port behavior that ADR 0004 describes.

## Context

ADR 0004 moved the build baseline to Dovecot `2.4.2` on Ubuntu 26.04, but
the plugin still only compiles. It registers a `wyrebox` storage class whose
mailboxes answer from a daemon snapshot and never open a Dovecot lib-index.
LIST goes through a global iterator hook instead of a mailbox list driver.
A real `imap` process needs an index for sequence and UID mapping, so it
cannot select, fetch, or list WyreBox mailboxes. The only plugin behavior
tests compile against the simplified header fixture in `tests/dovecot/fixtures/`.
They model the old design and cannot exercise lib-index at all.

## Decision

- The `wyrebox` storage is index-backed, following Dovecot's own `pop3c`
  and `imapc` backends:
  - The storage class sets `MAIL_STORAGE_CLASS_FLAG_NO_ROOT`, so it needs no
    `mail_path`.
  - Mailboxes are allocated with `index_storage_mailbox_alloc`.
  - Mailboxes open with `index_storage_mailbox_open` after a successful
    daemon SELECT.
  - The index is in-memory. The list driver reports no index path, and every
    session rebuilds the index from the daemon, so WyreBox mailbox identity
    never lives in Dovecot files.
- Sync takes the daemon's SELECT result and UID map:
  - It writes the daemon UIDVALIDITY into an empty index.
  - It appends new UIDs in ascending order and raises the next UID to the
    daemon's UIDNEXT.
  - A UIDVALIDITY change inside one session fails the sync.
- Message bytes come from the daemon FETCH path and are served as an istream
  through `index_mail_init_stream`. The daemon client is the plugin's only
  path to WyreBox state; it never opens DuckDB or the object store.
- LIST uses a `wyrebox` mailbox list driver registered with
  `mailbox_list_register` when the plugin loads, replacing the iterator hook.
  It lists the daemon's mailboxes with `/` as the separator.
- Operators select the backend with `mail_driver = wyrebox` and
  `mailbox_list_layout = wyrebox`. The storage refuses to start with any other
  list layout. A minimal Dovecot `2.4.2` configuration looks like this:

  ```
  dovecot_config_version = 2.4.2
  dovecot_storage_version = 2.4.2
  mail_driver = wyrebox
  mailbox_list_layout = wyrebox
  mail_path =
  mail_plugins {
    wyrebox = yes
  }
  namespace inbox {
    inbox = yes
    separator = /
  }
  import_environment {
    WYREBOX_SOCKET = /run/wyrebox/wyrebox.sock
  }
  ```

  Dovecot 2.4 rejects unknown plugin settings without a config module, and
  config blocks must span several lines: a one-line block fails with
  `Garbage after '{'`.
- The daemon socket path comes from the `WYREBOX_SOCKET` environment
  variable, which `import_environment` passes into the `imap` process. It
  defaults to `/run/wyrebox/wyrebox.sock`. The account identity is the
  Dovecot user name.
- Until flag and mutation support land, the storage is read-only. SELECT
  answers `[READ-ONLY]`, and save, copy, and expunge fail with a
  not-possible error.
- Fixture mode no longer compiles the plugin. The real port needs
  lib-index headers that the simplified fixture does not and should not copy.
  Fixture mode keeps the source and build contract checks only.
  This supersedes ADR 0004's guarantee that code compiling in fixture mode
  also compiles in system mode. The fixture-based mailbox and loader smoke
  tests are retired.
- A scripted IMAP test runs the real Dovecot `imap` binary against a real
  `wyreboxd` holding a delivered fixture message:
  - It runs LIST, SELECT, and `UID FETCH (BODY.PEEK[])` and compares the
    fetched bytes with the golden file.
  - It also covers the missing-mailbox and daemon-down error paths.
  - It runs `imap` as a non-root user, because `imap` refuses to run as root
    without `mail_uid`. When Dovecot is missing or the test runs as root, it
    reports a skip.

## Alternatives Considered

- Keep the snapshot-only mailbox and add a Maildir side index. Rejected:
  Maildir must not become the storage model, and the index would duplicate
  WyreBox mailbox identity.
- Persist the Dovecot index under `mail_home`. Rejected for now: the daemon
  owns UID and UIDVALIDITY state. A persistent cache can come later without
  changing the daemon contract.
- Pass the socket path as a plugin setting. Rejected: Dovecot `2.4.2`
  rejects settings that no loaded config module declares, and doveconf only
  loads config modules from the system module directory.
- Keep compiling the plugin against the header fixture. Rejected: it would
  mean copying lib-index headers into the fixture and maintaining a second
  Dovecot.

## Consequences And Known Gaps

- Flags and keywords stay unsupported until #313. APPEND, COPY, MOVE, and
  EXPUNGE stay unsupported until #314.
- Daemon calls are blocking and run on the `imap` ioloop, as `pop3c` does.
  Async daemon calls are a follow-up.
- Builders without Dovecot `2.4.2` headers lose plugin compile coverage. Only
  the system-mode build, such as the `ubuntu-26.04` CI job, compiles and runs
  the plugin. Plugin changes must be validated there, running `imap` as a
  non-root user.
- The plugin links the full WyreBox library into `imap`. Splitting out a
  thin daemon-client dependency is a follow-up.
