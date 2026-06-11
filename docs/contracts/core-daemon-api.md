# Core Daemon API Contract

## Status

Accepted for issue 0004. This contract defines only the first API slice for the
core daemon API transport and local access semantics.

## Scope

This contract defines:

- UDS transport and local access assumptions for first-slice client traffic.
- Cap'n Proto envelope/framing categories.
- Request identity and correlation fields exposed to callers.
- Caller-observed success semantics.
- State authority boundaries for Postfix helpers, Dovecot plugins, and local tools.

It does not define command payload schemas, daemon runtime internals, or command
query implementation.

Error-class semantics are defined by `docs/contracts/error-model.md`.

## Unix Domain Socket Transport

The first API transport is Unix domain socket only, at
`/run/wyrebox/wyrebox.sock`.

- No TCP listener.
- No TLS.
- No HTTP endpoint.
- No remote authentication model.
- No LMTP transport in this slice.

The first transport requirement for this phase is local socket IPC only.

## Local Access And Peer Identity

Postfix helpers, Dovecot plugins, and local tools/skills must connect to
`/run/wyrebox/wyrebox.sock` to access daemon operations.

Where available, peer credential checks are used (for example, Linux peer
credential passing) and local access assumptions are enforced through Unix socket
ownership/group/mode and group-based authorization aligned with Linux runtime
contract expectations.

State access through direct mutable handles in this local access path is not
allowed.

## Cap'n Proto Framing Boundary

Cap'n Proto is the first wire format on the socket transport.

Envelope and framing are contract categories in this slice, not concrete field
layouts:

- request frame
- response frame
- error frame
- stream/chunk frame

Only the category taxonomy is defined here, so that transport framing remains
stable while payload schemas are introduced in later slices.

## Request Identity And Correlation

Every request uses request identity at the envelope level:

- required `request_id`.
- `delivery_id` and queue identifiers where available for delivery ingress.
- IMAP operation correlation IDs where the caller stack can supply them.
- for durable mutation operations, daemon success responses include `journal_offset`
  (or equivalent durable marker) when the caller path can surface it.

These identifiers are used for log correlation, retry de-duplication support, and
error-path inspection.

## Caller-Observed Success Semantics

Caller success is defined by a definitive daemon success response for the matching
request identity.

Ambiguous transport outcomes are not caller success:

- response loss,
- connection loss,
- request timeout,
- restart/reconnect interruption.

Even if the daemon may have committed internally before a response loss, the
caller must not treat the operation as success without the definitive response.

Permanent validation or configuration failures remain explicit errors and are not
silently promoted.

## State Authority Boundary

`wyreboxd` remains the only mutable owner in the first architecture slice.
Clients in this API phase are consumers of daemon operations, including:

- Postfix helpers
- Dovecot plugins
- local tools and skills

These callers must not open or mutate DuckDB materialized state, Wirelog state,
object-store metadata, or canonical journal state directly.

State access is mediated by daemon operations only.

GLib/GObject implementation style may be used by future implementers, but this
contract intentionally defines no runtime implementation details.

## Deferred Operation Payloads

Command payload schemas and operation groups are deferred to later issue-0004
units:

- delivery ingestion
- fetch
- mailbox list/select
- flag/keyword update
- search
- fact insert/retract
- Wirelog predicate query
- safe DuckDB query templates

Query-safety policy details are also deferred in this slice; in this slice, no
arbitrary write SQL is allowed over the daemon API.

## Out Of Scope

The following are out of scope for this slice:

- concrete Cap'n Proto message layouts
- concrete request/response/error/query payload schemas
- TCP, TLS, HTTP, LMTP, and remote authentication
- public remote API exposure
- daemon internals, including replay internals and storage-engine startup logic
