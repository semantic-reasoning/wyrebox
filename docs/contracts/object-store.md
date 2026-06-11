# Object Store Contract

## Status

Accepted for the first implementation unit of issue 0003. This contract fixes
the minimal local immutable object-store behavior used by early WyreBox core
code.

## Scope

This contract defines only the local immutable object store for original RFC
5322 message bytes. It covers object keys, local path layout, byte preservation,
idempotent writes, fetch behavior, and validation failures.

The following details are intentionally out of scope for this contract:

- DuckDB table DDL.
- append-only journal event format.
- daemon socket API.
- Cap'n Proto schema.
- Postfix or Dovecot integration.
- Wirelog fact or rule storage.
- UID or UIDVALIDITY logic.
- non-local object storage providers.

## Object Identity

An object key identifies immutable RFC 5322 message bytes. The initial key
format is `sha256:<64 lowercase hex chars>`.

The SHA-256 digest is computed over the exact input byte sequence. The key is
deterministic: storing the same bytes returns the same object key, and storing
different bytes returns different object keys.

The object key is the API identity. Local filename suffixes and directory
partitioning are storage layout details and are not part of the API identity.

This is identity only inside the object-store API. Outside that boundary, an
object key is a reference to immutable raw RFC 5322 bytes and does not identify
a delivered-message occurrence.

## Local Layout

The local object store is rooted at a caller-provided directory. The first
implementation must not force `/var/lib/wyrebox`; tests and tools may provide
temporary directories.

Objects are stored under:

`<root>/objects/sha256/<first-two-hex>/<full-hex>.eml`

The `.eml` suffix records that the object contains original RFC 5322 message
bytes. It is a local layout detail only; the object key is the API identity.

## Byte Preservation

The object store stores original RFC 5322 message bytes unchanged. Fetching an
object by key returns the original bytes byte-for-byte, including headers, body,
line endings, and any other stored content.

Flags, keywords, mailbox moves, facts, virtual views, parsed metadata, derived
indexes, and query stores must not rewrite raw object bytes.

## Immutable Write Behavior

Putting bytes stores the object only when the corresponding object path does
not already exist. A duplicate put of identical bytes returns the same key and
does not rewrite the existing object.

An existing object is not overwritten by local object-store operations. Later
journal and daemon work may define additional consistency checks around object
writes, but the raw object remains immutable.

## Fetch And Failure Behavior

Fetching a valid existing object key returns the stored object bytes with
transfer-full ownership to the caller.

Invalid object keys fail with `GError`. Missing objects addressed by otherwise
valid keys also fail with `GError`.

## Component Boundary

The initial implementation is a GLib/GObject component located under
`wyrebox/`, with public and internal headers next to the corresponding source
files. It does not introduce a top-level `include/` directory.

The local object-store constructor accepts a root directory. The component owns
only local immutable object bytes and does not implement DuckDB, journal,
daemon, Postfix, Dovecot, Wirelog, Cap'n Proto, ingestion, or UID behavior.
