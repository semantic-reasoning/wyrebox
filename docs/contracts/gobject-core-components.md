# GObject Core Components Contract

## Status

Accepted for issue 0003. This contract defines the first GObject core component
boundaries for the immutable-object and mutation-path stack.

## Scope

This contract defines component boundaries, ownership, and lifecycle rules only.
It is not an implementation contract.

The following details are intentionally out of scope:

- C implementation behavior.
- SQL schema DDL and migration execution details.
- runtime daemon API protocol design.
- Postfix integration behavior.
- Dovecot integration behavior.
- wirelog runtime internals.

## Source And Header Layout

The first-party component sources and headers for this contract are under
`wyrebox/` with public and internal headers colocated with their source files.
No top-level `include/` directory is introduced.

## Formatting And Style

Components in this contract use C and GLib/GObject style. Implementations must
follow `tools/gst-indent` style, use two-space indentation, and keep code style
consistent with existing WyreBox conventions.

Implementations must use `g_autoptr`, `g_autofree`, `g_auto`, and related
cleanup macros aggressively where ownership is not explicitly handed off through
documented API ownership rules.

## GObject Ownership Conventions

Public constructors for GObject component types return full ownership to the
caller.

Raw byte and input-object ownership is documented per API:

- In-memory RFC 5322 byte inputs use explicit non-owning read semantics unless the
  contract states otherwise.
- `GError` is the standard error channel for failure reporting.
- Out parameters must document whether ownership is transferred to caller.
- Finalizers release all resources owned by the instance, including GLib resources,
  file descriptors, and references needed for in-memory state.

## Local Object Store Adapter Boundary

The local object-store adapter owns immutable raw RFC 5322 objects and their
on-disk representation metadata. It does not own mailbox membership tables,
mailbox flags, UID state, keyword state, or derived memberships.

The object-store adapter owns durable object content only and must treat raw
objects as append-only immutable bytes.

## Mutation Journal Writer Boundary

The mutation journal writer component is the canonical write path for state
mutations.

`wyreboxd` is the only daemon-side caller that may append to the canonical journal
stream. Helpers, plugins, tests that are not journal-unit tests, and auxiliary
tools must not append canonical journal records directly.

Records are appended only and are durable-write complete before mutation success is
reported by the daemon.

## Journal Reader And Replay Worker Boundary

The journal reader and replay worker validates journal records, detects invalid
or torn trailing data, and replays only valid records in order.

The worker feeds downstream materializers and hot-state builders only. It must not
mutate raw object bytes as part of replay work.

## DuckDB Metadata Materialized Store Boundary

DuckDB stores materialized query and index state, not canonical mutation authority.

Canonical mutation truth is replayed into materializers. DuckDB is a derivative
materialized view layer and is not a source for append-only mutation authority.

## Migration Runner Boundary

Migration runner behavior is an explicit transition mechanism:

- known schema versions have explicit migration steps;
- upgrades are only allowed through documented transitions;
- unknown schema versions are rejected until a migration path is defined and approved.

There is no implicit unknown-version upgrade behavior.

## Snapshot Manager Boundary

The snapshot manager captures and restores materialized state, cursor/checkpoint
state, and replay boundary state for recovery paths.

Snapshots must not rewrite immutable raw RFC 5322 object bytes. Snapshot state
is for rebuild and restart acceleration, not for canonical mutation source-of-truth.

## Single-Writer And Authority Boundaries

`wyreboxd` owns:

- canonical journal append,
- DuckDB materialization pipeline,
- Wirelog runtime state transitions, and
- object-store metadata ownership.

Postfix and Dovecot helpers/plugins must not write canonical journal, DuckDB
materialized authority, Wirelog runtime state, or object-store metadata directly.
They must interact through daemon boundaries later.

## Recovery And Rebuild Boundary

Materialized state is rebuildable from canonical journal input, object store
metadata/content, and Wirelog-derived facts/rules.

Recovery paths use canonical replay and materialized rebuild, then continue from the
captured replay boundary.

Postfix and Dovecot integrations are expected to operate through daemon/API
boundaries in later units, not direct mutable state access.

## TDD And Validation

Each boundary in this contract receives focused tests before implementation.

Initial validation for this contract uses the docs test at
`tests/docs/test_issue_0003_gobject_core_components_contract.py`, and the docs
test is executed in Meson as `docs issue 0003 gobject core components contract`.
