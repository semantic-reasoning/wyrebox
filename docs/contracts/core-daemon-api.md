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
- Delivery ingestion operation contract.
- Mailbox LIST/SELECT operation contract.
- Message FETCH operation contract.
- Message SEARCH operation contract.
- Flag/keyword update operation contract.
- State authority boundaries for Postfix helpers, Dovecot plugins, and local tools.

It does not define concrete command payload schemas, daemon runtime internals, or
command query implementation.

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

## Delivery Ingestion Operation Contract

Delivery ingestion is the first operation contract for Postfix ingress helpers.
It is a daemon API operation over the Cap'n Proto-over-UDS transport; this
section does not define concrete `.capnp` field layouts or generated code.

Every delivery ingestion request carries ingress identity fields:

- required `request_id`;
- required `delivery_id`;
- queue ID when the Postfix caller has one available;
- envelope sender; and
- one or more recipients.

The raw RFC 5322 message payload has an explicit transfer boundary. The caller
sends the exact payload bytes through the API framing, and `wyreboxd` stores
those bytes as the canonical original message object. Delivery ingestion must
not rewrite raw message bytes for flags, mailbox membership, facts, search, or
virtual views.

A delivery ingestion success response is valid only after both required durable
steps are complete:

- durable raw object-store commit of the canonical original payload; and
- durable append of the corresponding mutation journal entry.

The success response includes a durable receipt, such as `journal_offset` or an
equivalent durable marker, that lets the caller and logs correlate accepted
delivery with the durable mutation position.

Retry and permanent failure semantics are governed by
`docs/contracts/error-model.md`: temporary failures are retryable; permanent
failure is reserved for explicit non-retryable validation, configuration, or
policy errors; and response loss, connection loss, timeout, or other ambiguous
communication is not delivery success.

This operation does not expose arbitrary SQL, write SQL, DuckDB mutation,
Wirelog fact mutation, object-store metadata mutation, or direct journal append
surfaces to helpers. It accepts delivery-ingestion inputs only.

## Mailbox List Select Operation Contract

Mailbox LIST and SELECT are the first Dovecot-facing LIST and SELECT calls over
the Cap'n Proto-over-UDS daemon API. This section defines operation behavior
only; it does not define concrete `.capnp` schemas, field layouts, generated
code, or Dovecot backend implementation.

Each mailbox LIST or SELECT request carries request identity and caller/account
identity sufficient for `wyreboxd` to scope mailbox visibility. The caller's
authorized account identity is the outer visibility boundary. A successful LIST
or SELECT response must not return mailbox records outside the authorized
account scope, and authorization failure is reported through the error model
rather than hidden behind direct store access.

Mailbox identity is WyreBox-owned and account scoped:

- ordinary mailboxes are returned with a stable mailbox_id value;
- virtual mailboxes are returned with a stable view_id value; and
- Virtual mailbox identity is first-class WyreBox mailbox identity, not a
  client-side fiction, search-result alias, or Dovecot-only virtual folder.

LIST returns mailbox records suitable for a Dovecot storage backend to translate
into IMAP-visible mailbox listing results. Each returned record includes:

- the mailbox kind, `ordinary` or `virtual`;
- the stable identifier for that kind;
- the stable IMAP-visible name;
- the hierarchy delimiter;
- attributes that distinguish selectable from non-selectable mailboxes; and
- children or no-children attributes where the hierarchy state is known.

SELECT opens one ordinary mailbox or virtual mailbox by stable identity or
IMAP-visible account-scoped name and returns selected-mailbox state sufficient
for IMAP serving. The selected-mailbox state includes at minimum:

- mailbox kind and stable identity;
- stable IMAP-visible name;
- `UIDVALIDITY`; and
- `UIDNEXT`.

Additional Dovecot status fields, such as message counts, unseen counts,
highest MODSEQ, special-use attributes, subscription state, or namespace policy
may be added by later operation slices without changing the ownership boundary
defined here.

Mailbox LIST and SELECT error behavior is governed by
`docs/contracts/error-model.md`:

- a mailbox absent from the authorized account scope is `not found`;
- account or mailbox authorization failure is an authorization/permission
  outcome;
- SELECT of a non-selectable hierarchy or container is Conflict as an
  operation-aware selection-state conflict, not success; and
- temporary backend failure remains a temporary failure visible to the Dovecot
  caller path.

LIST and SELECT are read-only daemon operations. They do not append mutation
journal records, and they do not mutate DuckDB, Wirelog, object-store metadata,
or journal state. They may read daemon-owned materialized state and derived view
state to describe ordinary and virtual mailbox visibility, but they do not
refresh Wirelog facts or directly write derived memberships.

