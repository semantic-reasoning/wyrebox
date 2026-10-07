# ADR 0003: Delivery Materialization Isolation And Retry

## Status

Accepted for issue #317. Amended by issue #331 (object, catalog, and storage
recovery error classification). `.internal-docs/` is not part of the
repository, so this ADR together with the referenced contract sections is the
decision record.

## Context

`wyreboxd` acknowledges a delivery after the raw object and the
`MessageDelivered` journal record are durable, then materializes journaled
deliveries into each account's INBOX in DuckDB. Catch-up resumes from one
global `materialization_checkpoint`, groups consecutive records into
per-account runs, and applies each run in one DuckDB transaction that also
advances the checkpoint.

Before this decision the first run that could not be applied stopped the whole
pass. A deterministic problem in one account, such as an INBOX row that is not
selectable or a `mailbox_uid_state` row with an unexpected UIDVALIDITY, blocked
every account behind it:

- deliveries were still acknowledged, but no account saw new mail;
- the main-loop retry warned every 5 s with no backoff or limit;
- the next restart exited with `EX_DATAERR`, so all later deliveries
  temporarily failed.

Transient causes, such as a locked DuckDB file or an I/O error, also exited
with `EX_DATAERR`, which is wrong for a supervisor restart policy.

## Decision

### Per-account hold, kept in memory

A catch-up pass holds an account instead of stopping:

- A run that fails with `G_IO_ERROR_INVALID_DATA` holds its account for the
  rest of the pass. The run's transaction is rolled back, and every later run
  of that account in the pass is skipped, so a held account never receives
  records out of journal order.
- DuckDB constraint violations raised while writing a run's account-owned rows
  are reported as `G_IO_ERROR_INVALID_DATA`, because they are deterministic for
  that account. Checkpoint writes are not classified this way.
- Any other error while applying a run (DuckDB I/O, lock, or connection
  failure) aborts the whole pass. These failures are global and would affect
  every later account too.
- Failures before any run is applied still abort the pass: metadata load,
  unsafe journal suffix, checkpoint seek, projection replay (including a
  missing or corrupt raw object), and a record without an account identity.

Holds are not persisted. Every pass re-derives them from the journal and the
catalog, so a fixed INBOX recovers automatically on the next pass, and a
restart rediscovers a hold that still exists.

### Checkpoint invariant

The checkpoint advances only inside a run's transaction, and only while the
pass reads from the persisted checkpoint and has no held account, including
holds carried into a post-ingest pass. After the first hold, later
healthy runs commit their memberships without moving the checkpoint. The
checkpoint therefore stops just before the earliest held record, never leads
an unapplied record, and never moves backwards.

Healthy accounts' records after a hold are re-read on later passes. Applying
an already materialized record is idempotent: the object, message, header, and
membership rows must match exactly, and an existing membership consumes no
UID. This relies on delivery-created `mailbox_memberships` rows never being
deleted or hidden. Any later EXPUNGE or MOVE design must keep that property,
for example with tombstone rows, before it lands.

### UID determinism

UIDs are assigned per mailbox in that mailbox's journal order. A healthy
account's UIDs do not depend on other accounts. A held account receives
nothing at or after its earliest held record and, once recovered, receives its
records in journal order. Restart and catalog rebuild therefore converge on the
same UIDs. Without holds, materialization output is unchanged.

### Scan cursor

Re-reading the journal from the checkpoint on every delivery while an account
stays held costs work proportional to the backlog. The daemon keeps an
in-memory scan cursor at the last record it processed:

- Post-ingest passes continue from the cursor, skip runs of already held
  accounts, and may hold more accounts. They advance the checkpoint only while
  no account is held and the cursor equals the checkpoint.
- The retry timer runs a full pass from the persisted checkpoint with an empty
  hold set, which replaces the hold set and moves the cursor to the end of
  that pass, or clears it when the pass is clean.
- A pass that aborts resets the cursor to the persisted checkpoint and keeps
  the previous hold set.
- The cursor and hold set are lost on restart; the startup pass is a full pass.

