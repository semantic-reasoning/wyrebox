# ADR 0005: Dovecot Index-Backed Storage Runtime

## Status

Accepted for issue #355. Follow-up changes under issue #355 implement it. Until
each lands, the plugin keeps the pre-port behavior that ADR 0004 describes.
Issue #313 adds flag and keyword changes.

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
  - Open loads one SELECT and one UID map. The first sync after open reuses
    that installed state and does not call the daemon again, so SELECT costs
    at most one of each.
  - Later syncs refresh from the daemon only when
    `index_mailbox_want_full_sync` says so. `MAILBOX_SYNC_FLAG_FAST` syncs
    inside the full-sync interval keep the installed state and skip the
    daemon. A cheap mailbox change marker and moving daemon calls off the
    blocking `imap` ioloop remain follow-ups (not yet).
  - It writes the daemon UIDVALIDITY into an empty index.
  - It appends new UIDs in ascending order and raises the next UID to the
    daemon's UIDNEXT.
  - It copies the daemon flags and keywords from the UID map onto every
    message in the index, replacing the index values. The sync transaction
    skips unchanged messages, so an unchanged mailbox reports no flag
    updates.
  - On a daemon refresh, daemon state replaces the mailbox's copy only after
    the index commit succeeds, so the index UIDs and the rows used for FETCH
    always match.
  - A UIDVALIDITY change inside one session fails the sync. The mailbox keeps
    its previous state and reports itself through `is_inconsistent`, so
    `imap` disconnects the session instead of serving new bytes under old
    UIDs.
- Message bytes come from the daemon FETCH path and are served as an istream
  through `index_mail_init_stream`. The daemon client is the plugin's only
  path to WyreBox state; it never opens DuckDB or the object store.
- LIST uses a `wyrebox` mailbox list driver registered with
  `mailbox_list_register` when the plugin loads, replacing the iterator hook.
  It lists the daemon's mailboxes with `/` as the separator. Mailbox names
  pass to the daemon unchanged, so the namespace must have no namespace prefix.
