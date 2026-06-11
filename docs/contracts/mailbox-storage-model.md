# Mailbox Storage Model Contract

## Status

Accepted for issue 0001 boundary planning. This contract fixes the stable
IMAP-visible storage model that later schema, daemon API, and Dovecot backend
work must preserve.

## Scope

This contract defines the stable IMAP-visible storage model only. It describes
the account, mailbox, message, immutable object, mailbox membership, UID,
UIDVALIDITY, flag, keyword, internal date, RFC 822 size, and possible MODSEQ
ownership rules that Dovecot clients observe through the WyreBox storage
backend.

The following details are intentionally out of scope for this contract:

- DuckDB table DDL.
- journal event format.
- object-store path layout.
- Cap'n Proto API schema.
- Dovecot vfunc or plugin scaffolding.
- UID allocator algorithm.

## Account Identity And Ownership Boundary

An account is the owner boundary for ordinary mailboxes, virtual mailboxes,
messages, immutable objects, and mailbox memberships. Account identity is the
outer scope for IMAP-visible mailbox hierarchy, message membership, and object
authorization decisions.

WyreBox owns the account-scoped mailbox identity state exposed through Dovecot.
Dovecot remains the IMAP protocol layer, but Dovecot must observe account,
mailbox, UID, UIDVALIDITY, flag, keyword, internal date, RFC 822 size, and
virtual view state from WyreBox rather than from Maildir or side indexes.

## Ordinary Mailbox Identity

Ordinary mailboxes have stable WyreBox mailbox identity and hierarchy names.
Hierarchy names are the IMAP-visible names listed and selected through Dovecot,
but their identity and membership state belong to WyreBox.

Each ordinary mailbox has its own UID namespace, UIDNEXT, and UIDVALIDITY.
Mailbox-scoped UIDs are meaningful only inside the mailbox that owns them.
UIDNEXT is the next UID value that WyreBox will expose for that mailbox.
WyreBox owns UIDVALIDITY and is responsible for preserving or changing it under
later documented rules.

## Virtual Mailbox Identity

Virtual mailboxes are first-class WyreBox mailbox views, not Dovecot-only
virtual folders and not search result aliases. Each virtual mailbox has
independent mailbox identity, UID namespace, UIDNEXT, and UIDVALIDITY. A
virtual UID is scoped to the virtual mailbox that owns it and does not borrow
the UID namespace of an ordinary mailbox.

Virtual mailbox membership is derived from WyreBox or Wirelog state and exposed
through the same storage model as ordinary mailbox membership. Dovecot's
existing virtual plugin may be evaluated later only as a supporting mechanism;
it is not the owner of WyreBox virtual mailbox identity or virtual UID
allocation.

## Object Message And Membership Model

Within an account, object, message, and mailbox membership are distinct model
entities.

An object is the immutable stored original RFC 5322 byte stream identified by
an object key. Object identity is independent of any mailbox that references
the bytes.

A message records WyreBox metadata that maps to one immutable object. The
message is the account-level representation that can be referenced by one or
more mailbox memberships.

A mailbox membership links one message into one ordinary or virtual mailbox.
Mailbox membership owns the mailbox-scoped UID, flags, keywords, internal date,
RFC 822 size, and possibly MODSEQ. Membership-owned state can differ between
mailboxes even when two memberships reference the same message and object.

Raw objects are shared across ordinary and virtual memberships without
duplication. A raw object may appear in an ordinary mailbox and in one or more
virtual mailboxes through separate memberships, but the underlying RFC 5322
bytes remain one immutable object.

## Immutable Object Fetch Contract

Raw RFC 5322 message bytes are immutable. FETCH returns the original bytes
byte-for-byte by object key, including headers, body, line endings, and any
other stored RFC 5322 content.

FETCH implementations may use metadata, caches, or indexes to find the object
key, but the authoritative content stream is the stored immutable object.
Flags, keywords, mailbox moves, facts, virtual views, and derived indexes must
not rewrite the raw object.

## Membership Mutation Contract

STORE flag and keyword changes append a WyreBox mutation and do not rewrite
the raw object. The mutation updates mailbox membership state, then Dovecot
observes the resulting flags, keywords, and any relevant sequencing state
through the WyreBox storage backend.

A membership mutation changes membership-owned state such as flags, keywords,
mailbox-scoped UID visibility, internal date, RFC 822 size metadata when
validated from the object, and possibly MODSEQ. It does not change the
immutable object's bytes.

## Canonical State And Rebuildable Support State

`wyreboxd` is the only writer of the canonical mutation journal, DuckDB
materialized state, Wirelog runtime state, and WyreBox object-store metadata.
Postfix helpers, Dovecot backend code, and support indexers must not write
canonical state directly.

DuckDB is not the canonical mutation authority. DuckDB tables are materialized
query and index state derived from the journal and replayable WyreBox state.

Dovecot indexes, search indexes, and caches are rebuildable support state.
They may accelerate LIST, SELECT, FETCH, STORE visibility, SEARCH, or virtual
view exposure, but they must be recoverable from WyreBox canonical state and
must not become the source of truth for mailbox identity, UIDVALIDITY,
membership, flags, keywords, raw bytes, or virtual mailbox identity.

## Deferred Schema Details

Schema details are deferred. This contract deliberately does not define:

- DuckDB table DDL.
- journal event format.
- object-store path layout.
- Cap'n Proto API schema.
- Dovecot vfunc/plugin scaffolding.
- UID allocator algorithm.

Later schema work must preserve this model but may choose concrete table names,
keys, indexes, event shapes, API messages, and allocator implementation details.

## Follow-Up Decisions

Exact MODSEQ policy: decide whether MODSEQ is always present, present only when
CONDSTORE/QRESYNC support is enabled, or represented through an equivalent
sequencing field.

Special-use and subscriptions: define how IMAP special-use attributes,
subscriptions, namespace listing behavior, and virtual mailbox visibility are
represented.

Virtual refresh mechanics: define how Wirelog-derived membership changes are
materialized, how virtual UIDs remain stable, and how Dovecot observes refresh
events.

Later schema tables: define the concrete schema tables and indexes after this
contract is accepted, without changing the ownership boundaries above.
