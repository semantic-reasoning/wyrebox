# Core Schema Contract

## Status

Accepted for issue 0003. This contract defines the first DuckDB materialized
schema design at the contract level before SQL implementation details are
committed.

## Scope

This contract defines DuckDB materialized-schema shape and ownership rules only.
It does not define the implementation language, SQL runtime internals, or the
full query-safety policy engine.

The following details are intentionally out of scope:

- DuckDB C++ engine integration details.
- Wirelog query language implementation.
- Postfix or Dovecot transport wiring.
- Cap'n Proto socket schema.
- Daemon restart orchestration beyond materialization expectations.

## Canonical State And Materialized DuckDB Boundary

The append-only mutation journal is the canonical WyreBox write source.
`wyreboxd` is the only component that appends canonical mutations.

DuckDB stores materialized query and index state derived from canonical sources.
DuckDB is not the canonical mutation authority.

Raw RFC 5322 objects remain immutable in the object store and are never the
mutation source for flags, mailbox moves, facts, or virtual views.

## Stable Identity And Reference Model

Stable identifiers at contract level are:

- `message_id`: message entity identity owned by WyreBox.
- `object_id`: immutable object identity from content.
- `mailbox_id`: mailbox identity for each ordinary mailbox.
- `view_id`: virtual mailbox view identity.
- `membership_id`: one mailbox view membership instance.

Messages reference immutable objects by immutable `object_id`.
`object_id` is the immutable content-based object key produced by the object-store
contract.

Object references in schema records must never imply raw byte rewrites. A raw
object may be shared by many memberships without changing the object bytes.

## Required Materialized Schema Objects

A first-phase materialized schema must provide, at minimum, these logical tables
and views:

- `objects`: immutable object metadata keyed by `object_id`.
- `messages`: message records keyed by `message_id`, referencing
  `objects.object_id`.
- `mailbox_memberships`: message-to-mailbox or message-to-view links, carrying
  mailbox-scoped state such as UID, internal date, and fetch visibility.
- `mailbox_uid_state`: per-mailbox and per-view UID namespace state, including
  `uidnext` and `uidvalidity`.
- `message_flags`: mailbox-scoped flags materialized from canonical mutation
  records.
- `message_keywords`: mailbox-scoped keywords materialized from canonical
  mutation records.
- `message_facts`: fact rows materialized from fact mutation records and derived
  sources.
- `derived_view_memberships`: Wirelog-derived membership rows for virtual mailbox
  views.
- `schema_metadata`: schema contract metadata including schema version.
- `materialization_checkpoint`: progress and replay position of canonical replay,
  including last journal byte offset and sequence.

`mailbox_memberships` and `derived_view_memberships` are the required
distinction between ordinary and virtual exposures.

## Mailbox And Virtual Mailbox UID Contract

Each ordinary mailbox and each virtual mailbox has its own UID namespace.
Each namespace owns its own `UIDNEXT` and `UIDVALIDITY` state in
`mailbox_uid_state`.

Virtual mailboxes are first-class WyreBox views and expose memberships through
`derived_view_memberships`.
They are backed by Wirelog-derived source membership facts and are not built by
duplicating raw message objects.

## Flags And Keywords As Materialized State

Flags and keywords are not stored in raw RFC 5322 objects.
They are represented as separate rows in materialized state tables and must be
derived from `FlagChanged` and `KeywordChanged` journal records.

Mailbox mutations can only change materialized flag/keyword rows and membership
rows as required by schema contracts.

## Schema Versioning And Migration Policy

`schema_metadata` stores an explicit `schema_version` value.

Schema changes must be applied through explicit migration steps that are
documented in adjacent planning artifacts and then reflected in `schema_version`.

A restart may run with a supported schema version, must reject unknown versions,
and must require an explicit migration for unknown schema jumps.

## Restart And Replay Regeneration Expectations

Materialized tables are rebuildable.
`wyreboxd` restart replay may rebuild all materialized state from canonical
journal plus object-store inputs and Wirelog fact/rule-derived sources where
applicable.

Replay must converge on equivalent `objects`, `messages`, `mailbox_memberships`,
`message_flags`, `message_keywords`, `mailbox_uid_state`, `message_facts`,
`derived_view_memberships`, and `schema_metadata` content for the same accepted
journal prefix.

`materialization_checkpoint` must track durable replay position and allow restart
to continue from the last fully materialized position.

## Query And API Safety Boundary

DuckDB query usage is read/materialized-query oriented in this contract phase.

Non-admin clients must not be able to execute arbitrary write SQL against the
materialized store.

Administrative write operations are explicit operations, not arbitrary SQL text.
Materialized state updates are driven by replay and migration steps under
`wyreboxd` control.