This operation does not expose arbitrary SQL, write SQL, DuckDB mutation,
Wirelog mutation, object-store metadata mutation, direct journal append, or
direct journal write surfaces to Dovecot plugins or local callers. It accepts
mailbox LIST/SELECT inputs only.

## Message Fetch Operation Contract

Message FETCH is a Dovecot-facing FETCH call over the Cap'n Proto-over-UDS
daemon API. This section defines operation behavior only; it does not define
concrete `.capnp` schemas, field layouts, generated code, or Dovecot backend
implementation.

Every FETCH request carries request and caller identity sufficient for
`wyreboxd` to authorize and correlate the operation:

- required `request_id`;
- Dovecot/IMAP operation correlation ID where the caller stack can supply one;
- caller/account identity;
- stable selected-mailbox identity for an ordinary mailbox or virtual mailbox;
  and
- mailbox-scoped UID or equivalent message reference within that selected
  mailbox context.

The caller/account identity scopes all access. FETCH must not return messages
outside the authorized selected mailbox or virtual mailbox view. A message
reference is valid only inside the selected mailbox identity supplied by the
request; the same raw message may have different mailbox-scoped references in
ordinary and virtual mailbox contexts.

FETCH retrieves the byte-for-byte original RFC 5322 message from the canonical
object selected through daemon-owned mailbox state. It may use daemon-owned
materialized state, indexes, or caches to resolve the stable selected-mailbox
identity and message reference to the canonical object, but the returned body
bytes are the original immutable message bytes. FETCH must not rewrite raw bytes
for flags, facts, search, or virtual views.

Body bytes are returned with stream/chunk response framing. Each body chunk is
correlatable to the original `request_id` and selected message reference, and
the stream ends with a definitive end response or definitive error response for
that request/message. A caller must not infer success from partial chunks alone.

FETCH error behavior is governed by `docs/contracts/error-model.md`:

- an absent mailbox, selected mailbox outside the authorized scope, or absent
  message reference in scope is `not found`;
- account, mailbox, or view authorization failure is `permission denied`;
- invalid selected-mailbox state, stale selection context, or a message
  reference that is not valid for that selected mailbox is `conflict`;
- transient object storage, materialized-state, or daemon API failures are
  `temporary backend failure` in the Dovecot caller path; and
- ambiguous transport outcomes are not success.

FETCH is a read-only daemon operation. It does not append mutation journal
records, and it does not mutate DuckDB, Wirelog, object-store metadata, raw
objects, or journal state. It reads only enough daemon-owned state to authorize
the selected mailbox context, resolve the requested message, and stream the
canonical object bytes.

This operation does not expose arbitrary SQL, write SQL, DuckDB mutation,
Wirelog mutation, object-store metadata mutation, raw object rewrite, direct
journal append, direct journal write, or mutation journal append surfaces to
Dovecot plugins or local callers. It accepts message FETCH inputs only.

## Message Search Operation Contract

Message SEARCH is a Dovecot-facing SEARCH call over the Cap'n Proto-over-UDS
daemon API. This section defines operation behavior only; it does not define
concrete `.capnp` schemas, field layouts, generated code, Dovecot backend
implementation, or the concrete criteria schema.

Every SEARCH request carries request and caller identity sufficient for
`wyreboxd` to authorize and correlate the operation:

- required `request_id`;
- Dovecot/IMAP operation correlation ID where the caller stack can supply one;
- caller/account identity;
- stable selected-mailbox identity for an ordinary mailbox or virtual mailbox;
  and
- selected-mailbox `UIDVALIDITY` or equivalent selection epoch.

The caller/account identity scopes all access. The selected mailbox identity,
`UIDVALIDITY`, and selection epoch are the authorization and state boundary.
SEARCH must not return results outside the authorized selected ordinary mailbox
or virtual mailbox view.

SEARCH criteria are IMAP-derived search criteria supplied as daemon operation
inputs, not arbitrary SQL or raw DuckDB query strings. The concrete criteria
schema is deferred to a later `.capnp` schema slice, but the semantic boundary
is fixed here: Dovecot plugins request IMAP-search behavior from `wyreboxd`;
they do not submit database or Datalog query text.

SEARCH returns mailbox-scoped UIDs or equivalent mailbox-scoped message
references suitable for Dovecot, not raw object-store keys as client-visible
result identity. A matching raw message object may therefore have different
returned identities in an ordinary mailbox and in a virtual mailbox.

For an ordinary selected mailbox, SEARCH evaluates only that selected mailbox's
membership. For a selected virtual mailbox, SEARCH is evaluated within that
virtual view membership, and results remain mailbox-scoped to that view.

