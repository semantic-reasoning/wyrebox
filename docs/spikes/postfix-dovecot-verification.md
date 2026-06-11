# Postfix/Dovecot Verification Spike

## Status

Accepted. Issue 0001 is closed by the existing boundary and storage documents:

- `docs/adr/0001-postfix-ingress-boundary.md`
- `docs/adr/0002-dovecot-plugin-boundary.md`
- `docs/contracts/mailbox-storage-model.md`

This spike does not add new architecture decisions. It verifies that those
documents satisfy the 0001 boundary question and records the remaining work as
explicit follow-up issues.

## Postfix Result

The Postfix boundary is staged:

- `pipe(8)` delivery helper first.
- WyreBox LMTP ingress later as additive production-oriented work.
- Dovecot LMTP deferred until the WyreBox Dovecot mail storage backend is
  proven.

The first helper receives the original RFC 5322 byte stream from Postfix,
passes delivery metadata to `wyreboxd`, and reports success only after durable
raw object storage and durable journal append are confirmed. Temporary daemon,
socket, permission, or ambiguous communication failures map to Postfix retry
behavior. Permanent validation or non-retryable configuration failures map to
Postfix permanent failure behavior.

Duplicate delivery idempotency remains a follow-up gap for 0009, not a new
boundary decision. The `pipe(8)` path can see an ambiguous result after
`wyreboxd` commits but before the helper receives the response; issue 0009 must
make duplicate behavior deterministic through the ingestion implementation.

## Dovecot Result

The Dovecot boundary is a real WyreBox mail storage backend. It is not Maildir
plus side indexes, not an FTS-only or virtual-only integration, and not Dovecot
LMTP plus existing storage as the canonical backend.

WyreBox owns mailbox identity, per-mailbox UID namespaces, UIDVALIDITY, flags,
keywords, ordinary membership, and virtual mailbox identity. Dovecot remains
the IMAP protocol layer and observes WyreBox state through the storage backend.
FETCH resolves to immutable WyreBox object keys and returns the original RFC
5322 bytes. STORE flag and keyword updates enter the WyreBox mutation path and
do not rewrite raw message objects.

Virtual mailboxes are first-class WyreBox mailbox views with their own mailbox
identity, UID namespace, UIDNEXT, and UIDVALIDITY. Dovecot's virtual plugin may
be evaluated later only as a supporting mechanism.

## Evidence Sources

The verification relies on the evidence already used in the ADRs:

- Postfix `pipe(8)` manual:
  https://www.postfix.org/pipe.8.html
- Postfix LMTP client manual:
  https://www.postfix.org/lmtp.8.html
- Postfix local delivery manual:
  https://www.postfix.org/local.8.html
- Dovecot 2.3 mail storage design:
  https://doc.dovecot.org/2.3/developer_manual/design/mail_storage/
- Dovecot 2.3 plugin design:
  https://doc.dovecot.org/2.3/developer_manual/design/plugins/
- Dovecot public lib-storage header:
  https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage.h
- Dovecot private lib-storage header:
  https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage-private.h

These sources support the selected extension boundaries: Postfix can invoke a
local delivery helper and later deliver to LMTP, while Dovecot's lib-storage
surface is the boundary that can own mailbox list, status, fetch, search,
flags, keywords, and virtual mailbox mapping behavior.

## Capability Verification Matrix

| Capability | Verified boundary | Follow-up issue |
| --- | --- | --- |
| LIST | WyreBox storage backend lists WyreBox-owned ordinary mailbox hierarchy and later virtual mailbox names. | 0010 defines ordinary listing; 0011 defines virtual view listing. |
| SELECT | WyreBox storage backend opens mailboxes and reports status from WyreBox-owned mailbox identity. | 0010 defines status fields, errors, and scripted IMAP SELECT tests. |
| UID/UIDVALIDITY | WyreBox owns per-mailbox UID namespaces, UIDNEXT, and UIDVALIDITY for ordinary and virtual mailboxes. | 0003 defines persistence and replay rules; 0010 verifies ordinary UID stability; 0011 verifies virtual UID stability. |
| FETCH original bytes | FETCH resolves immutable object keys and returns original RFC 5322 bytes byte-for-byte. | 0003 defines object/schema mapping; 0010 verifies Dovecot FETCH behavior. |
| STORE flags/keywords | STORE routes flag and keyword mutations through WyreBox journal-backed mutation state without raw object rewrite. | 0003 defines schema and journal representation; 0010 implements and tests STORE behavior. |
| SEARCH | Dovecot SEARCH is served by the WyreBox storage backend using WyreBox metadata/object reads, with FTS only as a supporting layer. | 0010 implements basic SEARCH; later FTS strategy remains inside that integration scope unless split out. |
| Virtual views | Virtual views are first-class WyreBox mailboxes backed by Wirelog-derived membership, with independent UID namespace and UIDVALIDITY. | 0011 implements refresh, visibility, membership, and scripted IMAP validation. |

