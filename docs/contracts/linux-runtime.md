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
- When journaled deliveries reference raw objects that are missing or do not
  match the journaled size or SHA-256 key, startup logs `journaled deliveries
  with missing or corrupt raw objects: <n>, first at journal sequence
  <sequence>; delivery catch-up holds the affected accounts` and continues;
  the hold warnings name the accounts and objects. Deliveries that were
  materialized before the checkpoint are not held and are reported only by
  this warning; restore their objects the same way.
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
  supports, a catalog migration that needs an offline checkpoint, a
  catalog file that is not a valid or compatible DuckDB database, an invalid
  storage marker, or `storage markers do not match`, which means a volume
  from another installation or a mismatched restore is mounted. Mount the
  right volumes or restore a matching pair. systemd
  does not restart `wyreboxd`. Read the `wyreboxd` journal for `catalog
  preparation failed`, `delivery materialization failed`, or `delivery
  storage is invalid`, and fix the cause before starting again. For
  `checkpoint precondition not satisfied` or an invalid catalog file, restore
  the catalog from backup or rebuild it as described under Recovery.
- `EX_TEMPFAIL` (75) means a failure that may clear by itself, such as another
  process holding the catalog open, an I/O error, a raw object that exists
  but cannot be read (`failed to read raw object`), or an object store that
  is not mounted (`check that the object store is mounted`). Storage that is
  not initialized or not mounted is also 75: `storage is not initialized`
  means both markers are missing, so mount the storage volume or, on a new
  installation or an upgrade, run `--initialize-storage` as described under
  Systemd Operational Model; `storage marker <path> does not exist but <path>
  does` means one volume is not mounted, or an interrupted
  `--initialize-storage` needs to run again. systemd restarts
  `wyreboxd` within its start rate limit. If restarts keep failing, look for
  processes holding the catalog, check that the object store is mounted at
  `object_root_dir`, and check disk space and permissions. For `failed to
  read raw object`, fix the permissions or the disk for the named object.
- `EX_OSERR` (71) means `wyreboxd` could not open its object store, journal,
  catalog services, or socket, and `EX_CONFIG` (78) means the configuration is
  invalid. systemd restarts after both, within its start rate limit.
- The ADR lists the known misclassifications, for example DuckDB open
  failures with an unrecognised message exiting with 75.

## Rule-Derived Virtual Mailboxes

Virtual mailboxes are configured in the `wyreboxd` configuration file. The
`[wirelog]` section names one Wirelog rules file, and each `[view:<id>]`
section adds one virtual mailbox. `examples/wyreboxd/` has a complete example:

```ini
[wirelog]
rules_path=/etc/wyrebox/views.dl

[view:projects]
imap_name=Projects
scope=message
```

```
.decl has_keyword(message_id: symbol, keyword: symbol)
.decl show_in_virtual_folder(view_id: symbol, message_id: symbol)

show_in_virtual_folder("projects", message_id) :-
    has_keyword(message_id, "project").
```

Rules and configuration:

- `rules_path` must be absolute. Views require `rules_path`.
- A view id uses letters, digits, `-`, `_`, and `.`. Each view needs a
  unique `imap_name`; `INBOX` is reserved.
- Each view needs a `scope`, which names the facts its rules read to decide
  whether a message is a member:
  - `message`: the facts of the message itself, such as a project keyword or
    the sender domain.
  - `thread`: the facts of the messages connected to it. Messages are
    connected when they share a `message_id`, `replies_to`, or `references`
    value, directly or through other messages.
  - `account`: any fact of the account.

  `wyreboxd` does not check the scope against the rules. A rule that reads
  facts outside its view's scope is evaluated without them, so its view can
  miss members until the next full evaluation.
- The rules file must declare `show_in_virtual_folder(view_id: symbol,
  message_id: symbol)`. A message belongs to a view when the rules derive
  `show_in_virtual_folder("<view id>", <message id>)`.
- Message ids have the form `journal:<offset>:<sequence>`, as returned in the
  delivery receipt.
- Rules read the account's active facts, as inserted and retracted through
  the daemon fact mutation operation. Facts of predicates the rules do not
  declare are ignored, and so are derived message ids that do not exist in
  the account.
- A missing or unreadable rules file, rules that do not compile, or rules
  without `show_in_virtual_folder` stop startup with `EX_CONFIG` (78) and a
  message naming the file or the view.

Refresh:

- A fact insert or retract refreshes the views of its account before the
  response is sent. Deliveries refresh the views of their accounts in the same
  materialization pass.
- A refresh evaluates only the messages delivered, or whose facts changed,
  since the account's previous refresh: `message` views read those messages'
  facts, `thread` views the facts of the messages connected to them, and
  `account` views all facts of the account. The catalog records the journal
  sequence each account was refreshed to.
- When the rules file, a view, or a scope changes, the next refresh evaluates
  every message of the account again, a batch of whole threads at a time, so
  memory stays bounded by the batch rather than the account. A changed rules
  file takes effect after a restart; changed rules give the members of a view
  new UIDs.
- Virtual UIDs and UIDVALIDITY are stable. A message that leaves a view and
  comes back keeps its UID.
- A refresh failure holds the account, like a delivery hold. `wyreboxd` logs
  `held account <account> at virtual mailbox refresh: <error>` and retries
  with the same backoff. The `show_in_virtual_folder.v1` predicate query over
  the daemon socket returns the current derived memberships for inspection.

Delivery-time facts:

