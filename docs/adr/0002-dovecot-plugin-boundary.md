# ADR 0002: Dovecot Plugin Boundary

## Status

Accepted for issue 0001 boundary planning. Production Dovecot plugin code is
not part of this ADR.

## Context

WyreBox keeps Dovecot as the IMAP protocol layer while replacing Maildir as the
canonical mailbox backend. Existing IMAP clients must continue to use Dovecot,
but mailbox identity, raw object lookup, durable mutations, and virtual mailbox
membership must come from WyreBox state rather than from Dovecot's existing
Maildir, mdbox, FTS-only, or virtual-only storage paths.

The boundary must preserve the storage rules from the project plan:

- raw RFC 5322 bytes are immutable objects;
- `wyreboxd` is the only writer of canonical WyreBox mutation state;
- DuckDB and search indexes are materialized/supporting stores, not the
  synchronous mutation authority;
- ordinary and virtual mailboxes are visible through Dovecot to existing IMAP
  clients.

## Decision

WyreBox will provide a real Dovecot mail storage backend. The backend is the
canonical Dovecot-side boundary for mailbox listing, mailbox open/SELECT
behavior, stable UID and UIDVALIDITY reporting, original-byte FETCH, STORE
flag and keyword updates, SEARCH support, and first-class virtual mailbox
exposure.

WyreBox owns mailbox identity, UID allocation, UIDVALIDITY, flags, keywords,
ordinary mailbox identity, and virtual mailbox identity. Dovecot remains the
IMAP protocol engine and calls the WyreBox storage backend through Dovecot's
lib-storage contracts.

Raw RFC 5322 message bytes remain immutable WyreBox objects. FETCH must resolve
the message object's key through the WyreBox backend or `wyreboxd` and return
the original stored byte stream. STORE flag and keyword updates must not
rewrite raw objects; they must enter the WyreBox mutation path and then be
reflected back through storage status, sync, and fetch operations.

Ordinary and virtual mailboxes are both exposed through the storage backend.
Virtual mailboxes are WyreBox-owned views with their own mailbox identity, UID
namespace, UIDNEXT, and UIDVALIDITY. A raw object may appear in an ordinary
mailbox and one or more virtual views without duplicating the raw object.

Search and FTS are supporting layers. They may accelerate SEARCH or provide
additional matching, but they are not the canonical mailbox backend and cannot
own mailbox identity, message membership, UID allocation, UIDVALIDITY, flags,
keywords, or virtual mailbox identity.

## Official Dovecot Evidence

The Dovecot 2.3 mail storage design documentation says mail storage is the
common container for mailboxes and lists backend methods including storage
allocation, namespace creation, mailbox-list attachment, mailbox allocation,
and purge support:
https://doc.dovecot.org/2.3/developer_manual/design/mail_storage/

The Dovecot plugin design documentation says plugins are loaded through
versioned init/deinit symbols, that plugins should define a
`<plugin_name>_version` using `DOVECOT_ABI_VERSION`, and that lib-storage hooks
exist through `mail_storage_hooks_add()`:
https://doc.dovecot.org/2.3/developer_manual/design/plugins/

The public lib-storage source declares the status fields WyreBox must supply,
including `uidvalidity`, `uidnext`, keyword arrays, permanent flags, and
modseq-related status. It also declares mailbox lifecycle, status, transaction,
search, fetch stream, flag update, and keyword update entry points:
https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage.h

The private lib-storage source shows the backend vfunc surface that a storage
implementation participates in: `mail_storage_vfuncs` allocates storage and
mailboxes, `mailbox_vfuncs` opens boxes, reports status, runs transactions and
searches, and allocates `struct mail`, while `mail_vfuncs` supplies streams,
flags, keywords, modseqs, and update operations. It also contains virtual
mailbox vfuncs for mapping backend UIDs to virtual UIDs:
https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage-private.h

These sources support the storage-backend path and show why a search-only,
FTS-only, or side-index integration would not own enough of Dovecot's mailbox
contract.

## Rejected Alternatives

Maildir plus WyreBox side indexes is rejected as the canonical path. It would
leave Dovecot's Maildir backend in charge of mailbox files, UID behavior, and
flag storage while WyreBox tried to mirror or override that state elsewhere.
That violates the decision that raw objects are immutable WyreBox objects and
that `wyreboxd` is the only writer of canonical mutation state.

Existing Dovecot storage plus FTS or virtual-only integration is rejected as
insufficient. FTS and virtual hooks can support search acceleration or derived
view presentation, but they do not make WyreBox the owner of mailbox identity,
UID allocation, UIDVALIDITY, flags, keywords, ordinary membership, or virtual
mailbox identity.

Dovecot LMTP plus existing storage is rejected as the canonical backend and
deferred as an ingress topic. Dovecot LMTP can deliver into an existing Dovecot
storage backend, but it does not solve WyreBox-owned mailbox identity,
immutable object fetch, STORE mutation routing, or virtual view ownership. LMTP
may be re-evaluated later as additive ingress after the storage backend
contract is proven.

## WyreBox-Owned Mailbox Model

WyreBox owns the IMAP-visible identity and mutation state exposed by the
Dovecot storage backend:

- mailbox identity and hierarchy names, including ordinary and virtual
  mailboxes;
