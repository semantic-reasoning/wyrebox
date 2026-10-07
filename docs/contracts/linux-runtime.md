# Linux Runtime Contract

## Status

Accepted for issue 0002. This issue approves
`docs/contracts/linux-runtime.md` as a public contract despite the general
documentation guidance that public docs are created later from accepted
internal decisions.

This contract fixes the Linux runtime assumptions that later daemon,
Postfix integration, Dovecot backend, systemd, and packaging work must obey.

## Scope

This contract defines the Linux runtime contract only. It covers daemon
identity, socket access, failure classification, the systemd-first operating
model, filesystem locations, state ownership, and backup/restore units at the
contract level.

The following details are intentionally out of scope for this contract:

- systemd unit files.
- tmpfiles.d files.
- sysusers.d files.
- package scripts or specs.
- install rules.
- source code.
- Postfix or Dovecot configuration files.
- runtime directory creation code.
- packaging dependency inventory.

## Daemon Identity And Socket

`wyreboxd` runs as user `wyrebox` and group `wyrebox`.

The default daemon socket is `/run/wyrebox/wyrebox.sock`. The socket owner is
`wyrebox`, socket group is `wyrebox`, and the expected mode is `0660`.

The socket is the first daemon API transport. It carries the controlled
Cap'n Proto API over a Unix domain socket. TCP API support remains out of
scope until authentication, authorization, and query safety controls are
designed.

## Postfix And Dovecot Access Model

Postfix helpers and Dovecot plugins connect to `/run/wyrebox/wyrebox.sock`
only. They must never open mutable DuckDB, Wirelog, object store metadata, or
journal state directly.

Postfix and Dovecot runtime user or group access to WyreBox is managed through
Unix socket permissions. Later packaging or deployment work may choose the
specific group-membership mechanics needed for Postfix chroot and Dovecot
privilege-separation layouts, but the access boundary remains the daemon
socket.

Postfix delivery helpers pass delivery metadata and message bytes to
`wyreboxd`. Dovecot backend code reads and mutates mailbox-visible state
through daemon API operations. Neither integration has a direct mutable state
fallback.

## Failure Classification

Permission failures are classified distinctly from daemon success, daemon
temporary failure, and daemon permanent failure. Socket connection errors,
permission denials, ownership or mode mismatches, stale socket failures, and
ambiguous communication loss are transport/access conditions, not successful
daemon responses.

For Postfix, when a transport/access condition prevents a durable daemon
success response, Postfix delivery maps the condition to temporary delivery
failure and retry. For Dovecot, Dovecot fetch and search map the condition to
an IMAP-visible temporary backend failure.

Permanent delivery failure is reserved for explicit daemon validation or
configuration responses that are documented as non-retryable. A helper or
plugin must not reinterpret permission failure as durable daemon success.

## Socket Unavailable And Stale Socket Behavior

Socket unavailable means the socket path is missing, connection is refused,
the listener cannot complete a valid daemon handshake, the connect attempt
times out, or the connection is lost before the request has a definitive
daemon response.

Postfix delivery maps socket unavailable to temporary delivery failure and
retry. Dovecot fetch and search map socket unavailable to an IMAP-visible
temporary backend failure. Both paths use no direct state fallback.

A stale socket is a filesystem entry at `/run/wyrebox/wyrebox.sock` that does
not represent a live compatible `wyreboxd` listener. Only `wyreboxd` may
remove or replace a stale socket path. Postfix helpers and Dovecot plugins
report the transport/access failure through their normal temporary-failure
paths and do not unlink the socket.

On startup, `wyreboxd` owns stale socket recovery. It may remove a stale socket
after proving that no live compatible daemon owns it. If another live listener
already owns the path, startup must fail rather than stealing the socket.

## Daemon Restart Behavior

`wyreboxd` completes journal replay before accepting new socket requests.
Restart restores the in-memory hot state and DuckDB materialized state from
canonical state before clients observe normal service.

Delivery materialization for an account held under
`docs/adr/0003-delivery-materialization-isolation.md` may remain incomplete
after startup; `wyreboxd` serves other accounts and retries the held account.
Permanent startup materialization and catalog failures exit with `EX_DATAERR`
(65) and transient ones with `EX_TEMPFAIL` (75). The shipped
`wyreboxd.service` sets `RestartPreventExitStatus=65`, so systemd restarts
`wyreboxd` after transient failures, within its default start rate limit, but
not after permanent ones.

Clients reconnect to the socket after restart. A Postfix helper must not
report delivery success unless it received a durable success response from
`wyreboxd`. If restart interrupts a delivery request before that response, the
helper reports temporary delivery failure so Postfix can retry.

