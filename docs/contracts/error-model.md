# Error Model Contract

## Status

Accepted for issue 0004.

## Scope

This contract defines only daemon/API error model semantics for the first
implementation phase.

It does not define any of the following:

- Cap'n Proto schema layout.
- Request envelopes and command schemas.
- SQL query template catalog and query policy implementation.
- daemon runtime behavior.
- Postfix and Dovecot implementation internals.
- TCP, TLS, or remote authentication behavior.

## Error Classes

The daemon/API reports six error classes:

- Temporary failure.
- Permanent failure.
- Permission denied.
- Not found.
- Conflict.
- Internal error.

Permission denied remains distinct from temporary failure, permanent failure, and
success. Permission-related outcomes are not treated as permanent validation
failures and are not promoted to success.

## Transport And Access Conditions

Transport/access conditions are separate from daemon/API response classes and are
handled explicitly:

- Socket unavailable.
- Stale socket.
- Connect timeout.
- Permission mismatch.
- Response loss or ambiguous communication loss.

Each transport/access condition is mapped by the client caller to its own retry
policy path rather than being converted into a durable API class without evidence
of completion.

## Postfix Delivery Mapping

For delivery operations:

- Transport/access conditions map to temporary delivery failure with Postfix retry.
- Response loss, connection loss, and communication interruption before durable
  success map to temporary delivery failure and retry.
- Permanent failure is reserved for explicit daemon validation or configuration
  outcomes that are non-retryable.
- Delivery success requires durable raw message object storage and durable journal
  append.

## Dovecot Visible Mapping

For Dovecot-visible LIST, SELECT, fetch, search, and flag operations:

- Transport/access and temporary conditions map to IMAP-visible temporary backend
  failure.
- Not found and conflict outcomes are operation-aware (for example, dependent on
  mailbox, message, or state transitions) but are not tied to a fixed IMAP
  tagged response text.
- SELECT of a non-selectable hierarchy or container maps to Conflict as an
  operation-aware selection-state conflict, not success.
- No direct state fallback is consulted from API-level failures.

## Ambiguous Results

Ambiguous outcomes are never success:

- Response loss.
- Connection loss.
- Request timeout.
- Daemon restart during request.

If ambiguity occurs before a definitive durable success boundary is crossed, the
result must remain temporary and must not be converted into any success state.

## Durability Boundary

Delivery success is valid only after all required durability steps complete:

- durable raw object storage and durable journal append for delivery; and
- durable journal append for flag and fact mutations.

This contract does not weaken the mutation-journal requirement that commit
success is contingent on durable journal append and recovery-safe ordering.

## Out Of Scope

- Unix domain socket access wiring details.
- Full Cap'n Proto schema layout.
- Full Postfix/Dovecot command behavior.
- LMTP, TCP transport, TLS, and remote auth.
- daemon startup/restart internals.