- `extraction_rules_path` in `[wirelog]` names an optional extraction rules
  file. It must be absolute and requires `rules_path`:

  ```ini
  [wirelog]
  rules_path=/etc/wyrebox/views.dl
  extraction_rules_path=/etc/wyrebox/extraction.rules
  ```

- When views are configured, `wyreboxd` extracts facts from every delivered
  message before refreshing its account. Header facts such as `message_id`,
  `replies_to`, `references`, `participant`, and `sender_domain` are always
  extracted. The rules file adds dictionary and regex rules, one group per
  rule, applied in file order:

  ```ini
  [dictionary:apollo]
  field=subject
  match=apollo
  project=apollo

  [regex:invoice]
  field=subject
  predicate=reference_candidate
  pattern=INV-[0-9]+
  capture_group=0
  ```

  A dictionary rule emits `project_keyword(message_id, project)` when `match`
  occurs in `field`, ignoring case. Fields, predicates, and matching follow
  `docs/contracts/deterministic-fact-extraction.md`. `pattern` is read
  verbatim, so backslashes need no escaping. `capture_group` defaults to 0.
- A missing or unreadable rules file, an unknown group or key, or an invalid
  field, predicate, or pattern stops startup with `EX_CONFIG` (78).
- The extracted facts of a message are appended as one `FactsExtracted`
  journal record, and the raw message object is not changed. Facts are
  extracted once per message: changed extraction rules apply to later
  deliveries, while existing messages keep their facts. Each startup extracts
  facts for messages that do not have them yet.
- Extracted facts are visible to the view rules like inserted facts, so a rule
  of a `thread` view can follow `replies_to` and `message_id` facts to place a
  whole thread in the view, whatever order its messages arrive in.
- An extraction failure, such as an unreadable message object, holds the
  account like a refresh failure. The log line reads
  `held account <account> at virtual mailbox refresh: fact extraction failed:
  message <message id>: <error>`.
- Virtual mailbox membership changes are journaled, so rebuilding the catalog
  from the journal reproduces the same facts, memberships, virtual UIDs, and
  UIDVALIDITY without appending new records.

Known limitations:

- Changing the `imap_name` of an existing view conflicts with the stored view
  and holds the account at refresh. Use a new view id instead.
- Startup checks every account for changes since its previous refresh,
  which grows with the number of accounts.

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

The shipped unit declares
`RequiresMountsFor=/var/lib/wyrebox/journal /var/lib/wyrebox/object-store`, so
systemd mounts the default storage roots before it starts `wyreboxd`. When the
configuration moves `journal_root_dir` or `object_root_dir`, add a drop-in
with `systemctl edit wyreboxd` that repeats `RequiresMountsFor=` with the
configured paths. `RequiresMountsFor=` only orders and requires mount units
that systemd knows about; the storage markers remain the guard against
starting on a volume that is not mounted (see
`docs/adr/0003-delivery-materialization-isolation.md`).

Storage initialization is an explicit, one-time operator step. Create the
mount points, mount the journal and object store volumes, give the mounted
roots to the service user, then initialize:

```sh
install -d -o wyrebox -g wyrebox -m 0750 /var/lib/wyrebox
install -d /var/lib/wyrebox/journal /var/lib/wyrebox/object-store
mount /var/lib/wyrebox/journal
mount /var/lib/wyrebox/object-store
install -d -o wyrebox -g wyrebox -m 0700 /var/lib/wyrebox/journal /var/lib/wyrebox/object-store
runuser -u wyrebox -- wyreboxd --initialize-storage --config /etc/wyrebox/wyrebox.conf
```

Skip the `mount` lines when the roots are plain directories. The root of a
newly created filesystem is owned by root, so without the ownership step
`--initialize-storage` exits with 71. Run it as the service user: run as
root, it leaves root-owned roots and markers that `wyreboxd` cannot read, so
startup exits with 75 until an operator runs `chown -R wyrebox:wyrebox` on
both roots. Never run it from `ExecStartPre=` or any other automatic hook,
because initializing on every start would turn an unmounted volume into a
new, empty installation. It prints the storage ID and exits; running it again
on initialized storage prints `storage already initialized` and changes
nothing.

Upgrading existing storage from a release without storage markers: stop
`wyreboxd`, check that both volumes are mounted, run the same
`--initialize-storage` command, then start `wyreboxd`. Until then startup
exits with 75 and logs `storage is not initialized`. The command adopts the
existing journal and objects when both hold data, and refuses when only one
does.

`--initialize-storage` exits with 0 when storage is initialized or already
was, 75 when a root looks unmounted (only one side holds data, or only the
object store marker exists), 65 when an existing marker is invalid or the
markers do not match, 78 when the configuration is invalid, and 71 for any
other failure, such as a permission or write error. When it refuses with 75:

- `check that the object store is mounted` or `check that the journal is
  mounted`: mount the named volume and run it again.
- Only a lone object store marker: mount the correct journal volume. If the
  journal really is new, move the object store marker aside only after
  confirming that the object store belongs to this installation.
- An empty journal and an object store that holds only empty shard
  directories, `.tmp-object-` files, or complete objects under
  `objects/sha256`, left by deliveries that failed before their journal
  append: move them aside and run it again.

After repeated 75 exits, for example while a volume is not mounted or
storage is not initialized after an upgrade, systemd stops restarting
`wyreboxd` at its start rate limit. Fix the cause, then run
`systemctl reset-failed wyreboxd` and `systemctl start wyreboxd`.

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