## Acceptance Criteria Verification

| 0001 acceptance criterion | Result | Evidence or follow-up |
| --- | --- | --- |
| Postfix ingress is documented as staged: `pipe(8)` first, LMTP later. | Pass | ADR 0001; follow-up 0009 for pipe implementation and 0013 for LMTP. |
| The `pipe(8)` helper has documented success, temporary failure, and permanent failure semantics. | Pass | ADR 0001 failure mapping and durable ingestion boundary; implementation tests in 0009. |
| LMTP follow-up requirements are documented without blocking the first implementation. | Pass | ADR 0001 defers LMTP endpoint details; 0013 owns additive LMTP ingress. |
| The Dovecot path is fixed as a WyreBox mail storage backend. | Pass | ADR 0002 decision. |
| The storage backend path has documented support or gaps for mailbox list, fetch, flags, search, and virtual views. | Pass | ADR 0002 capability gap table and this spike's matrix; follow-ups 0010 and 0011. |
| Virtual mailboxes are treated as WyreBox-owned mailbox views with their own UID namespace and UIDVALIDITY. | Pass | ADR 0002 and mailbox storage model contract; follow-up 0011. |
| The minimum mailbox model is specific enough for schema work to begin. | Pass | Mailbox storage model contract; 0003 can design concrete schema. |
| UID and UIDVALIDITY ownership are explicitly assigned to WyreBox or Dovecot. | Pass | ADR 0002 and mailbox storage model contract assign ownership to WyreBox. |
| Raw message bytes are guaranteed to remain immutable and fetchable by object key. | Pass | Mailbox storage model contract; 0003 and 0010 verify schema and FETCH behavior. |
| Flag updates are confirmed to avoid rewriting raw message objects. | Pass | ADR 0002 and mailbox storage model contract; 0010 verifies STORE behavior. |
| Open questions are converted into follow-up issues, not left as vague TODOs. | Pass with mapped gaps | Remaining gaps are mapped below to 0002, 0003, 0009, 0010, 0011, and 0013. |

## Remaining Follow-Up Gaps

| Gap | Follow-up issue |
| --- | --- |
| Linux runtime layout, daemon socket permissions, Postfix chroot access, Dovecot runtime access, and packaging assumptions. | 0002 |
| Concrete schema, object key strategy, UID/UIDVALIDITY persistence, journal events, object-store layout, and replay behavior. | 0003 |
| Postfix `pipe(8)` helper implementation, exit-status mapping, delivery metadata mapping, operational logs, durable success tests, and duplicate delivery idempotency. | 0009 |
| Dovecot ABI pin/source-contract spike, ordinary mailbox backend implementation, LIST, SELECT, UID/UIDVALIDITY, FETCH original bytes, STORE flags/keywords, SEARCH, scripted IMAP tests, and ownership documentation. | 0010 |
| Wirelog-derived virtual mailbox naming, refresh mechanics, independent virtual UID namespace, virtual UIDVALIDITY, membership stability, and scripted IMAP validation. | 0011 |
| Additive Postfix LMTP endpoint shape, recipient-level status behavior, process model, durable success response, and coexistence with the pipe helper. | 0013 |

## Start Of Issue 0002

Issue 0002 can start after this spike because the Postfix and Dovecot boundary
decisions are captured in ADR 0001, ADR 0002, and the mailbox storage model
contract. Runtime and packaging work can now rely on `pipe(8)` first, LMTP
later, Dovecot LMTP deferred, and a real WyreBox Dovecot mail storage backend
as the selected boundaries.
