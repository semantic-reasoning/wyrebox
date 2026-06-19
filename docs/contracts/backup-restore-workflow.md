# Backup Restore Workflow

## Status

Accepted for issue 0025. This contract defines the recovery boundary for
restoring WyreBox state and deciding when materialized state must be rebuilt.

## Scope

This contract covers only recovery semantics. It does not define any backup
CLI, restore CLI, network protocol, copy tool, or runtime implementation.

The recovery boundary is:

- immutable raw message objects
- canonical journal prefix
- schema metadata
- rule and view package identity
- materialized-state checkpoint metadata
- DuckDB materialized state as an optional acceleration layer

## Durable Backup Set

The minimal durable backup set must include:

- immutable raw message objects
- canonical journal data up to a closed safe prefix
- schema metadata needed to interpret journal records
- configuration needed to locate durable state
- rule and view package identity required to rebuild derived state
- materialization checkpoint metadata when present
- materialized-state checkpoint metadata when present
- DuckDB materialized state as an optional acceleration layer

DuckDB materialized tables are not required durable data. They may be restored
from snapshot for speed, but the canonical recovery path must not depend on
them.

## Restore State Machine

Restores proceed through these states:

- staged
- validated
- rebuilding materialized state
- serving-ready
- failed-retryable

A restore may enter rebuilding when the DuckDB catalog is missing, stale, or
incompatible but the canonical state is still safe.

## Recovery Decisions

If the canonical backup is incomplete or unsafe, recovery must fail with a
clear error.

If the DuckDB catalog is missing or a materialization checkpoint is missing,
recovery must prefer rebuild over data loss.

If the journal suffix is unsafe or a committed message references a missing raw
object, recovery must fail.

## Deferred Work

The first implementation phase does not define backup manifests, snapshot copy
commands, or restore orchestration. Those remain follow-up work after the
contract is accepted.