- per-mailbox UID namespace, UIDNEXT, and UIDVALIDITY;
- mailbox membership from WyreBox ordinary state or Wirelog-derived virtual
  views;
- message-to-immutable-object mapping by object key;
- flags and keywords for each mailbox membership;
- internal date and RFC 822 size metadata needed for IMAP FETCH and SEARCH;
- MODSEQ or equivalent sequencing if required by Dovecot features enabled for
  the mailbox.

Dovecot may keep protocol caches and indexes as implementation details, but
those caches must be rebuildable from WyreBox state and must not become the
canonical write path.

## Fetch And Mutation Path

FETCH of full message content, headers, body sections, and sizes must resolve
from WyreBox metadata to the immutable object key and stream the original RFC
5322 bytes through the storage backend. Byte-for-byte preservation is part of
the backend contract.

STORE flag and keyword changes must be handled as WyreBox mutations. The
backend may receive the request through Dovecot mail update and transaction
entry points, but commit must route the change through the WyreBox mutation
path. Updating flags, keywords, MODSEQ, mailbox membership, or virtual view
facts must never rewrite the raw RFC 5322 object.

## Virtual Mailboxes

Virtual mailboxes are first-class WyreBox-owned mailbox views exposed through
the storage backend. Each virtual mailbox has its own mailbox identity, UID
namespace, UIDNEXT, and UIDVALIDITY independent of the ordinary mailboxes from
which its members are derived.

Virtual membership is derived from WyreBox/Wirelog state and materialized for
Dovecot through the backend. Dovecot's existing virtual plugin may be evaluated
only as a supporting mechanism; it is not the canonical owner of WyreBox
virtual mailbox identity or UID allocation.

## Dovecot ABI And Allocation Constraints

The target Dovecot support version is not fixed by this ADR. Support version
remains a source-contract spike, and the plugin ABI must be pinned before
implementation.

Dovecot plugin loading is ABI-sensitive through `DOVECOT_ABI_VERSION`, and
storage backends participate in Dovecot's allocation, pool, object lifetime,
and vfunc conventions. WyreBox may use GObject wrappers at WyreBox boundaries,
daemon adapters, and long-lived internal services, but those wrappers must not
fight Dovecot ownership rules inside Dovecot-owned storage, mailbox,
transaction, mail, stream, or plugin objects.

## Capability Gap And Follow-Up Table

| Capability | Boundary decision | Gap or follow-up |
| --- | --- | --- |
| LIST | Implement through the WyreBox storage backend and WyreBox-owned mailbox hierarchy. | Define hierarchy naming, subscriptions, special-use behavior, and virtual mailbox visibility rules. |
| SELECT | Implement through backend mailbox open/status using WyreBox-owned mailbox identity. | Specify exact status fields, read-only behavior, CONDSTORE/MODSEQ behavior, and error mapping. |
| Stable UID/UIDVALIDITY | WyreBox owns per-mailbox UID allocation, UIDNEXT, and UIDVALIDITY for ordinary and virtual mailboxes. | Define allocation persistence, replay recovery, compaction behavior, and UIDVALIDITY reset rules. |
| Original byte FETCH | FETCH resolves object keys through WyreBox or `wyreboxd` and streams immutable RFC 5322 bytes. | Add golden `.eml` tests for byte-for-byte header, body, section, and size behavior. |
| STORE flags/keywords | STORE routes through the WyreBox mutation path and must not rewrite raw objects. | Define transaction commit behavior, keyword validation, MODSEQ updates, and retry/error mapping. |
| Basic SEARCH | Backend supplies basic Dovecot SEARCH semantics from WyreBox metadata and immutable-object reads where required. | Decide which terms use DuckDB, object reads, cache fields, or later FTS acceleration. |
| Virtual view exposure | Virtual views are first-class WyreBox mailboxes with their own UID namespace and UIDVALIDITY. | Define Wirelog refresh mechanics, virtual UID stability, and scripted IMAP validation cases. |

## Consequences

The selected boundary is deeper than a search plugin or LMTP-only integration,
but it aligns Dovecot's mailbox contract with WyreBox's storage and mutation
rules. It keeps Postfix and Dovecot in their protocol roles while making
WyreBox the owner of canonical mailbox state.

Implementation must start with source-contract validation against a pinned
Dovecot version before plugin code is written. Tests must exercise IMAP-visible
behavior through Dovecot, but this ADR deliberately adds no production plugin
scaffolding, Dovecot configuration examples, or mailbox storage model contract.

## Follow-Up Decisions

The following decisions remain open and must be converted into later focused
issues or contracts:

- FTS strategy: decide whether Dovecot FTS, DuckDB-backed search, a WyreBox
  search adapter, or a hybrid path serves each SEARCH class.
- Virtual view refresh mechanics: define how Wirelog-derived membership updates
  become stable virtual UIDs and visible mailbox changes.
- Scripted IMAP validation details: define LIST, SELECT, FETCH, STORE, SEARCH,
  and virtual mailbox scripts before Thunderbird smoke tests.
- Mailbox storage model contract: create the stable contract for accounts,
  mailboxes, messages, immutable objects, membership, UIDs, UIDVALIDITY, flags,
  keywords, internal dates, RFC 822 sizes, and MODSEQ requirements.
