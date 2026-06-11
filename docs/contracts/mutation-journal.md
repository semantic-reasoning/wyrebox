# Mutation Journal Contract

## Status

Accepted for the mutation-journal implementation unit of issue 0003. This
contract fixes the minimal append-only journal behavior needed before the first
journal writer, replay worker, or DuckDB materializer is implemented.

## Scope

This contract defines only the canonical mutation journal contract. It covers
writer ownership, journal root policy, append-only record semantics, minimal
record envelope fields, canonical event names, durability, replay, object-store
consistency boundaries, and immutable raw-object rules.

The following details are intentionally out of scope for this contract:

- C implementation.
- DuckDB DDL or schema.
- daemon socket API.
- Cap'n Proto schemas.
- Postfix or Dovecot integration.
- Wirelog rule execution.
- compaction implementation.

## Writer Ownership And Authority

`wyreboxd` is the only writer of the canonical append-only journal. Postfix
helpers, Dovecot backend code, tests that are not journal-unit tests, migration
tools, and administrative utilities must not append canonical journal records
directly.

DuckDB is materialized query and index state fed by journal replay, catch-up,
and later compaction. DuckDB is never the synchronous mutation authority, and a
DuckDB write alone must not make a mutation durable or visible as canonical
WyreBox state.

## Journal Root And Path Policy

The production journal root is durable WyreBox state under
`/var/lib/wyrebox/`. The default journal directory is
`/var/lib/wyrebox/journal`.

The journal component must accept a caller-provided root directory. Tests,
developer tools, and isolated integration runs may use temporary roots instead
of `/var/lib/wyrebox`, but this override changes only storage placement and not
the append-only, durability, writer-ownership, or replay contract.

Concrete segment naming and rotation policy are deferred. Every journal path
used by the first implementation must remain below the configured journal root.

## Append-Only Record Model

The journal is an append-only sequence of records. Once a record is durably
committed at a journal offset, that record's bytes are immutable and must not be
rewritten in place for flags, keywords, mailbox moves, facts, derived views,
materialization, or compaction.

A journal offset is the stable byte offset of a record from the beginning of
the logical journal stream. Offsets are stable external references for logs,
replay checkpoints, and materialization checkpoints. Segment rotation may map a
logical offset to a segment-local location, but it must not renumber committed
records.

The first implementation uses a strictly increasing sequence number in each
record envelope. Replay validates that sequence numbers increase by one in
journal order. The sequence number is a replay integrity aid; the stable
journal offset remains the durable position identifier.

## Record Envelope

Each journal record has a minimal envelope containing:

- `version`: the journal record format version.
- `event_type`: one canonical event name.
- `payload`: event data encoded as payload bytes or a structured payload.
- `sequence`: the strictly increasing record sequence number.
- `checksum`: a checksum over the envelope fields and payload needed to detect
  torn, corrupt, or mismatched records.

The initial checksum algorithm and binary encoding are deferred to the C
implementation unit. The chosen encoding must preserve enough length and
checksum information for replay to detect an invalid trailing record rather
than silently accepting partial bytes as a valid mutation.

## Canonical Event Names

The required canonical event names are:

- `MessageDelivered`
- `FlagChanged`
- `KeywordChanged`
- `FactInserted`
- `FactRetracted`
- `DerivedViewMembershipChanged`

Additional event names require an update to this contract or a follow-up
contract before implementation.

## Durability Semantics

Delivery success requires both durable raw object storage and durable journal
append of the corresponding `MessageDelivered` event. `wyreboxd` must not return
delivery success before the raw object bytes are durable and the journal record
has reached the configured durable commit boundary.

For delivery events, group commit is allowed only when the daemon waits for the
group's durable journal commit before returning success for any delivery in
that group. A delivery request whose journal record has not reached the durable
commit boundary must be reported as temporary failure or remain incomplete, not
as success.

Flag and keyword mutations are canonical only after the corresponding
`FlagChanged` or `KeywordChanged` record is durably appended. Group commit is
allowed for flag and keyword mutations if the caller does not receive durable
success until the grouped records are durable. Deferred acknowledgement of flag
or keyword success is allowed; acknowledging success before durable journal
append is not allowed.

## Replay Behavior

Replay processes valid records in journal order from the beginning of the
logical journal stream or from a validated checkpoint. Replay applies only
records whose envelope, sequence, length, event type, and checksum are valid.

A torn or invalid trailing record must be detected explicitly. Replay may stop
at the last valid committed record and quarantine, truncate, or report the
invalid trailing bytes according to the later implementation policy, but it
must not apply the torn or invalid trailing record as a mutation.

Journal replay must be able to restore in-memory hot state and feed DuckDB
materialization after restart. DuckDB catch-up and rebuild consume the journal
as input; DuckDB contents do not replace the journal as the source of canonical
mutation truth.

## Object Store Consistency Boundary

The raw object for a `MessageDelivered` event must exist durably in the object
store before the `MessageDelivered` journal record is durably committed.

An orphaned durable object without a committed `MessageDelivered` event is
recoverable garbage, not delivered mail. Recovery may keep it for later
deduplication or remove it during cleanup, but mailbox-visible delivery is
derived from the committed journal event, not from object presence alone.

If a `MessageDelivered` record references a missing raw object during replay,
replay must treat the condition as journal/object-store inconsistency requiring
repair or operator-visible failure. It must not synthesize delivered mail from
missing raw bytes.

## Raw Object Immutability

Raw RFC 5322 object bytes are never rewritten for journaled mutations. Flags,
keywords, mailbox moves, facts, virtual mailbox membership, derived view
membership, DuckDB materialization, Wirelog-derived state, and compaction must
record their changes outside the raw object byte stream.

The mutation journal records state changes that refer to immutable raw objects;
it does not mutate the raw objects themselves.
