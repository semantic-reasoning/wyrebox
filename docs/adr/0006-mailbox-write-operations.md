# ADR 0006: Mailbox Write Operations

## Status

Accepted for issue #314. Follow-up stacked PRs implement the journal, schema,
daemon API, view refresh, and Dovecot plugin changes. Contracts that this ADR
summarizes are already updated under the same issue.

## Context

IMAP clients expect APPEND, COPY, MOVE, and EXPUNGE. The WyreBox Dovecot
backend refused those paths, and the daemon API had no matching operations.
UID assignment for deliveries happens at materialization time in journal
order, and ADR 0003 requires re-applying delivery memberships after a held
account's catch-up pass. A later EXPUNGE or MOVE design therefore needs
tombstones rather than hard-deleting membership rows. Only INBOX exists in
production today; mailbox create/rename/delete stays out of scope for #314.

## Decision

- **EXPUNGE** journals `MembershipsExpunged` and hides ordinary memberships
  with a tombstone (`is_visible` becomes false; `expunged_journal_offset` /
  `expunged_journal_sequence` record the hide). UIDNEXT and UIDVALIDITY are
  unchanged; tombstoned UIDs are never reused. Already-absent UIDs are
  omitted; an all-absent request succeeds without a journal append. Schema
  v12 rebuilds `mailbox_memberships` with `journal_item` and those tombstone
  columns (checkpoint PRESERVE).
- **COPY and MOVE** journal `MembershipsCopied` and `MembershipsMoved` with a
  shared payload. Multi-UID commands are one record with a `journal_item`
  index. Destination UIDs are reserved under the daemon mutation lock (the
  request-router route lock) after a fresh catch-up, carried in the payload,
  and returned for COPYUID.
  MOVE tombstones sources in the same materialization transaction. Neither
  operation duplicates or rewrites the raw object.
- **APPEND** journals `MessageAppended` with the same durability as delivery:
  content-addressed object store write under the journal append guard, then
  the durable record. There is no APPEND idempotency key. Size is bounded by
  the Cap'n Proto frame limit (4 MiB) minus a reserve and maps to Dovecot
  `[LIMIT]`. MULTIAPPEND is not atomic: each message is its own durable
  request.
- **Virtual mailboxes** refuse mailbox-write destinations and MOVE/EXPUNGE
  sources. COPY may use a virtual mailbox as a source. A view shows a message
  only while it has at least one visible ordinary membership (aligned with
  `object_reachability` / GC candidates). Flag/keyword STORE on a selected
  virtual mailbox remains under the Flag Keyword Update contract.
- **IMAP tests for #314** use same-mailbox COPY/MOVE (`INBOX` → `INBOX`).
  Cross-mailbox paths are covered by materializer unit tests that seed a
  second ordinary mailbox. Config-provisioned Trash/Sent/Archive is a
  follow-up.
- **Dovecot plugin** (later PR):
  - EXPUNGE push uses a per-transaction `expunges_requested` flag, not
    `MAIL_INDEX_TRANSACTION_CHANGE_EXPUNGE` (IMAP never sets that bit).
  - When `.copy` has `moving`, record MOVE source UIDs on the source
    transaction; `.expunge` skips those UIDs. Only the daemon MOVE record
    removes them, so a failed same-mailbox MOVE cannot lose the sources.
  - Fill `saved_uids` / UIDVALIDITY in `transaction_save_commit_pre` for
    APPENDUID and COPYUID; do not `mail_index_append` those UIDs—the next
    sync's `sync_append_new` appends them in order with any concurrent
    deliveries.
  - Free the save context on every exit path, including a failed
    `commit_pre`.

Normative behavior lives in `docs/contracts/core-daemon-api.md`,
`docs/contracts/mailbox-storage-model.md`, `docs/contracts/mutation-journal.md`,
`docs/contracts/core-schema.md`, and ADR 0003.

## Alternatives Considered

- **Hard-delete memberships or a separate `membership_removals` table.**
  Rejected: hard-delete breaks delivery re-apply under holds; a separate table
  still needs a schema rebuild for `journal_item`, so tombstone columns are
  cheaper.
- **UIDs only in the success response, not in the journal payload.** Rejected:
  the journal would not record what APPENDUID/COPYUID promised the client.
- **One journal record per copied UID.** Rejected: without a batch append API,
  multi-UID MOVE would not be atomic across a crash.
- **Chunked APPEND upload inside #314.** Rejected: reject oversize with
  `[LIMIT]` now; chunked upload (delivery and APPEND) is a follow-up.
- **Config-provisioned `MailboxDeclared` special-use mailboxes in #314.**
  Rejected for this issue's non-scope; file as a follow-up so real clients get
  Trash/Sent/Archive.
- **Mapping virtual writes to fact mutations.** Rejected: Datalog rules are
  not invertible.

## Consequences And Known Gaps

- Stacked implementation PRs must follow contracts-first: schema v12 and
  journal codecs, daemon services, view refresh (PR4) that hides messages with
  no visible ordinary membership, then the Dovecot plugin and scripted IMAP
  tests.
- Same-mailbox COPY/MOVE meets the letter of IMAP acceptance tests but does
  not exercise Trash/Sent. Operators still need a follow-up for
  config-provisioned Archive/Trash/Sent mailboxes.
- Quota and notify plugins that require a temporary index sequence for
  `dest_mail` may need the imapc-style temporary-append pattern, or stay
  unsupported until documented otherwise.
- Physical object GC remains governed by the closed #15 reachability work;
  this ADR only feeds it through visible ordinary membership counts.