SEARCH error behavior is governed by `docs/contracts/error-model.md`:

- an absent selected mailbox in the caller's authorized scope is `not found`;
- account, mailbox, or view authorization failure is `permission denied`;
- stale `UIDVALIDITY`, stale selection context, stale selection epoch, or
  invalid selected state is `conflict`;
- transient index, materialized-state, or daemon API failures are
  `temporary backend failure` in the Dovecot caller path; and
- ambiguous transport outcomes are not success.

SEARCH is a read-only daemon operation. It does not append mutation journal
records, and it does not mutate DuckDB, Wirelog, object-store metadata, raw
objects, or journal state. It may read daemon-owned indexes/materialized state
needed to authorize the selected mailbox and evaluate the requested IMAP-derived
criteria.

This operation does not expose arbitrary SQL, write SQL, direct DuckDB query
execution, Wirelog mutation/query, object-store metadata mutation, direct
journal append/write, or mutation journal append surfaces to Dovecot plugins or
local callers. It accepts message SEARCH inputs only.

## Flag Keyword Update Operation Contract

Flag/keyword update is a Dovecot-facing STORE-style mutation over the Cap'n
Proto-over-UDS daemon API. This section defines operation behavior only; it
does not define concrete `.capnp` schemas, field layouts, generated code, or
Dovecot backend implementation.

Every flag/keyword update request carries request and caller identity
sufficient for `wyreboxd` to authorize and correlate the operation:

- required `request_id`;
- Dovecot/IMAP operation correlation ID where the caller stack can supply one;
- caller/account identity;
- stable selected-mailbox identity for an ordinary mailbox or virtual mailbox;
- selected-mailbox `UIDVALIDITY` or equivalent selection epoch; and
- mailbox-scoped UID or equivalent message reference within that selected
  mailbox context.

The caller/account identity scopes all access. The selected mailbox identity
and mailbox-scoped UID/message reference are the mutation boundary. A
flag/keyword update must not affect messages outside the authorized selected
mailbox or virtual mailbox view, even when the same raw message object appears
in multiple ordinary or virtual mailbox contexts.

Flag/keyword update supports operation types for requested IMAP system flag and
user keyword changes:

- set the supplied system flags and user keywords;
- clear the supplied system flags and user keywords; and
- replace the current mailbox-scoped system flag and user keyword set with the
  supplied values.

The operation changes daemon-owned mailbox membership state only. It never
rewrites raw RFC 5322 objects, never alters canonical object bytes, and never
stores flags or keywords inside the raw message object.

Dovecot-visible success is valid only after the corresponding flag/keyword
mutation has reached the durable mutation-journal boundary. A success response
includes `journal_offset` or an equivalent durable marker so the Dovecot caller
path and logs can correlate the accepted mutation with durable state.

Flag/keyword update error behavior is governed by
`docs/contracts/error-model.md`:

- an absent mailbox, selected mailbox outside the authorized scope, or absent
  message reference in scope is `not found`;
- account, mailbox, message, or view authorization failure is
  `permission denied`;
- stale `UIDVALIDITY`, stale selected-mailbox context, invalid selected state,
  or a message reference that is not valid for that selected mailbox is
  `conflict`;
- transient mutation-journal, materialized-state, or daemon API failure is
  `temporary backend failure` in the Dovecot caller path; and
- ambiguous transport outcomes are not success.

Flag/keyword update is daemon-mediated. It may cause daemon-owned journaled
flag/keyword state mutation only through this operation. It does not expose
arbitrary SQL, write SQL, DuckDB mutation, Wirelog mutation, object-store
metadata mutation, raw object rewrite, direct journal append, direct journal
write, or direct object-store/journal mutation surfaces to Dovecot plugins or
local callers. It accepts flag/keyword update inputs only.

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

- fact insert/retract
- Wirelog predicate query
- safe DuckDB query templates

Concrete SEARCH `.capnp` schemas, field layouts, and criteria payloads are
deferred. Concrete flag/keyword `.capnp` schemas and field layouts are
deferred, along with query-safety policy details. In this slice, no arbitrary
write SQL is allowed over the daemon API.

## Out Of Scope

The following are out of scope for this slice:

- concrete Cap'n Proto message layouts
- concrete request/response/error/query payload schemas
- TCP, TLS, HTTP, LMTP, and remote authentication
- Dovecot implementation
- fact/query APIs
- concrete daemon implementation
- full .capnp generation
- public remote API exposure
- daemon internals, including replay internals and storage-engine startup logic
