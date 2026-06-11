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

Clients reconnect to the socket after restart. A Postfix helper must not
report delivery success unless it received a durable success response from
`wyreboxd`. If restart interrupts a delivery request before that response, the
helper reports temporary delivery failure so Postfix can retry.

Dovecot fetch and search operations interrupted by restart return an
IMAP-visible temporary backend failure. Dovecot code must not bypass the daemon
to read mutable state during restart.

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