Dovecot fetch and search operations interrupted by restart return an
IMAP-visible temporary backend failure. Dovecot code must not bypass the daemon
to read mutable state during restart.

## Delivery Materialization Troubleshooting

This section is the operator runbook for
`docs/adr/0003-delivery-materialization-isolation.md`. Until a daemon status
operation exists, `wyreboxd` log lines are the operator interface.

Symptoms and log lines:

- A held account keeps accepting deliveries, but new mail does not appear in
  its INBOX. Other accounts are unaffected. `wyreboxd` logs a warning:
  `delivery materialization held account <account> at journal offset <offset>
  sequence <sequence>: <error>; retry in <n> ms`. The offset and sequence
  identify the first delivery of that account that the pass could not apply,
  and `<error>` names the cause. Several held accounts appear in one line,
  separated by `; `.
- A pass that cannot complete, for example because of a journaled delivery
  without an account or a catalog write failure, logs
  `delivery materialization failed: <error>; retry in <n> ms`. While it lasts,
  at least the deliveries from the failing point on, and possibly every
  delivery after the checkpoint, are not materialized for any account.
- `<n>` is the delay the pending retry was scheduled with, not the time left.
- The same failure is logged as a warning when it first appears or changes,
  and then at most once per maximum retry interval (5 minutes). Repeats in
  between are debug messages.
- During shutdown the suffix is `service stopped, no retry scheduled`.
- When a later pass succeeds, `wyreboxd` logs
  `delivery materialization recovered`.
- A startup failure is printed to stderr as
  `wyreboxd: delivery materialization failed: <error>` or
  `wyreboxd: catalog preparation failed: <error>`, without a retry suffix, and
  `wyreboxd` exits.

Diagnosis:

`wyreboxd` keeps the catalog open. Stop `wyreboxd` before inspecting the
catalog; Postfix defers deliveries while it is down. Read the restart caveat
under Recovery first: while `delivery materialization failed` warnings
continue, `wyreboxd` may not start again until the cause is fixed. A copy of
the catalog file and its `.wal` file taken while `wyreboxd` runs is not
consistent and may not open. Query the catalog with the DuckDB CLI in
read-only mode:

```sql
SELECT mailbox_id, imap_name, is_selectable, is_visible
FROM mailboxes WHERE account_id = '<account>';
SELECT namespace_id, uidvalidity, uidnext
FROM mailbox_uid_state
WHERE account_id = '<account>' AND namespace_kind = 'mailbox';
SELECT journal_offset, journal_sequence
FROM materialization_checkpoint WHERE checkpoint_key = 'materialization';
```

Common hold causes are an existing INBOX row that is not selectable or not
visible, and INBOX `mailbox_uid_state` that is missing, stale, or has an
unexpected UIDVALIDITY, and a raw object that is missing, unreadable, or does
not match the journaled size or SHA-256 key. The hold warning names the
journal sequence and the object key; restore the object file under the
object root from backup, unchanged, and the next retry materializes the
account. Raw objects are shared by content, so one bad object can hold several
accounts. The checkpoint stays just before the earliest held delivery.

Recovery:

- `wyreboxd` retries automatically: the first retry runs after 5 s, each
  failed retry doubles the delay, and the delay is capped at 5 minutes. New
  deliveries do not retry a held account.
- `wyreboxd` is the only catalog writer. Do not edit catalog rows by hand.
  The catalog is materialized from the journal, so the supported repair is a
  rebuild: stop `wyreboxd`, move the catalog file and its `.wal` file aside
  together and keep them until the rebuild is verified, then start `wyreboxd`.
  Startup replays the whole journal into a new catalog before it serves
  requests, and Postfix defers deliveries until then. Never remove the catalog
  file while leaving its `.wal` file in place. A rebuild does not clear a hold
  or failure caused by the journal data itself.
- Restarting `wyreboxd` runs a full pass immediately. This is safe when only
  hold warnings are logged: a hold that remains is logged again and does not
  stop startup. While `delivery materialization failed` warnings continue, a
  restart turns the runtime retry into a fatal startup failure, so fix the
  cause first.

Startup exit codes:

- `EX_DATAERR` (65) means a permanent problem: corrupt or account-less journal
  data, an unsafe journal suffix, a catalog schema newer than this build
  supports, a catalog migration that needs an offline checkpoint, or a
  catalog file that is not a valid or compatible DuckDB database. systemd
  does not restart `wyreboxd`. Read the `wyreboxd` journal for `catalog
  preparation failed`, `delivery materialization failed`, or `delivery
  storage is invalid`, and fix the cause before starting again. For
  `checkpoint precondition not satisfied` or an invalid catalog file, restore
  the catalog from backup or rebuild it as described under Recovery.
