#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "core-daemon-api.md"

REQUIRED_SECTIONS = [
    "# Core Daemon API Contract",
    "## Status",
    "## Scope",
    "## Unix Domain Socket Transport",
    "## Local Access And Peer Identity",
    "## Cap'n Proto Framing Boundary",
    "## Request Identity And Correlation",
    "## Caller-Observed Success Semantics",
    "## State Authority Boundary",
    "## Deferred Operation Payloads",
    "## Out Of Scope",
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
    assert needle in sections[section], f"{section} missing text: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(
        flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL
    ), f"{section} missing pattern: {pattern}"


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains forbidden language: {pattern}"
    )


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")
    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(sections, "## Status", r"Accepted for issue 0004")

    assert_section_matches(
        sections,
        "## Scope",
        r"command payload schemas",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"does not define command payload schemas, daemon runtime internals, or command query implementation",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"Error-class semantics are defined by `docs/contracts/error-model.md`",
    )

    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"/run/wyrebox/wyrebox.sock",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"Unix domain socket only",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No TCP listener",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No HTTP endpoint",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No remote authentication model",
    )
    assert_section_matches(
        sections,
        "## Unix Domain Socket Transport",
        r"No LMTP transport",
    )

    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"Postfix helpers, Dovecot plugins, and local tools/skills",
    )
    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"peer credential",
    )
    assert_section_matches(
        sections,
        "## Local Access And Peer Identity",
        r"group-based authorization",
    )

    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"request frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"response frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"error frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"stream/chunk frame",
    )
    assert_section_matches(
        sections,
        "## Cap'n Proto Framing Boundary",
        r"categories.*not concrete field layouts",
    )

    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"request_id",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"delivery_id",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"queue identifiers",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"IMAP operation correlation",
    )
    assert_section_matches(
        sections,
        "## Request Identity And Correlation",
        r"journal_offset",
    )

    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"definitive daemon success response",
    )
    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"Ambiguous transport outcomes are not caller success",
    )
    assert_section_matches(
        sections,
        "## Caller-Observed Success Semantics",
        r"not.*silently promoted",
    )

    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"wyreboxd.*only mutable owner",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"Postfix helpers.*Dovecot plugins.*local tools and skills",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"must not open or mutate DuckDB materialized state, Wirelog state, object-store metadata, or canonical journal state directly",
    )
    assert_section_matches(
        sections,
        "## State Authority Boundary",
        r"GLib/GObject implementation style",
    )

    for operation in [
        "delivery ingestion",
        "fetch",
        "mailbox list/select",
        "flag/keyword update",
        "search",
        "fact insert/retract",
        "Wirelog predicate query",
        "safe DuckDB query templates",
    ]:
        assert_in_section(sections, "## Deferred Operation Payloads", operation)
    assert_section_matches(
        sections,
        "## Deferred Operation Payloads",
        r"no arbitrary write SQL",
    )

    for excluded in [
        "concrete Cap'n Proto message layouts",
        "daemon internals",
        "public remote API",
        "TLS",
        "HTTP",
        "TCP",
        "remote authentication",
        "LMTP",
    ]:
        assert_in_section(sections, "## Out Of Scope", excluded)

    for forbidden in [
        r"command-specific schema",
        r"full SQL support",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