- Subscription model A (issue #361): every listed mailbox is
  `\Subscribed`. `LSUB` and `LIST (SUBSCRIBED)` therefore match the ordinary
  LIST set. `SUBSCRIBE` and `UNSUBSCRIBE` succeed as no-ops and do not change
  later LSUB results. `LIST (SPECIAL-USE)` returns only mailboxes whose
  daemon `special_use` is set and is not `\Inbox` (not an RFC 6154 flag).
  Persistent per-mailbox subscription state stays out of the daemon API until
  a later issue needs it.
- Daemon-reported errors use the `WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR` domain
  with the daemon error class as the code; transport errors stay in
  `G_IO_ERROR`. A missing mailbox answers `NO [NONEXISTENT]`. An unreachable
  daemon or a daemon temporary failure answers `NO [UNAVAILABLE]` for SELECT,
  STATUS and LIST and is logged. Anything else is a critical storage error.
- Operators select the backend with `mail_driver = wyrebox` and
  `mailbox_list_layout = wyrebox`. The storage refuses to start with any other
  list layout. A minimal Dovecot `2.4.2` configuration looks like this:

  ```
  dovecot_config_version = 2.4.2
  dovecot_storage_version = 2.4.2
  mail_driver = wyrebox
  mailbox_list_layout = wyrebox
  mail_path =
  mail_attachment_detection_options =
  mail_plugins {
    wyrebox = yes
  }
  namespace inbox {
    inbox = yes
    separator = /
  }
  import_environment {
    WYREBOX_SOCKET = /run/wyrebox/wyrebox.sock
    WYREBOX_DAEMON_UDS_TIMEOUT_SEC = 30
  }
  ```

  Dovecot 2.4 rejects unknown plugin settings without a config module, and
  config blocks must span several lines: a one-line block fails with
  `Garbage after '{'`.
- The daemon socket path comes from the `WYREBOX_SOCKET` environment
  variable, which `import_environment` passes into the `imap` process. It
  defaults to `/run/wyrebox/wyrebox.sock`. The account identity is the
  Dovecot user name.
- Shared UDS client calls use a 30-second idle timeout by default
  (`g_socket_client_set_timeout` / `g_socket_set_timeout`). Operators may
  raise it with `WYREBOX_DAEMON_UDS_TIMEOUT_SEC` (positive integer only; zero
  or invalid values keep the default). There is no Dovecot plugin setting.
  Expiry surfaces as `G_IO_ERROR_TIMED_OUT`, which the plugin maps to IMAP
  `NO [UNAVAILABLE]`.
- Ordinary mailboxes answer SELECT with `[READ-WRITE]`. EXAMINE and virtual
  mailbox views stay read-only, and a flag change committed on a read-only
  mailbox fails with a not-possible error. Save and copy also fail with a
  not-possible error until #314.
- Flag and keyword changes reach `wyreboxd` when the Dovecot transaction
  commits. The `transaction_commit` wrapper commits the index first, then
  reads the pending flag records, groups identical SET and CLEAR deltas, and
  sends one homogeneous `FlagKeywordUpdateRequest` per group (ascending UID
  list, at most 65535 UIDs). The daemon journals each accepted group once, and
  the plugin updates its copy of a row only after the daemon accepts that UID.
  This covers STORE and the implicit `\Seen` of a `BODY[]` fetch.
  - A keyword the daemon cannot represent, or an unavailable or conflicting
    daemon, stops the push: that message and the remaining ones revert to
    the daemon's values. A daemon rejection of a single message reverts only
    that message. Rejections fail the command with `[CANNOT]`.
  - A message the daemon no longer has is skipped and disappears at the
    next sync.
- The daemon has no expunge operation yet, so the storage refuses expunges
  with a permission error. EXPUNGE and UID EXPUNGE answer
  `OK Expunge ignored`, CLOSE answers `OK Closed without expunging`, and
  messages keep their `\Deleted` flag.
- Dovecot `2.4` adds `$HasAttachment` and `$HasNoAttachment` keywords by
  default. The daemon can store `$` keywords through STORE, but WyreBox still
  refuses to start when `mail_attachment_detection_options` includes
  `add-flags`; the example above sets `mail_attachment_detection_options =`
  empty so Dovecot does not invent attachment keywords behind the daemon.
- Fixture mode no longer compiles the plugin. The real port needs
  lib-index headers that the simplified fixture does not and should not copy.
  Fixture mode keeps the source and build contract checks only.
  This supersedes ADR 0004's guarantee that code compiling in fixture mode
  also compiles in system mode. The fixture-based mailbox and loader smoke
  tests are retired.
- A scripted IMAP test runs the real Dovecot `imap` binary against a real
  `wyreboxd` holding a delivered fixture message:
  - It runs LIST, SELECT, STATUS, and `UID FETCH (BODY.PEEK[])`, compares
    UIDVALIDITY, UIDNEXT and the message count with the daemon's own values,
    and compares the fetched bytes with the golden file.
  - It also covers the missing-mailbox and daemon-down error paths.
  - It covers STORE, the implicit `\Seen`, EXAMINE, the refused expunge, and
    a rejected keyword, checking both the IMAP replies and the daemon flags.
    The Postfix-to-IMAP end-to-end test checks that stored flags survive a
    daemon and Dovecot restart without changing the message bytes.
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

- APPEND, COPY, MOVE, and EXPUNGE stay unsupported until #314. Because
  refused expunges keep `\Deleted` messages, clients that hide deleted
  messages diverge from the server. Do not enable autoexpunge or POP3 on
  WyreBox namespaces until #314: their expunges log commit errors.
- The index is rebuilt per session, so CONDSTORE and QRESYNC modification
  sequences do not survive a reconnect.
- Daemon calls are still blocking on the `imap` ioloop, as `pop3c` does.
  Async daemon calls remain a follow-up; #363 only cuts how many blocking
  SELECT and UID-map calls run.
- Builders without Dovecot `2.4.2` headers lose plugin compile coverage. Only
  the system-mode build, such as the `ubuntu-26.04` CI job, compiles and runs
  the plugin. Plugin changes must be validated there, running `imap` as a
  non-root user.
- The plugin links the full WyreBox library into `imap`. Splitting out a
  thin daemon-client dependency is a follow-up.
- INTERNALDATE and the save date come from the message `Date:` header
  rather than the WyreBox delivery time.
- Virtual mailbox views report no flags or keywords, because the daemon UID
  map for a view carries none. No scripted IMAP test selects a view yet, so
  the read-only refusal of STORE on a view is untested at the IMAP level.
- Without a mailbox change marker, FAST syncs can miss another session's
  updates until the next full sync. A stuck daemon still blocks the `imap`
  ioloop for up to the UDS idle timeout rather than forever.
- The daemon trusts the account identity that the caller supplies. Binding
  callers to an account before multi-user deployment is a follow-up.
- Keeping the previous state on a failed re-sync and disconnecting on a
  UIDVALIDITY change need an interactive IMAP session harness to test.
