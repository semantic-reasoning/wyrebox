#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "backup-restore-workflow.md"

REQUIRED_SECTIONS = [
    "# Backup Restore Workflow",
    "## Status",
    "## Scope",
    "## Durable Backup Set",
    "## Restore State Machine",
    "## Recovery Decisions",
    "## Deferred Work",
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


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")
    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(sections, "## Status", r"Accepted for issue 0025")
    assert_section_matches(
        sections,
        "## Status",
        r"defines the recovery boundary for restoring WyreBox state",
    )
    assert_section_matches(
        sections,
        "## Scope",
        r"does not define any backup CLI, restore CLI, network protocol, copy tool, or runtime implementation",
    )

    for durable_item in [
        "immutable raw message objects",
        "canonical journal data up to a closed safe prefix",
        "schema metadata",
        "rule and view package identity",
        "materialized-state checkpoint metadata",
        "DuckDB materialized state as an optional acceleration layer",
    ]:
        assert_in_section(sections, "## Durable Backup Set", durable_item)

    for state_name in [
        "staged",
        "validated",
        "rebuilding materialized state",
        "serving-ready",
        "failed-retryable",
    ]:
        assert_in_section(sections, "## Restore State Machine", state_name)

    assert_section_matches(
        sections,
        "## Recovery Decisions",
        r"missing or a materialization checkpoint is missing.*prefer rebuild over data loss",
    )
    assert_section_matches(
        sections,
        "## Recovery Decisions",
        r"journal suffix is unsafe or a committed message references a missing raw object, recovery must fail",
    )
    assert_section_matches(
        sections,
        "## Deferred Work",
        r"does not define backup manifests, snapshot copy commands, or restore orchestration",
    )


if __name__ == "__main__":
    main()
