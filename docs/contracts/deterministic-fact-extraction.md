# Deterministic Fact Extraction Contract

## Status

Accepted for deterministic metadata fact extraction.

## Scope

Deterministic fact extraction is a local metadata-only pipeline. It derives facts
from parsed `WyreboxEmlMetadata` values and caller-supplied in-memory rules.

The extractor does not require a probabilistic service, network service,
external text service, body scanner, persistence layer, reload scheduler,
daemon API, DuckDB schema change, or Wirelog runtime rule loading.

`wyreboxd` runs this extractor on delivered messages with rules read from a
configuration file. That wiring, the rules file format, and the journal record
are described in `docs/contracts/linux-runtime.md` and
`docs/contracts/mutation-journal.md`.

## Fact Record Shape

Each extracted fact is represented as a `WyreboxFactRecord` with:

- `predicate`: the fact predicate name.
- `args`: ordered string arguments.
- `source`: provenance for the extractor and source field.
- `confidence_ppm`: exact deterministic confidence. Deterministic extraction
  emits `1000000`.
- `created_at_unix_us`: the creation timestamp supplied to extraction.
- `retracted_at_unix_us`: zero for active facts, or a nonzero retraction
  timestamp for retraction records.

## Header Facts

Header-derived facts are emitted from parsed metadata before caller-supplied
rule facts.

Supported header-derived predicates are:

- `message_id(mail_id, rfc_message_id)` from `Message-ID`.
- `sender_domain(mail_id, domain)` from the first valid normalized address in
  `From`.
- `participant(mail_id, normalized_address)` for each valid address in `From`,
  `To`, `Cc`, and `Bcc`.
- `participant_display_name(mail_id, normalized_address, display_name)` when
  an address has a display name.
- `sent_at(mail_id, raw_date_value)` from `Date`.
- `replies_to(mail_id, rfc_message_id)` from `In-Reply-To`.
- `references(mail_id, rfc_message_id)` from `References`.
- `list_id(mail_id, identifier)` from the identifier inside `List-Id` angle
  brackets.
- `delivered_to(mail_id, normalized_address)` for each valid address in
  `Delivered-To`, or in `X-Original-To` when `Delivered-To` is absent.

Header provenance uses `header:<field>`, such as `header:message-id`,
`header:from`, `header:to`, `header:cc`, `header:bcc`, `header:date`,
`header:in-reply-to`, `header:references`, `header:list-id`,
`header:delivered-to`, and `header:x-original-to`. Participant and display-name
facts use the provenance of their source address field.

## Dictionary Project Keywords

Dictionary rules are caller-supplied in-memory rules over parsed metadata
fields. Supported fields are `subject`, `from`, `to`, `cc`, and `bcc`.

Each dictionary rule contains a field, rule id, match text, and canonical
project key. Rules match the decoded working value without modifying parsed
metadata. A matching rule emits:

`project_keyword(mail_id, canonical_project_key)`

Dictionary matching is an exact case-insensitive substring check over the
selected decoded working value. For valid UTF-8 strings, matching uses GLib
casefolding as implemented by the extractor. Dictionary provenance uses:

`dictionary:<field>:<rule-id>`

Dictionary facts are emitted after header facts in caller rule order.

## Regex Candidates

Regex rules are caller-supplied in-memory rules over decoded working values for
the supported parsed metadata fields.
Supported fields are `subject`, `from`, `to`, `cc`, and `bcc`.

Each regex rule contains a field, rule id, predicate, pattern, and capture
group. Capture group `0` emits the full match. A positive capture group emits
that capture. Unmatched and empty captures emit no fact.

Supported regex candidate predicates are:

- `amount_candidate(mail_id, value)`.
- `date_candidate(mail_id, value)`.
- `reference_candidate(mail_id, value)`.

Regex matching uses GLib `GRegex`. Regex provenance uses:

`regex:<field>:<rule-id>`

Regex facts are emitted after header and dictionary facts. Regex output is
deterministic in caller rule order, then match order within each selected
metadata field.

## Ordering

Extraction output order is deterministic:

1. Header facts in extractor-defined header order.
2. Dictionary facts in caller rule order.
3. Regex facts in caller rule order, then match order.

Header order is message ID, sender domain, participant facts grouped by `From`,
`To`, `Cc`, and `Bcc`, date, reply/reference identifiers, then List-Id and
delivery recipients. A display-name fact immediately follows its participant
fact.

Unchanged inputs and unchanged caller rule order produce the same active fact
snapshot order.

## Snapshot Reconciliation

Re-running extraction produces a new active fact snapshot. Reconciliation is an
in-memory helper over a previous active snapshot and a new active snapshot.

Fact identity for reconciliation is:

`predicate + args + source`

Creation and retraction timestamps are ignored for identity equality.
Reconciliation emits retractions before inserts. Retractions are ordered by the
previous snapshot order. Inserts are ordered by the new snapshot order.

The reconciliation helper returns owned change records and does not mutate input
records. It does not claim persistence, reload scheduling, journal writes,
daemon calls, or Wirelog runtime updates.

## Wirelog Export

Facts can be serialized through the fact record serialization APIs for Wirelog
text export. This contract covers serialization of fact records and does not
claim runtime rule loading or execution in Wirelog.

## Normalization

Parsed metadata strings are preserved as parsed. Current extraction does not
perform stemming, transliteration, locale collation, semantic normalization,
natural-language processing, or multilingual normalization.

For extraction only, RFC 2047 B and Q encoded words in `Subject`, `From`, `To`,
`Cc`, and `Bcc` are decoded to UTF-8. Whitespace between adjacent encoded words
is suppressed as required by RFC 2047. If a word has an unknown charset or
cannot be decoded, dictionary and regex matching for that field falls back to
its original unfolded value. For address facts, the affected display name falls
back to its raw unfolded display-name text while the raw address syntax is
still parsed. Parsed metadata remains unchanged.

Address lists are split deterministically from their raw syntax, including RFC
5322 groups, quoted display names, and comments. Encoded display names are
decoded only after list splitting; RFC 2047 encoded-word contents are treated
as opaque during splitting so decoded punctuation cannot create synthetic
address boundaries. Each syntactically valid address is emitted separately;
display names are emitted in `participant_display_name`. The local part is
preserved including case; the domain is lowercased. Display names and comments
are not included in `participant`. Invalid address-list members emit no
participant fact. `List-Id` emits only when an angle-bracketed identifier is
present. Non-ASCII domains must be valid UTF-8 hostnames and use
Unicode-aware lowercase conversion.

Dictionary matching uses the existing deterministic GLib casefold behavior for
valid UTF-8 strings. Future normalization must be explicit, deterministic, and
covered by tests.

## Out Of Scope

The deterministic fact extraction contract does not define body scanning,
probabilistic classification, external text services, network lookups,
persistent dictionary storage, rule reload lifecycle, fact retraction scheduling,
Wirelog runtime rule loading, DuckDB materialization, daemon API wiring, or
schema changes.
