#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "core-schema.md"

REQUIRED_SECTIONS = [
    "# Core Schema Contract",
    "## Status",
    "## Scope",
    "## Canonical State And Materialized DuckDB Boundary",
    "## Stable Identity And Reference Model",
    "## Required Materialized Schema Objects",
    "## Mailbox And Virtual Mailbox UID Contract",
    "## Flags And Keywords As Materialized State",
    "## Schema Versioning And Migration Policy",
    "## Restart And Replay Regeneration Expectations",
    "## Query And API Safety Boundary",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing contract text: {needle}"


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
        r"Accepted for issue 0003",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"DuckDB materialized-schema shape and ownership rules only",
    )

    assert_section_matches(
        sections,
        "## Canonical State And Materialized DuckDB Boundary",
        r"append-only mutation journal is the canonical",
    )
    assert_section_matches(
        sections,
        "## Canonical State And Materialized DuckDB Boundary",
        r"DuckDB is not the canonical mutation authority",
    )
    assert_section_matches(
        sections,
        "## Canonical State And Materialized DuckDB Boundary",
        r"raw RFC 5322 objects remain immutable",
    )

    assert_section_matches(
        sections,
        "## Stable Identity And Reference Model",
        r"message_id",
    )
    assert_section_matches(
        sections,
        "## Stable Identity And Reference Model",
        r"object_id",
    )
    assert_section_matches(
        sections,
        "## Stable Identity And Reference Model",
        r"mailbox_id",
    )
    assert_section_matches(
        sections,
        "## Stable Identity And Reference Model",
        r"view_id",
    )
    assert_section_matches(
        sections,
        "## Stable Identity And Reference Model",
        r"immutable content.*object key",
    )

    for table in [
        "messages",
        "objects",
        "mailbox_memberships",
        "mailbox_uid_state",
        "message_flags",
        "message_keywords",
        "message_facts",
        "derived_view_memberships",
        "schema_metadata",
        "materialization_checkpoint",
    ]:
        assert_in_section(sections, "## Required Materialized Schema Objects", f"`{table}`")

    assert_section_matches(
        sections,
        "## Mailbox And Virtual Mailbox UID Contract",
        r"each virtual mailbox.*own.*UID namespace",
    )
    assert_section_matches(
        sections,
        "## Mailbox And Virtual Mailbox UID Contract",
        r"UIDVALIDITY",
    )
    assert_section_matches(
        sections,
        "## Mailbox And Virtual Mailbox UID Contract",
        r"Wirelog-derived",
    )
    assert_section_matches(
        sections,
        "## Mailbox And Virtual Mailbox UID Contract",
        r"not .* duplicat",
    )

    assert_section_matches(
        sections,
        "## Flags And Keywords As Materialized State",
        r"flags and keywords are not stored in raw",
    )
    assert_section_matches(
        sections,
        "## Flags And Keywords As Materialized State",
        r"FlagChanged",
    )
    assert_section_matches(
        sections,
        "## Flags And Keywords As Materialized State",
        r"KeywordChanged",
    )

    assert_section_matches(
        sections,
        "## Schema Versioning And Migration Policy",
        r"schema_metadata.*schema_version",
    )
    assert_section_matches(
        sections,
        "## Schema Versioning And Migration Policy",
        r"Explicit migration",
    )
    assert_section_matches(
        sections,
        "## Schema Versioning And Migration Policy",
        r"must reject unknown versions",
    )

    assert_section_matches(
        sections,
        "## Restart And Replay Regeneration Expectations",
        r"materialized tables are rebuildable",
    )
    assert_section_matches(
        sections,
        "## Restart And Replay Regeneration Expectations",
        r"journal plus object-store inputs",
    )
    assert_section_matches(
        sections,
        "## Restart And Replay Regeneration Expectations",
        r"materialization_checkpoint.*durable replay position",
    )

    assert_section_matches(
        sections,
        "## Query And API Safety Boundary",
        r"non-admin clients must not be able to execute arbitrary write SQL",
    )
    assert_section_matches(
        sections,
        "## Query And API Safety Boundary",
        r"read/materialized-query oriented",
    )

    for prohibited in [
        r"raw bytes.*rewritten.*flags",
        r"duplicate raw object",
        r"raw object .* duplicated",
    ]:
        assert_forbidden(text, prohibited)


if __name__ == "__main__":
    main()
