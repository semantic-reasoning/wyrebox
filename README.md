# WyreBox

WyreBox is a Linux-only Maildir replacement backend for existing Postfix and
Dovecot deployments. It keeps Postfix as the SMTP ingress layer, Dovecot as the
IMAP protocol layer, and existing IMAP clients unchanged, while replacing the
underlying storage model.

## Overview

- **`wyreboxd`** — the core daemon and the only writer of canonical state.
- **Append-only journal** — the canonical write path; every mutation is recorded
  durably before it is acknowledged.
- **Immutable object store** — original RFC 5322 message bytes are kept
  byte-for-byte, content-addressed by SHA-256, and never rewritten for flags,
  moves, facts, or views.
- **DuckDB** — a materialized query and index store fed by journal replay.
- **Wirelog** — Datalog facts, rules, and derived mailbox views, exposing virtual
  mailboxes as first-class IMAP folders.
- **Cap'n Proto API** — a controlled request/response interface over a Unix
  domain socket (`/run/wyrebox/wyrebox.sock`).

Email is delivered through Postfix into the daemon, stored immutably and recorded
in the journal, materialized into DuckDB for query, and exposed through Dovecot's
standard IMAP protocol — including virtual mailboxes derived from message facts.

## License

Mozilla Public License 2.0 (MPL-2.0).
