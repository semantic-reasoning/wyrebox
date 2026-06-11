# ADR 0001: Postfix Ingress Boundary

## Status

Accepted for the first implementation slice.

## Context

WyreBox keeps Postfix as the SMTP ingress layer and does not implement an SMTP
server for the first prototype. Postfix must hand accepted message bytes and
envelope context to a WyreBox integration, and that integration must preserve
the storage rules: raw RFC 5322 message bytes are immutable, and `wyreboxd` is
the only writer of canonical WyreBox state.

The first implementation needs a narrow boundary that can prove durable
delivery semantics without committing the project to the final production
transport. It must also avoid depending on Dovecot LMTP before the WyreBox
Dovecot storage backend has proven mailbox, fetch, flag, search, and virtual
view behavior.

## Decision

The first Postfix ingress implementation path is a `pipe(8)` delivery helper.
The helper receives a message from Postfix, forwards the message and envelope
metadata to `wyreboxd`, and exits with a Postfix-compatible success,
temporary failure, or permanent failure result.

LMTP ingress is later additive work, not a replacement decision now. A later
WyreBox LMTP endpoint may improve recipient-level status handling and remove
some process-per-delivery overhead, but it must use the same durable ingestion
transaction boundary as the `pipe(8)` helper.

Dovecot LMTP is deferred until the WyreBox Dovecot storage backend is proven.
The project must first validate WyreBox-owned mailbox identity, stable UID and
UIDVALIDITY behavior, original byte fetch, flag and keyword updates, basic
search, and first-class virtual mailbox views through the storage backend.

## Helper Input Expectations

The initial helper expects Postfix to provide the complete message on standard
input as the original RFC 5322 byte stream to ingest. The helper must not parse
and rewrite the message as part of delivery; any validation or canonicalization
belongs behind the daemon API and must not change the immutable raw object.

The helper command line or environment is expected to include configured
delivery metadata such as sender, recipient, queue ID when available, and any
site-specific delivery identifier configured for operational tracing.

## Envelope Metadata Assumptions

Postfix remains authoritative for SMTP envelope handling before the helper is
invoked. WyreBox assumes the helper can pass at least:

- the envelope sender;
- one target recipient for the current delivery attempt;
- the Postfix queue ID when configured and available;
- a delivery ID if the integration is configured to generate or receive one.

The helper must receive these values through pipe arguments, environment
variables, or configured metadata. It must not infer required envelope metadata
from message headers or body content alone.

The first helper contract is per delivery attempt. Multi-recipient fanout and
recipient-level status behavior are not required from the `pipe(8)` slice and
belong to the later LMTP design.

## Durable Ingestion Success Boundary

Postfix success is returned only after `wyreboxd` confirms durable raw object
storage and durable journal append for the delivery. DuckDB materialization,
Wirelog derivation, search indexing, and other asynchronous views may lag this
boundary, but they must be recoverable by journal replay.

The helper must treat loss of the daemon response, loss of the socket, or an
ambiguous daemon response as not durably accepted by WyreBox.

## Failure Mapping

A daemon-confirmed success maps to Postfix delivery success.

A temporary failure maps to Postfix retry behavior. Temporary failures include
an unavailable `/run/wyrebox/wyrebox.sock`, daemon overload, transient storage
errors, permission-unavailable socket access, interrupted daemon communication,
or any error where retry may later succeed without changing the message or
envelope.

A permanent failure maps to Postfix permanent failure behavior. Permanent
failures are limited to validation failures and configuration failures that
`wyreboxd` classifies as not retryable for the submitted message and envelope,
such as an invalid recipient for the configured WyreBox account mapping or a
message rejected by a durable policy decision.

## Duplicate Delivery Risk

The `pipe(8)` helper may experience an ambiguous result after sending the
message to `wyreboxd`, especially if the daemon commits durable state but the
helper exits with a temporary failure because the response was lost. Postfix may
then retry the delivery, creating duplicate delivery risk.

The first slice accepts this risk as an integration constraint and requires the
helper to include queue ID, sender, recipient, delivery ID where available, and
daemon journal offsets in logs. A later ingestion contract may add explicit
idempotency keys or duplicate suppression, but that is not part of this ADR.
Because duplicate delivery is expected under retries, handling for any delivery
identity that a later contract treats as stable must be idempotent or detect
duplicates before exposing duplicate state.

## Chroot And Privilege-Drop Implications

Postfix deployments may run delivery helpers with restricted users, groups, and
chroot settings. The `pipe(8)` helper must be installable so it can execute
with the intended privilege drop and still reach the daemon socket.

If a Postfix chroot is used, the administrator must either expose
`/run/wyrebox/wyrebox.sock` inside that chroot or configure the transport so the
helper can access the real runtime path. The helper must not require direct
filesystem access to DuckDB files, Wirelog runtime files, raw objects, object
metadata, or the journal.

## Daemon Socket Dependency

The first daemon API transport for Postfix ingress is the Unix domain socket at
`/run/wyrebox/wyrebox.sock`. The helper depends on that socket and must fail
temporarily when it cannot connect, cannot complete the request, or cannot
receive a clear durable result from `wyreboxd`.

## State Mutation Boundary

Postfix helpers must not mutate DuckDB, Wirelog, object-store metadata, or the
canonical mutation journal directly. They must not open mutable WyreBox storage
state and must not become a second writer.

Only `wyreboxd` writes canonical state: durable raw object storage,
object-store metadata owned by WyreBox, the canonical mutation journal, DuckDB
materialized tables, and Wirelog fact/rule runtime state.

## Deferred Decisions

Dovecot storage-backend decisions are deferred to ADR 0002 and left for the
next atomic unit. This ADR does not define the Dovecot mail storage backend,
UID ownership, UIDVALIDITY behavior, flag update mechanics, search hooks,
virtual mailbox view exposure, or Dovecot LMTP as a canonical ingress path.

LMTP endpoint details are deferred to later additive work. That later work must
document endpoint shape, recipient-level status behavior, process model
differences from `pipe(8)`, and compatibility with the same daemon ingestion
transaction.

## Consequences

This decision creates a small first implementation path: a Postfix `pipe(8)`
helper talks to `wyreboxd` and maps daemon results to Postfix delivery results.
It keeps the durable ingestion boundary clear while avoiding early commitment
to Dovecot LMTP or a full LMTP server.

The tradeoff is that the first path has weaker recipient-level status behavior
and an explicit duplicate delivery risk under ambiguous helper or daemon
failures. Those gaps are documented follow-up concerns rather than reasons to
delay the first integration boundary.
