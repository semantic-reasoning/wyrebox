#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "mutation-journal.md"

REQUIRED_SECTIONS = [
    "# Mutation Journal Contract",
    "## Status",
    "## Scope",
    "## Writer Ownership And Authority",
    "## Journal Root And Path Policy",
    "## Append-Only Record Model",
    "## Record Envelope",
    "## Canonical Event Names",
    "## Durability Semantics",
    "## Replay Behavior",
    "## Object Store Consistency Boundary",
    "## Delivery Occurrence Identity Boundary",
    "## Raw Object Immutability",
]

REQUIRED_EVENTS = [
    "MessageDelivered",
    "FlagChanged",
    "KeywordChanged",
    "FactInserted",
    "FactRetracted",
    "DerivedViewMembershipChanged",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing contract: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains forbidden or contradictory language: {pattern}"
    )


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(
        sections,
        "## Status",
        r"Accepted for the mutation-journal implementation unit of issue 0003",
    )

    assert_section_matches(
        sections,
        "## Scope",
        r"This contract defines only the canonical mutation journal contract",
    )
    for excluded in [
        "C implementation",
        "DuckDB DDL or schema",
        "daemon socket API",
        "Cap'n Proto schemas",
        "Postfix or Dovecot integration",
        "Wirelog rule execution",
        "compaction implementation",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    assert_section_matches(
        sections,
        "## Writer Ownership And Authority",
        r"`wyreboxd` is the only writer of the canonical append-only journal",
    )
    assert_section_matches(
        sections,
        "## Writer Ownership And Authority",
        r"DuckDB is materialized query and index state",
    )
    assert_section_matches(
        sections,
        "## Writer Ownership And Authority",
        r"DuckDB is never the synchronous mutation authority",
    )

    assert_section_matches(
        sections,
        "## Journal Root And Path Policy",
        r"production journal root is durable WyreBox state under\s+`/var/lib/wyrebox/`",
    )
    assert_section_matches(
        sections,
        "## Journal Root And Path Policy",
        r"default journal directory is\s+`/var/lib/wyrebox/journal`",
    )
    assert_section_matches(
        sections,
        "## Journal Root And Path Policy",
        r"must accept a caller-provided root directory",
    )
    assert_section_matches(
        sections,
        "## Journal Root And Path Policy",
        r"Tests.*may use temporary roots",
    )

    assert_section_matches(
        sections,
        "## Append-Only Record Model",
        r"append-only sequence of records",
    )
    assert_section_matches(
        sections,
        "## Append-Only Record Model",
        r"record's bytes are immutable and must not be\s+rewritten in place",
    )
    assert_section_matches(
        sections,
        "## Append-Only Record Model",
        r"journal offset is the stable byte offset",
    )
    assert_section_matches(
        sections,
        "## Append-Only Record Model",
        r"must not renumber committed\s+records",
    )
    assert_section_matches(
        sections,
        "## Append-Only Record Model",
        r"strictly increasing sequence number",
    )

    for envelope_field in [
        "`version`",
        "`event_type`",
        "`payload`",
        "`sequence`",
        "`checksum`",
    ]:
        assert_in_section(sections, "## Record Envelope", envelope_field)
    assert_section_matches(
        sections,
        "## Record Envelope",
        r"payload bytes or a structured payload",
    )
    assert_section_matches(
        sections,
        "## Record Envelope",
        r"detect an invalid trailing record",
    )

    for event in REQUIRED_EVENTS:
        assert_in_section(sections, "## Canonical Event Names", f"`{event}`")

    assert_section_matches(
        sections,
        "## Durability Semantics",
        r"Delivery success requires both durable raw object storage and durable journal\s+append",
    )
    assert_section_matches(
        sections,
        "## Durability Semantics",
        r"must not return\s+delivery success before the raw object bytes are durable and the journal record",
    )
    assert_section_matches(
        sections,
        "## Durability Semantics",
        r"group commit is allowed only when the daemon waits",
    )
    assert_section_matches(
        sections,
        "## Durability Semantics",
        r"Flag and keyword mutations are canonical only after the corresponding\s+`FlagChanged` or `KeywordChanged` record is durably appended",
    )
    assert_section_matches(
        sections,
        "## Durability Semantics",
        r"acknowledging success before durable journal\s+append is not allowed",
    )

    assert_section_matches(
        sections,
        "## Replay Behavior",
        r"Replay processes valid records in journal order",
    )
    assert_section_matches(
        sections,
        "## Replay Behavior",
        r"torn or invalid trailing record must be detected explicitly",
    )
    assert_section_matches(
        sections,
        "## Replay Behavior",
        r"must not apply the torn or invalid trailing record",
    )
    assert_section_matches(
        sections,
        "## Replay Behavior",
        r"restore in-memory hot state and feed DuckDB\s+materialization",
    )
    assert_section_matches(
        sections,
        "## Replay Behavior",
        r"DuckDB contents do not replace the journal",
    )

    assert_section_matches(
        sections,
        "## Object Store Consistency Boundary",
        r"raw object.*must exist durably.*before the `MessageDelivered` journal record",
    )
    assert_section_matches(
        sections,
        "## Object Store Consistency Boundary",
        r"orphaned durable object without a committed `MessageDelivered` event is\s+recoverable garbage, not delivered mail",
    )
    assert_section_matches(
        sections,
        "## Object Store Consistency Boundary",
        r"must not synthesize delivered mail from\s+missing raw bytes",
    )

    assert_section_matches(
        sections,
        "## Delivery Occurrence Identity Boundary",
        r"`journal_offset` plus `journal_sequence` identifies a committed delivery/projection occurrence",
    )
    assert_section_matches(
        sections,
        "## Delivery Occurrence Identity Boundary",
        r"`object_key` identifies immutable raw RFC 5322 bytes only",
    )
    assert_section_matches(
        sections,
        "## Delivery Occurrence Identity Boundary",
        r"same `object_key` may back multiple committed `MessageDelivered` records",
    )
    assert_section_matches(
        sections,
        "## Delivery Occurrence Identity Boundary",
        r"does not define duplicate suppression, retry idempotency, same RFC Message-ID policy, or broader duplicate policy",
    )

    assert_section_matches(
        sections,
        "## Raw Object Immutability",
        r"Raw RFC 5322 object bytes are never rewritten for journaled mutations",
    )
    assert_section_matches(
        sections,
        "## Raw Object Immutability",
        r"Flags,\s+keywords, mailbox moves, facts, virtual mailbox membership",
    )

    for forbidden in [
        r"DuckDB is the synchronous mutation authority",
        r"DuckDB is the canonical mutation authority",
        r"(may|can) return [^\n.]*delivery success before [^\n.]*journal",
        r"Postfix [^\n.]*append [^\n.]*journal records directly",
        r"Dovecot [^\n.]*append [^\n.]*journal records directly",
        r"raw .*object bytes (may|can|are) [^\n.]*rewritten[^\n.]*flags",
        r"orphaned durable objects? [^\n.]*are delivered mail",
        r"must force `/var/lib/wyrebox`",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
