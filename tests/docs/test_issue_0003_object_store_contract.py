#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "object-store.md"

REQUIRED_SECTIONS = [
    "# Object Store Contract",
    "## Status",
    "## Scope",
    "## Object Identity",
    "## Local Layout",
    "## Byte Preservation",
    "## Immutable Write Behavior",
    "## Fetch And Failure Behavior",
    "## Component Boundary",
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
        r"Accepted for the first implementation unit of issue 0003",
    )

    for excluded in [
        "DuckDB table DDL",
        "append-only journal event format",
        "daemon socket API",
        "Cap'n Proto schema",
        "Postfix or Dovecot integration",
        "Wirelog fact or rule storage",
        "UID or UIDVALIDITY logic",
        "non-local object storage providers",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    assert_section_matches(
        sections,
        "## Object Identity",
        r"`sha256:<64 lowercase hex chars>`",
    )
    assert_section_matches(
        sections,
        "## Object Identity",
        r"computed over the exact input byte sequence",
    )
    assert_section_matches(
        sections,
        "## Object Identity",
        r"same bytes returns the same object key",
    )
    assert_section_matches(
        sections,
        "## Object Identity",
        r"different bytes returns different object keys",
    )
    assert_section_matches(
        sections,
        "## Object Identity",
        r"identity only inside the object-store API",
    )
    assert_section_matches(
        sections,
        "## Object Identity",
        r"does not identify a delivered-message occurrence",
    )

    assert_in_section(
        sections,
        "## Local Layout",
        "`<root>/objects/sha256/<first-two-hex>/<full-hex>.eml`",
    )
    assert_section_matches(
        sections,
        "## Local Layout",
        r"must not force `/var/lib/wyrebox`",
    )
    assert_section_matches(
        sections,
        "## Local Layout",
        r"object key is the API identity",
    )

    assert_section_matches(
        sections,
        "## Byte Preservation",
        r"stores original RFC 5322 message bytes unchanged",
    )
    assert_section_matches(
        sections,
        "## Byte Preservation",
        r"returns the original bytes byte-for-byte",
    )
    assert_section_matches(
        sections,
        "## Byte Preservation",
        r"must not rewrite raw object bytes",
    )

    assert_section_matches(
        sections,
        "## Immutable Write Behavior",
        r"does not already exist",
    )
    assert_section_matches(
        sections,
        "## Immutable Write Behavior",
        r"duplicate put of identical bytes returns the same key and does not rewrite",
    )
    assert_section_matches(
        sections,
        "## Immutable Write Behavior",
        r"existing object is not overwritten",
    )

    assert_section_matches(
        sections,
        "## Fetch And Failure Behavior",
        r"Fetching a valid existing object key returns the stored object bytes",
    )
    assert_section_matches(
        sections,
        "## Fetch And Failure Behavior",
        r"Invalid object keys fail with `GError`",
    )
    assert_section_matches(
        sections,
        "## Fetch And Failure Behavior",
        r"Missing objects addressed by otherwise valid keys also fail with `GError`",
    )

    assert_section_matches(
        sections,
        "## Component Boundary",
        r"GLib/GObject component located under\s+`wyrebox/`",
    )
    assert_section_matches(
        sections,
        "## Component Boundary",
        r"does not introduce a top-level `include/` directory",
    )

    for forbidden in [
        r"Maildir [^\n.]*canonical",
        r"raw object bytes [^\n.]*rewritten",
        r"must force `/var/lib/wyrebox`",
        r"object key [^\n.]*\.eml",
        r"DuckDB [^\n.]*implemented",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
