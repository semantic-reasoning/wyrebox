# Deterministic Fact Extraction Contract

## Status

Accepted for deterministic metadata fact extraction.

## Scope

Deterministic fact extraction is a local metadata-only pipeline. It derives facts
from parsed `WyreboxEmlMetadata` values and caller-supplied in-memory rules.

The extractor does not require a probabilistic service, network service,
external text service, body scanner, persistence layer, reload scheduler,
daemon API, DuckDB schema change, or Wirelog runtime rule loading.

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
- `sender_domain(mail_id, domain)` from `From`.
- `participant(mail_id, raw_header_value)` from `From`, `To`, `Cc`, and `Bcc`.
- `sent_at(mail_id, raw_date_value)` from `Date`.
- `replies_to(mail_id, rfc_message_id)` from `In-Reply-To`.
- `references(mail_id, rfc_message_id)` from `References`.

Header provenance uses `header:<field>`, such as `header:message-id`,
`header:from`, `header:to`, `header:cc`, `header:bcc`, `header:date`,
`header:in-reply-to`, and `header:references`.

## Dictionary Project Keywords

Dictionary rules are caller-supplied in-memory rules over parsed metadata
fields. Supported fields are `subject`, `from`, `to`, `cc`, and `bcc`.

Each dictionary rule contains a field, rule id, match text, and canonical
project key. A matching rule emits:

`project_keyword(mail_id, canonical_project_key)`

Dictionary matching is an exact case-insensitive substring check over the
selected parsed metadata field. For valid UTF-8 strings, matching uses GLib
casefolding as implemented by the extractor. Dictionary provenance uses:

`dictionary:<field>:<rule-id>`

Dictionary facts are emitted after header facts in caller rule order.

## Regex Candidates

Regex rules are caller-supplied in-memory rules over parsed metadata fields.
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

Dictionary matching uses the existing deterministic GLib casefold behavior where
implemented for valid UTF-8 strings. Future normalization must be explicit,
deterministic, and covered by tests.

## Out Of Scope

The deterministic fact extraction contract does not define body scanning,
probabilistic classification, external text services, network lookups,
persistent dictionary storage, rule reload lifecycle, fact retraction scheduling,
Wirelog runtime rule loading, DuckDB materialization, daemon API wiring, or
schema changes.
