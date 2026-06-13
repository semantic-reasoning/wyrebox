#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "deterministic-fact-extraction.md"

REQUIRED_SECTIONS = [
    "# Deterministic Fact Extraction Contract",
    "## Status",
    "## Scope",
    "## Fact Record Shape",
    "## Header Facts",
    "## Dictionary Project Keywords",
    "## Regex Candidates",
    "## Ordering",
    "## Snapshot Reconciliation",
    "## Wirelog Export",
    "## Normalization",
    "## Out Of Scope",
]

HEADER_PREDICATES = [
    "message_id",
    "sender_domain",
    "participant",
    "sent_at",
    "replies_to",
    "references",
]

REGEX_PREDICATES = [
    "amount_candidate",
    "date_candidate",
    "reference_candidate",
]

METADATA_FIELDS = [
    "subject",
    "from",
    "to",
    "cc",
    "bcc",
]

PROVENANCE_FORMATS = [
    "header:<field>",
    "dictionary:<field>:<rule-id>",
    "regex:<field>:<rule-id>",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_contains(section: str, needle: str, body: str) -> None:
    assert needle in body, f"{section} missing required text: {needle}"


def assert_matches(section: str, pattern: str, body: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, body, re.IGNORECASE | re.DOTALL), (
        f"{section} missing required pattern: {pattern}"
    )


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains forbidden language: {pattern}"
    )


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")
    sections = section_map(text)

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(
        missing_sections
    )

    assert_matches("## Scope", r"local metadata-only pipeline", sections["## Scope"])
    assert_matches(
        "## Scope",
        r"does not require a probabilistic service",
        sections["## Scope"],
    )
    assert_matches(
        "## Scope",
        r"network service",
        sections["## Scope"],
    )
    assert_matches(
        "## Scope",
        r"external text service",
        sections["## Scope"],
    )

    for field in [
        "predicate",
        "args",
        "source",
        "confidence_ppm",
        "created_at_unix_us",
        "retracted_at_unix_us",
        "1000000",
    ]:
        assert_contains("## Fact Record Shape", field, sections["## Fact Record Shape"])

    for predicate in HEADER_PREDICATES:
        assert_contains("## Header Facts", predicate, sections["## Header Facts"])

    assert_contains(
        "## Dictionary Project Keywords",
        "project_keyword(mail_id, canonical_project_key)",
        sections["## Dictionary Project Keywords"],
    )
    for field in METADATA_FIELDS:
        assert_contains("## Dictionary Project Keywords", field, sections["## Dictionary Project Keywords"])
        assert_contains("## Regex Candidates", field, sections["## Regex Candidates"])

    for predicate in REGEX_PREDICATES:
        assert_contains("## Regex Candidates", predicate, sections["## Regex Candidates"])

    for provenance in PROVENANCE_FORMATS:
        assert_contains("contract", provenance, text)

    assert_matches(
        "## Ordering",
        r"Header facts.*Dictionary facts.*Regex facts",
        sections["## Ordering"],
    )
    assert_matches(
        "## Ordering",
        r"Dictionary facts in caller rule order",
        sections["## Ordering"],
    )
    assert_matches(
        "## Ordering",
        r"Regex facts in caller rule order, then match order",
        sections["## Ordering"],
    )

    assert_contains(
        "## Snapshot Reconciliation",
        "predicate + args + source",
        sections["## Snapshot Reconciliation"],
    )
    assert_matches(
        "## Snapshot Reconciliation",
        r"retractions before inserts",
        sections["## Snapshot Reconciliation"],
    )
    assert_matches(
        "## Snapshot Reconciliation",
        r"does not mutate input records",
        sections["## Snapshot Reconciliation"],
    )
    assert_matches(
        "## Snapshot Reconciliation",
        r"does not claim persistence",
        sections["## Snapshot Reconciliation"],
    )

    assert_matches(
        "## Wirelog Export",
        r"serialized through the fact record serialization APIs",
        sections["## Wirelog Export"],
    )
    assert_matches(
        "## Wirelog Export",
        r"does not claim runtime rule loading",
        sections["## Wirelog Export"],
    )

    for deferred in [
        "Parsed metadata strings are preserved as parsed",
        "GLib casefold",
        "stemming",
        "transliteration",
        "locale collation",
        "semantic normalization",
        "natural-language processing",
        "multilingual normalization",
    ]:
        assert_contains("## Normalization", deferred, sections["## Normalization"])
    assert_matches(
        "## Normalization",
        r"explicit, deterministic, and covered by tests",
        sections["## Normalization"],
    )

    for out_of_scope in [
        "body scanning",
        "probabilistic classification",
        "external text services",
        "network lookups",
        "persistent dictionary storage",
        "rule reload lifecycle",
        "Wirelog runtime rule loading",
        "DuckDB materialization",
        "daemon API wiring",
        "schema changes",
    ]:
        assert_contains("## Out Of Scope", out_of_scope, sections["## Out Of Scope"])

    for forbidden in [
        r"(?<!not )\brequires?\s+a\s+probabilistic service\b",
        r"(?<!not )\brequires?\s+a\s+network service\b",
        r"(?<!not )\brequires?\s+an\s+external text service\b",
        r"(?<!not )\bperforms?\s+body scanning\b",
        r"(?<!not )\buses?\s+stemming\b",
        r"(?<!not )\buses?\s+transliteration\b",
        r"(?<!not )\buses?\s+semantic normalization\b",
        r"(?<!not )\bloads?\s+Wirelog runtime rules\b",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
