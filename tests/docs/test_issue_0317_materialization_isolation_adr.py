#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = (
    REPO_ROOT / "docs" / "adr" / "0003-delivery-materialization-isolation.md"
)
CONTRACTS_DIR = REPO_ROOT / "docs" / "contracts"
ADR_REFERENCE = "docs/adr/0003-delivery-materialization-isolation.md"

REQUIRED_SECTIONS = [
    "# ADR 0003: Delivery Materialization Isolation And Retry",
    "## Status",
    "## Context",
    "## Decision",
    "## Alternatives Considered",
    "## Consequences And Known Gaps",
]

REFERENCING_CONTRACTS = [
    "core-schema.md",
    "mutation-journal.md",
    "core-daemon-api.md",
    "linux-runtime.md",
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


def main() -> None:
    assert ADR_PATH.is_file(), f"missing ADR: {ADR_PATH}"
    text = ADR_PATH.read_text(encoding="utf-8")

    missing = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing, "missing required sections: " + ", ".join(missing)

    sections = section_map(text)
    decision = "## Decision"

    assert_section_matches(sections, decision,
        r"fails with `G_IO_ERROR_INVALID_DATA` holds its account")
    assert_section_matches(sections, decision,
        r"later run of that account in the pass is skipped")
    assert_section_matches(sections, decision,
        r"Holds are not persisted")
    assert_section_matches(sections, decision,
        r"never leads an unapplied record, and never moves backwards")
    assert_section_matches(sections, decision,
        r"existing membership consumes no UID")
    assert_section_matches(sections, decision,
        r"first interval is 5 s, each retry doubles it, and it is capped at 5 minutes")
    assert_section_matches(sections, decision,
        r"held account at startup is not fatal")
    assert_section_matches(sections, decision,
        r"`G_IO_ERROR_INVALID_DATA` and `G_IO_ERROR_NOT_SUPPORTED` exit with `EX_DATAERR` \(65\)")
    assert_section_matches(sections, decision,
        r"Every other error exits with `EX_TEMPFAIL` \(75\)")
    assert_section_matches(sections, decision,
        r"no UUIDv7 dependency is added")
    assert_section_matches(sections, "## Alternatives Considered",
        r"persisted per-account hold or per-account checkpoint table was rejected")

    assert_section_matches(sections, decision,
        r"post-ingest passes never reschedule or reset a pending retry")
    assert_section_matches(sections, decision,
        r"`RestartPreventExitStatus=65`")

    service_text = (REPO_ROOT / "systemd" / "wyreboxd.service.in").read_text(
        encoding="utf-8")
    assert re.search(r"^RestartPreventExitStatus=65$", service_text,
                     re.MULTILINE), (
        "wyreboxd.service.in must not restart after permanent failures"
    )

    backup_text = (CONTRACTS_DIR / "backup-restore-workflow.md").read_text(
        encoding="utf-8")
    assert "materialization holds are not persisted" in backup_text, (
        "backup-restore-workflow.md must state that holds are not persisted"
    )

    for contract in REFERENCING_CONTRACTS:
        contract_text = (CONTRACTS_DIR / contract).read_text(encoding="utf-8")
        assert ADR_REFERENCE in contract_text, (
            f"{contract} must reference {ADR_REFERENCE}"
        )


if __name__ == "__main__":
    main()
