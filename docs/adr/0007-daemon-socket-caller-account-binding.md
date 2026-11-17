# ADR 0007: Daemon Socket Caller Account Binding

## Status

Accepted for issue #364. Implementation follows this ADR in stacked units:
config and authorizer, then enforcement and harness updates.

## Context

The Dovecot plugin connects to the shared Unix socket
`/run/wyrebox/wyrebox.sock` and sends the Dovecot username as Cap'n Proto
`account_identity`. `wyreboxd` already receives Linux peer credentials
(`SO_PEERCRED`: uid, gid, pid) on each connection, but account-scoped
services only compare claimed Cap'n Proto fields. Any process that can open
the socket can claim another account's identity and read its mailboxes and
messages.

`docs/contracts/core-daemon-api.md` already requires peer credential checks
and an authorized account visibility boundary. Issue #318 covers validating
delivery account identities against known accounts. It does not authorize
read-path callers on the shared socket.

Typical Dovecot mailbox workers share one system uid (`vmail` / `dovecot`).
Peer uid alone therefore cannot isolate accounts unless operators map that
uid to an explicit account set, split uids per account, or add a later
session token.

## Decision

1. Authorize account-scoped daemon requests with **SO_PEERCRED uid + an
   explicit config allowlist** of account identities for that uid.
2. Keep the shared socket `/run/wyrebox/wyrebox.sock`. Do not introduce
   per-account sockets or login tokens in #364.
3. Config shape (INI, next to existing `[daemon]` / `[wirelog]` sections):

   ```
   [peer-account-binding:<uid>]
   accounts=account-a,account-b
   ```

   Optional process-trust form for a shared mailbox uid that is trusted for
   every account the daemon serves:

   ```
   [peer-account-binding:<uid>]
   accounts=*
   ```

4. **Fail closed.** A peer uid with no binding section, or a claimed
   `account_identity` outside that uid's allowlist, is permission denied.
   Startup requires at least one `[peer-account-binding:…]` section so an
   empty config cannot silently leave the socket open to every claim.
5. **`accounts=*` is process trust, not multi-tenant isolation.** Intended
   multi-tenant deployments map each mailbox uid to an explicit account
   list without `*`. Migration path: uid-per-account bindings first; a
   later login-token design may bind a single shared uid to one session
   account.
6. Apply the peer-account check to every account-scoped request that
   carries `account_identity`, through one central authorizer called from
   the existing service `authorize_*` paths (LIST, SELECT/STATUS, FETCH,
   SEARCH, DuckDB query templates including UID map, Wirelog predicate
   query, mail-event stream, and mailbox-write authorize helpers).
7. **Exclude delivery ingestion** from this peer-account binding in #364.
   Postfix pipe/LMTP keep `caller_identity == "postfix"` checks. Binding
   the Postfix helper uid is a follow-up if needed; do not conflate it
   with #318's known-account delivery validation.
8. Keep existing Cap'n Proto checks (`caller_identity` and
   identity/request account match). Peer binding is additional.

## Alternatives Considered

- **Per-account Unix sockets.** Extra packaging and permission surface; a
  shared mailbox uid can still open every socket unless ownership also
  splits. Deferred; weak gain versus an allowlist on the existing shared
  socket.
- **Login token issued at Dovecot login.** Stronger for shared-uid session
  isolation, but needs wire fields, issuance, storage, and rotation. Recorded
  as a follow-up, not the #364 first cut.
- **Peer uid alone without a config map.** Fails on shared-uid Dovecot and
  cannot express intended multi-tenant bindings.
- **Fail-open when no bindings are configured.** Leaves today's
  claim-any-account hole. Rejected; startup requires at least one binding
  section.

## Consequences And Known Gaps

- Operators must configure peer-account bindings before `wyreboxd` starts.
  Example and test configs must include a binding for the peer uid that
  runs the helper or Dovecot worker under test.
- Shared-uid Dovecot with `accounts=*` still trusts the whole mailbox
  process for every listed/`*` account; that mode only stops unrelated
  uids on the socket group.
- Plugin username → `account_identity` mapping is unchanged.
- Delivery ingestion stays on Postfix caller checks in #364; peer binding
  for Postfix helpers is a follow-up.
- TCP API remains out of scope.
- Login-token session binding and issue #318 known-account delivery
  validation remain follow-ups.
