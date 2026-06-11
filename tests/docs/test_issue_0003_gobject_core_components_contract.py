#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "gobject-core-components.md"

REQUIRED_SECTIONS = [
    "# GObject Core Components Contract",
    "## Status",
    "## Scope",
    "## Source And Header Layout",
    "## Formatting And Style",
    "## GObject Ownership Conventions",
    "## Local Object Store Adapter Boundary",
    "## Mutation Journal Writer Boundary",
    "## Journal Reader And Replay Worker Boundary",
    "## DuckDB Metadata Materialized Store Boundary",
    "## Migration Runner Boundary",
    "## Snapshot Manager Boundary",
    "## Single-Writer And Authority Boundaries",
    "## Recovery And Rebuild Boundary",
    "## TDD And Validation",
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

    assert_section_matches(sections, "## Status", r"Accepted for issue 0003")

    for excluded in [
        "C implementation",
        "SQL schema DDL",
        "runtime daemon API protocol",
        "Postfix integration behavior",
        "Dovecot integration behavior",
        "wirelog runtime internals",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    assert_section_matches(
        sections,
        "## Source And Header Layout",
        r"first-party component sources and headers.*`wyrebox/`",
    )
    assert_section_matches(
        sections,
        "## Source And Header Layout",
        r"No top-level `include/` directory is introduced",
    )

    assert_section_matches(
        sections,
        "## Formatting And Style",
        r"C and GLib/GObject",
    )
    assert_section_matches(
        sections,
        "## Formatting And Style",
        r"tools/gst-indent",
    )
    assert_section_matches(
        sections,
        "## Formatting And Style",
        r"g_autoptr.*g_autofree.*g_auto",
    )

    assert_section_matches(
        sections,
        "## GObject Ownership Conventions",
        r"public constructors.*full ownership",
    )
    assert_section_matches(
        sections,
        "## GObject Ownership Conventions",
        r"input.*non-owning read semantics",
    )
    assert_section_matches(
        sections,
        "## GObject Ownership Conventions",
        r"out parameters.*document",
    )
    assert_section_matches(
        sections,
        "## GObject Ownership Conventions",
        r"`GError`.*failure reporting",
    )
    assert_section_matches(
        sections,
        "## GObject Ownership Conventions",
        r"finalizers.*release all resources owned by the instance",
    )

    assert_section_matches(
        sections,
        "## Local Object Store Adapter Boundary",
        r"owns immutable raw RFC 5322 objects",
    )
    assert_section_matches(
        sections,
        "## Local Object Store Adapter Boundary",
        r"does not own mailbox membership|does not own mailbox flags",
    )

    assert_section_matches(
        sections,
        "## Mutation Journal Writer Boundary",
        r"canonical write path.*mutation",
    )
    assert_section_matches(
        sections,
        "## Mutation Journal Writer Boundary",
        r"canonical journal stream",
    )
    assert_section_matches(
        sections,
        "## Mutation Journal Writer Boundary",
        r"wyreboxd.*only .* append",
    )

    assert_section_matches(
        sections,
        "## Journal Reader And Replay Worker Boundary",
        r"validates.*journal records",
    )
    assert_section_matches(
        sections,
        "## Journal Reader And Replay Worker Boundary",
        r"replays only valid records in order",
    )
    assert_section_matches(
        sections,
        "## Journal Reader And Replay Worker Boundary",
        r"feeds downstream materializers",
    )
    assert_section_matches(
        sections,
        "## Journal Reader And Replay Worker Boundary",
        r"must not mutate raw object bytes",
    )

    assert_section_matches(
        sections,
        "## DuckDB Metadata Materialized Store Boundary",
        r"materialized query and index state",
    )
    assert_section_matches(
        sections,
        "## DuckDB Metadata Materialized Store Boundary",
        r"not.*canonical mutation authority",
    )

    assert_section_matches(
        sections,
        "## Migration Runner Boundary",
        r"explicit.*schema versions",
        )
    assert_section_matches(
        sections,
        "## Migration Runner Boundary",
        r"unknown schema versions.*rejected",
    )
    assert_section_matches(
        sections,
        "## Migration Runner Boundary",
        r"No implicit unknown-version upgrade",
    )

    assert_section_matches(
        sections,
        "## Snapshot Manager Boundary",
        r"captures.*materialized state",
    )
    assert_section_matches(
        sections,
        "## Snapshot Manager Boundary",
        r"restores materialized state",
    )
    assert_section_matches(
        sections,
        "## Snapshot Manager Boundary",
        r"must not rewrite immutable raw RFC 5322 object bytes",
    )

    assert_section_matches(
        sections,
        "## Single-Writer And Authority Boundaries",
        r"wyreboxd.*owns.*canonical journal append",
    )
    assert_section_matches(
        sections,
        "## Single-Writer And Authority Boundaries",
        r"Postfix and Dovecot helpers/plugins must not write",
    )
    assert_section_matches(
        sections,
        "## Single-Writer And Authority Boundaries",
        r"object-store metadata",
    )

    assert_section_matches(
        sections,
        "## Recovery And Rebuild Boundary",
        r"rebuildable from.*journal",
    )
    assert_section_matches(
        sections,
        "## Recovery And Rebuild Boundary",
        r"Postfix and Dovecot integrations.*daemon",
    )

    assert_in_section(
        sections,
        "## TDD And Validation",
        "tests/docs/test_issue_0003_gobject_core_components_contract.py",
    )
    assert_in_section(
        sections,
        "## TDD And Validation",
        "docs issue 0003 gobject core components contract",
    )

    for forbidden in [
        r"Dovecot integration behavior.*implemented",
        r"Postfix integration behavior.*implemented",
        r"daemon API protocol design.*implemented",
        r"directly mutate .*canonical.*raw object bytes",
        r"DuckDB.*is the canonical mutation authority",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
