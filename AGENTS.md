# AGENTS.md

## Project Overview

WyreBox is a Linux-only Maildir replacement backend for existing Postfix and Dovecot
deployments.

The compatibility path keeps:

- Postfix as the SMTP ingress layer.
- Dovecot as the IMAP protocol layer.
- Existing IMAP clients unchanged.

WyreBox provides:

- A core daemon, `wyreboxd`.
- A Dovecot mail storage backend.
- Postfix ingress integrations.
- Object storage for immutable RFC 5322 message bytes.
- An append-only mutation journal as the canonical write path.
- DuckDB as a materialized query/index store.
- Wirelog for Datalog facts, rules, and derived mailbox views.
- A controlled Cap'n Proto API over a Unix domain socket.

## Current Architectural Decisions

- Supported platform: Linux only.
- Build system: Meson.
- C implementation style: GLib/GObject.
- Cleanup style: use `g_autoptr`, `g_autofree`, `g_auto`, and related cleanup macros
  aggressively.
- Daemon socket: `/run/wyrebox/wyrebox.sock`.
- Daemon API wire format: Cap'n Proto over Unix domain sockets.
- Canonical write path: append-only write-ahead journal.
- DuckDB role: materialized query/index store fed by journal replay and compaction.
- Dovecot role: WyreBox provides a real Dovecot mail storage backend.
- Virtual mailboxes: first-class WyreBox mailbox views backed by Wirelog-derived
  membership.
- Postfix ingress strategy:
  - First: `pipe(8)` delivery helper.
  - Later: LMTP ingress as an additive production-oriented path.
- Licensing model: GPL-3.0-or-later or commercial dual license.

## Implementation Guidelines

- Prefer small, reviewable changes.
- Follow TDD. For behavioral changes, write or update the failing test first, then
  implement the minimum production code needed to pass it, then refactor while tests stay
  green.
- Keep implementation aligned with `.internal-docs/plan.md` and the numbered issue files
  in `.internal-docs/`.
- Do not silently change architectural decisions. If a decision needs to change, update
  the relevant internal planning document first.
- Use `wyrebox/` as the source directory.
- Do not create a separate top-level `include/` directory.
- Keep public/internal headers next to the corresponding source files under `wyrebox/`
  unless a later documented decision changes this layout.
- Use Meson for first-party components, tests, generated configuration examples, and
  install rules.
- Use GObject for long-lived services, adapters, stores, request handlers, and
  plugin-facing abstractions.
- Document ownership transfer rules in public and internal C APIs.
- Prefer GLib primitives for errors, logging, main-loop integration, collections, and
  memory ownership where they fit.
- Do not add manual cleanup ladders when `g_auto*` can express ownership clearly.
- Keep Dovecot ABI and allocation rules in mind. Use GObject wrappers at WyreBox
  boundaries where compatible, but do not fight Dovecot's required ownership model.

## Storage And Mutation Rules

- Raw RFC 5322 message bytes are immutable objects.
- Never rewrite raw message objects for flags, mailbox moves, facts, or virtual views.
- `wyreboxd` is the only writer of:
  - The canonical mutation journal.
  - DuckDB materialized tables.
  - Wirelog fact/rule runtime state.
  - Object-store metadata owned by WyreBox.
- Delivery success must require durable raw object storage and durable journal append.
- DuckDB must not become the sole synchronous mutation authority.
- Journal replay must be able to restore in-memory hot state and DuckDB materialized
  state after restart.

## Dovecot Guidelines

- The target is a real Dovecot mail storage backend, not Maildir plus side indexes.
- WyreBox owns mailbox identity state exposed through the backend.
- Virtual mailboxes are exposed through the WyreBox storage backend as first-class views.
- A raw message may appear in ordinary and virtual mailboxes without object duplication.
- Each mailbox, including virtual mailboxes, has its own UID namespace and UIDVALIDITY.
- Dovecot's built-in virtual plugin may be evaluated only as a supporting integration.

## Postfix Guidelines

- Implement `pipe(8)` ingestion first to prove the delivery contract.
- Add LMTP later without replacing the pipe helper.
- Do not return delivery success to Postfix until `wyreboxd` confirms durable ingestion.
- Map daemon temporary failures to Postfix retry behavior.
- Map daemon permanent validation failures to Postfix permanent failure behavior.
- Include delivery IDs, queue IDs where available, recipients, and journal offsets in
  operational logs.

## API Guidelines

- Use `/run/wyrebox/wyrebox.sock` for the first daemon API transport.
- Use Cap'n Proto schemas for request, response, error, and stream/chunk messages.
- Do not expose arbitrary write SQL.
- Prefer predefined DuckDB query templates for non-admin clients.
- Fact mutation must use explicit API operations.
- Keep TCP API support out of scope until authentication, authorization, and query safety
  controls are designed.

## Testing Guidelines

- TDD is the default development method.
- Each implementation issue should start by translating its acceptance criteria into
  tests or executable checks.
- Do not add production behavior without a corresponding test unless the behavior is
  explicitly untestable in the current phase and the gap is documented.
- Add focused tests for each behavior change.
- Use golden `.eml` fixtures for parsing and byte preservation.
- Verify byte-for-byte fetch of original message content.
- Test journal replay and interrupted materialization.
- Test UID stability across restart.
- Test flag updates without raw object rewrite.
- Test Postfix delivery success, retry, duplicate, and failure behavior.
- Test Dovecot behavior with scripted IMAP commands before relying on manual client
  testing.
- Use Thunderbird smoke tests only after scripted IMAP coverage exists.

## Documentation Guidelines

- Keep `.internal-docs/` as the working planning area.
- Public docs should be created later from accepted internal decisions.
- When creating public docs, prefer:
  - `docs/adr/` for architecture decisions.
  - `docs/contracts/` for stable contracts.
  - `docs/spikes/` for verification notes.
- Keep unresolved questions explicit and convert them into follow-up issues.

## Things Not To Do

- Do not implement a new SMTP server for the first prototype.
- Do not implement a new IMAP server for the first prototype.
- Do not make Maildir the canonical storage model.
- Do not require LLM or sLLM processing for ingestion, search, or IMAP compatibility.
- Do not let Postfix or Dovecot helpers open mutable DuckDB/object-store state directly.
- Do not introduce non-Linux portability work unless the project scope changes.
