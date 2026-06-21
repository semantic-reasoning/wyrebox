# WyreBox Postfix LMTP Examples

These examples define the Postfix LMTP handoff contract for a WyreBox LMTP
delivery helper.

The installed `wyrebox-postfix-lmtp` executable is a transcript helper used by
tests and supervised adapters. It reads a complete LMTP transcript from
standard input, submits the raw DATA bytes and envelope metadata to
`wyreboxd`, writes exactly one terminal LMTP reply on standard output, and
writes sanitized operational context on standard error. It is not a Postfix
`lmtp(8)` transport command and must not be installed as `argv=` in
`master.cf`.

Postfix LMTP transport requires a WyreBox LMTP delivery endpoint. Once that
delivery helper is available, run it on `/run/wyrebox/wyrebox-lmtp.sock` and
let the Postfix `lmtp(8)` client connect to that socket. The helper should use
the WyreBox daemon API at `/run/wyrebox/wyrebox.sock` for durable ingestion.

The current LMTP helper supports one accepted recipient per LMTP transaction.
Postfix must split recipients before delivery with the transport-specific
recipient limit:

```postfix
wyrebox-lmtp_destination_recipient_limit = 1
```

With that setting, Postfix opens a delivery transaction for one recipient at a
time; multiple `RCPT TO` commands in one transaction are rejected by this
helper slice.
Transactions with more than one accepted recipient and recipient-level mixed outcomes are future work that require a daemon response model carrying per-recipient delivery results.

Example helper invocation shape:

```sh
wyrebox-postfix-lmtp \
  --account-id example-account \
  --delivery-id "$queue_id:$recipient" \
  --socket /run/wyrebox/wyrebox.sock
```

Supported options are:

- `--account-id`: required WyreBox account identity.
- `--delivery-id`: required stable delivery identity for this helper
  invocation.
- `--socket`: optional WyreBox daemon Unix socket path.

If `--socket` is omitted, the helper connects to
`/run/wyrebox/wyrebox.sock`.

LMTP success is returned only after `wyreboxd` reports durable ingestion.
Temporary daemon failures, local connection failures, malformed daemon
responses, and ambiguous no-response failures produce a temporary LMTP reply so
Postfix may retry. Permanent daemon validation failures produce a permanent
LMTP reply.

The helper logs request identity and durable receipt context, but it does not
log raw message bytes, daemon free-form error text, or local parser error text.

The example `master.cf` disables chroot for the LMTP client service so
`/run/wyrebox/wyrebox-lmtp.sock` resolves as a normal host path. If chroot is
enabled, the LMTP delivery socket must be visible inside the Postfix chroot.
The daemon API socket is used by the WyreBox delivery helper process, not by
the Postfix `lmtp(8)` client.

See `master.cf.wyrebox-lmtp` for the service shape and
`transport.wyrebox-lmtp` for a transport map example.
