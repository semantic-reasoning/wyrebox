#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0001-postfix-ingress-boundary.md"

REQUIRED_SECTIONS = [
    "# ADR 0001: Postfix Ingress Boundary",
    "## Status",
    "## Context",
    "## Decision",
    "## Helper Input Expectations",
    "## Envelope Metadata Assumptions",
    "## Durable Ingestion Success Boundary",
    "## Failure Mapping",
    "## Duplicate Delivery Risk",
    "## Chroot And Privilege-Drop Implications",
    "## Daemon Socket Dependency",
    "## State Mutation Boundary",
    "## Deferred Decisions",
    "## Consequences",
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


def main() -> None:
    assert ADR_PATH.is_file(), f"missing ADR: {ADR_PATH}"

    text = ADR_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_in_section(
        sections,
        "## Decision",
        "The first Postfix ingress implementation path is a `pipe(8)` delivery helper.",
    )
    assert_in_section(
        sections,
        "## Decision",
        "LMTP ingress is later additive work, not a replacement decision now.",
    )
    assert_in_section(
        sections,
        "## Decision",
        "Dovecot LMTP is deferred until the WyreBox Dovecot storage backend is proven.",
    )

    assert_section_matches(
        sections,
        "## Helper Input Expectations",
        r"complete message on standard\s+input as the original RFC 5322 byte stream",
    )
    assert_section_matches(
        sections,
        "## Helper Input Expectations",
        r"must not parse\s+and rewrite the message as part of delivery",
    )

    assert_section_matches(
        sections,
        "## Envelope Metadata Assumptions",
        r"envelope sender.*target recipient.*Postfix queue ID.*delivery ID",
    )
    assert_section_matches(
        sections,
        "## Envelope Metadata Assumptions",
        r"pipe arguments, environment variables, or configured metadata",
    )
    assert_section_matches(
        sections,
        "## Envelope Metadata Assumptions",
        r"must not infer required envelope metadata from message headers or body content alone",
    )

    assert_section_matches(
        sections,
        "## Durable Ingestion Success Boundary",
        r"Postfix success is returned only after `wyreboxd` confirms durable raw object\s+storage and durable journal append",
    )
    assert_section_matches(
        sections,
        "## Durable Ingestion Success Boundary",
        r"DuckDB materialization,\s+Wirelog derivation, search indexing, and other asynchronous views may lag",
    )

    assert_section_matches(
        sections,
        "## Failure Mapping",
        r"temporary failure maps to Postfix retry behavior",
    )
    assert_section_matches(
        sections,
        "## Failure Mapping",
        r"unavailable `/run/wyrebox/wyrebox\.sock`.*daemon overload.*transient storage errors.*permission",
    )
    assert_section_matches(
        sections,
        "## Failure Mapping",
        r"permanent failure maps to Postfix permanent failure behavior",
    )
    assert_section_matches(
        sections,
        "## Failure Mapping",
        r"validation failures.*configuration failures",
    )

    assert_section_matches(
        sections,
        "## Duplicate Delivery Risk",
        r"Postfix may\s+then retry the delivery, creating duplicate delivery risk",
    )
    assert_section_matches(
        sections,
        "## Duplicate Delivery Risk",
        r"duplicate delivery is expected.*idempotent or detect duplicates",
    )

    assert_section_matches(
        sections,
        "## Chroot And Privilege-Drop Implications",
        r"restricted users, groups, and\s+chroot settings",
    )
    assert_section_matches(
        sections,
        "## Chroot And Privilege-Drop Implications",
        r"privilege drop and still reach the daemon socket",
    )
    assert_section_matches(
        sections,
        "## Chroot And Privilege-Drop Implications",
        r"`/run/wyrebox/wyrebox\.sock` inside that chroot",
    )

    assert_in_section(
        sections,
        "## Daemon Socket Dependency",
        "`/run/wyrebox/wyrebox.sock`",
    )

    assert_section_matches(
        sections,
        "## State Mutation Boundary",
        r"must not mutate DuckDB, Wirelog, object-store metadata, or the\s+canonical mutation journal directly",
    )
    assert_section_matches(
        sections,
        "## State Mutation Boundary",
        r"Only `wyreboxd` writes canonical state",
    )

    assert_section_matches(
        sections,
        "## Deferred Decisions",
        r"Dovecot storage-backend decisions are deferred.*next atomic unit",
    )
    assert_section_matches(
        sections,
        "## Deferred Decisions",
        r"does not\s+define the Dovecot mail storage backend",
    )


if __name__ == "__main__":
    main()
