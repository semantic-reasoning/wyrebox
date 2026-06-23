# WyreBox Postfix Pipe Examples

These examples show the Postfix `pipe(8)` handoff to the installed
`wyrebox-postfix-pipe` delivery helper.

`wyrebox-postfix-pipe` reads the raw RFC 5322 message bytes from standard
input and preserves the bytes it receives on standard input. Postfix envelope
metadata is passed as command-line options:

```sh
wyrebox-postfix-pipe \
  --account-id example-account \
  --delivery-id "$delivery_id" \
  --queue-id "$queue_id" \
  --sender "$sender" \
  --recipient alice@example.net \
  --recipient bob@example.net \
  --socket /run/wyrebox/wyrebox.sock
```

Supported options are:

- `--account-id`: required WyreBox account identity.
- `--delivery-id`: required delivery identity for this helper invocation.
- `--queue-id`: optional Postfix queue identity.
- `--sender`: optional envelope sender.
- `--recipient`: required envelope recipient; repeat it for multiple
  recipients.
- `--socket`: optional WyreBox daemon Unix socket path.

If `--socket` is omitted, the helper connects to
`/run/wyrebox/wyrebox.sock`. Use `--socket /path/to/wyrebox.sock` only when the
daemon is listening at a different Unix socket path.

The Postfix `pipe(8)` service must not use pipe flags that alter message
content. The example keeps only `flags=q`; the q flag affects command-line
address macro expansion only and does not change the message bytes passed to
the helper.

Set the transport-specific recipient limit in `main.cf` so `${recipient}` is
single-valued for each helper invocation:

```postfix
wyrebox-pipe_destination_recipient_limit = 1
```

The helper returns success only after `wyreboxd` reports durable ingestion.
Temporary daemon failures, local connection failures, and ambiguous
no-response failures return temporary failure so Postfix may retry. Permanent
daemon validation failures return permanent failure.

The helper logs delivery context and daemon receipt context, but it does not
log raw message bytes.

For chrooted Postfix services, the helper can connect only if the configured
socket path is visible inside the chroot. Disable chroot for this service or
make the socket path available at the same path inside the chroot.

Duplicate and idempotency policy is owned by daemon ingestion. If the helper
cannot tell whether a request reached the daemon, it returns temporary failure;
Postfix may retry the delivery.

See `master.cf.wyrebox-pipe` for the pipe service shape.

Two transport-map examples are provided:

- `transport.wyrebox-pipe` for the traditional `hash:` workflow that uses
  `postmap` and a compiled transport map.
- `transport.wyrebox-pipe-regexp` for a distro-compatible `regexp:` workflow
  that does not require Berkeley DB-backed transport-map support.