The journal safe-prefix scan still reads the whole journal on every pass, and
a full pass still revalidates raw objects from the checkpoint onwards, so a
long-lived hold makes retries and startup slower. Holds should be fixed
promptly.

### Retry backoff and logging

- A retry is scheduled when a pass aborts or ends with at least one held
  account. The first interval is 5 s, each retry doubles it, and it is capped
  at 5 minutes. A clean pass, with no abort, no held account, and reading from
  the persisted checkpoint, resets the interval and cancels any pending retry.
  Other post-ingest passes never reschedule or reset a pending retry, so steady
  delivery traffic cannot postpone it.
- Each retry uses a one-shot timer; the next one is scheduled only if the
  retry fails again.
- Warnings carry the account, journal offset, journal sequence, and error.
  A warning is logged when the failure state changes (a new held account, a
  cleared hold, or a different error) and otherwise at most once per cap
  interval as a summary. Repeated identical failures are logged at debug
  level. Recovery is logged as a message.
- The daemon exposes the materialization state (OK, retrying, or held), the
  consecutive failure count, the next retry interval, the held accounts, and
  the last error through a GObject status getter. An operator-facing status
  operation is follow-up work (#330); until then the warnings are the
  operator interface.

### Startup policy and exit codes

- A held account at startup is not fatal. `wyreboxd` logs the hold, starts
  serving, and schedules the retry. This replaces the #307 decision that any
  startup catch-up failure is fatal, for account-level problems only.
- A startup catch-up abort or a catalog preparation failure is fatal:
  - `G_IO_ERROR_INVALID_DATA` and `G_IO_ERROR_NOT_SUPPORTED` exit with
    `EX_DATAERR` (65). These are permanent: corrupt or account-less journal
    data, an unsafe journal suffix, a catalog schema newer than supported, or
    a catalog migration that needs an offline checkpoint.
  - Every other error exits with `EX_TEMPFAIL` (75), for example a locked
    DuckDB file or an I/O error.
- `wyreboxd` never runs checkpoint-requiring catalog migration steps itself,
  so a catalog migration step whose checkpoint precondition is not met fails
  with `G_IO_ERROR_NOT_SUPPORTED` and exits with 65. Callers that assert the
  precondition without materialization checkpoint metadata get
  `G_IO_ERROR_INVALID_DATA`. No shipped command runs these steps; rebuilding
  the catalog from the journal is the supported recovery.
- The shipped `wyreboxd.service` sets `RestartPreventExitStatus=65`, so
  systemd keeps restarting after transient failures but not after permanent
  ones.

### No new identifier

Holds are keyed by account identity and reported with journal offset and
sequence, which already identify a delivery. No new identifier is introduced,
so no UUIDv7 dependency is added.

## Alternatives Considered

A persisted per-account hold or per-account checkpoint table was rejected:

- it needs a schema version bump and migration, and changes recovery
  validation, admin checkpoint reporting, and the backup-restore contract;
- replay still has to start at the minimum position across accounts, so it
  saves no journal reading;
- it is materialized state about materialized state: a rebuilt or restored
  catalog would drop or carry stale holds.

## Consequences And Known Gaps

- A missing or corrupt raw object for one delivery still stops every pass,
  because object validation runs before records are grouped by account.
- Object-store read errors are reported as invalid data, so a transient
  object read failure at startup exits with `EX_DATAERR`.
- DuckDB open failures are not split into lock conflicts and corrupt or
  incompatible catalog files, so a corrupt catalog also exits with
  `EX_TEMPFAIL` and systemd keeps restarting until its start rate limit stops
  it.
- Delivery storage recovery and validation, which runs before catalog
  preparation, keeps exiting with `EX_DATAERR` for every error, including
  transient journal or object I/O errors, so systemd does not restart after
  them.
- The object, catalog, and storage recovery gaps above are tracked in #331.
- `wyrebox-admin materialization-checkpoint` reports the latest
  materialization manifest, not the delivery `materialization_checkpoint`
  row, and cannot open the catalog while `wyreboxd` runs (#332).
- There is no supported catalog rebuild command; the troubleshooting runbook
  describes a manual rebuild (#333).