- `EX_TEMPFAIL` (75) means a failure that may clear by itself, such as another
  process holding the catalog open or an I/O error. systemd restarts
  `wyreboxd` within its start rate limit. If restarts keep failing, look for
  processes holding the catalog and check disk space and permissions.
- `EX_OSERR` (71) means `wyreboxd` could not open its object store, journal,
  catalog services, or socket, and `EX_CONFIG` (78) means the configuration is
  invalid. systemd restarts after both, within its start rate limit.
- The ADR lists the known misclassifications, for example transient object
  read errors exiting with 65.

## Permission Mismatch Behavior

A permission mismatch exists when the socket owner, group, or mode differs
from `wyrebox:wyrebox` and `0660`, or when the connecting Postfix or Dovecot
process lacks the required socket access.

The condition is reported as permission mismatch in operational logs and is
handled as a transport/access condition. Postfix maps it to temporary delivery
failure and retry. Dovecot maps it to an IMAP-visible temporary backend
failure.

The mismatch must not fall back to direct state access. Corrective action
belongs to service configuration, package setup, or local administrator
permissions, not to Postfix or Dovecot state-file access.

## Systemd Operational Model

The first-class operational model is systemd with `wyreboxd.service`.

The service model uses `RuntimeDirectory=wyrebox` for `/run/wyrebox/`,
`StateDirectory=wyrebox` for `/var/lib/wyrebox/`, and
`CacheDirectory=wyrebox` for `/var/cache/wyrebox/`.

Logging is journald first. If file logs are needed later, packaging may use
`LogsDirectory=wyrebox` or an equivalent `/var/log/wyrebox/` layout.

Socket activation is deferred. The initial service owns socket creation and
lifecycle directly after startup and replay are complete.

Non-systemd Linux support is deferred. Later work may define an alternate
supervision model, but it must preserve this identity, socket, filesystem, and
state-ownership contract.

## Filesystem Layout

The Linux filesystem layout is:

- `/run/wyrebox/` for runtime files, including `wyrebox.sock`.
- `/var/lib/wyrebox/` for durable WyreBox state.
- `/var/cache/wyrebox/` for rebuildable cache data.
- `/etc/wyrebox/` for configuration.
- optional `/var/log/wyrebox/` for file logs when journald is not enough.

Runtime files are not durable state. Durable state belongs under
`/var/lib/wyrebox/`, and rebuildable cache data belongs under
`/var/cache/wyrebox/`.

## State Subdirectories

At the contract level, durable and rebuildable state is divided into:

- objects for immutable RFC 5322 message bytes.
- DuckDB materialized store for query and index state.
- canonical journal for append-only mutation records.
- Wirelog facts and rules for Datalog-derived mailbox views.
- snapshots for restore and compaction checkpoints.
- cache for rebuildable temporary or derived data.

Concrete subdirectory names are deferred to later schema and packaging work,
but each category must remain separable for permissions, backup, restore, and
operational inspection.

## Backup And Restore Units

Backup and restore units are defined at the contract level as:

- objects.
- canonical journal.
- DuckDB snapshot.
- Wirelog facts and rules.
- snapshots.
- configuration.

The canonical restore path is objects plus canonical journal plus Wirelog facts
and rules plus configuration, with DuckDB materialized state rebuilt or
restored from a consistent DuckDB snapshot. Snapshot formats, backup tooling,
and restore commands are deferred.

## Canonical State Ownership

Only `wyreboxd` mutates canonical state. Canonical state includes the
append-only mutation journal, object-store metadata owned by WyreBox, Wirelog
facts and rules, and any daemon-owned state required to reconstruct mailbox
views.

DuckDB is a materialized query/index store fed by journal replay and compaction.
It must not become the sole synchronous mutation authority.

Postfix and Dovecot integrations must use daemon API operations for delivery,
fetch/search visibility, flag or keyword updates, and fact mutation. They may
hold transient request buffers and rebuildable protocol-layer caches, but they
must not mutate canonical state directly.

## Deferred Work

Follow-up issues must cover systemd units and package scripts.

Follow-up packaging dependency notes are deferred to a later atomic unit. That
later work should inventory Meson, Ninja, Cap'n Proto, GLib/GObject, DuckDB,
Wirelog, Postfix, Dovecot, licensing, and distribution-specific package
constraints without changing this runtime contract.
