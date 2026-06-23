#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "dovecot-fixture-bundle.md"

REQUIRED_SECTIONS = [
    "# Dovecot Fixture Bundle Contract",
    "## Status",
    "## Scope",
    "## Repository Managed Inputs",
    "## Recipe Discovery Rules",
    "## Verification",
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

    assert_section_matches(sections, "## Status", r"Accepted for issue #298")
    assert_section_matches(
        sections,
        "## Scope",
        r"repository-managed Dovecot inputs",
    )
    assert_in_section(
        sections,
        "## Repository Managed Inputs",
        "tests/dovecot/fixtures/valid-2.3.21.1",
    )
    assert_in_section(
        sections,
        "## Repository Managed Inputs",
        "tests/dovecot/fixtures/valid-2.3.21.1/build-config-valid",
    )
    assert_in_section(
        sections,
        "## Repository Managed Inputs",
        "/usr/lib/dovecot/libdovecot-storage.so",
    )
    assert_section_matches(
        sections,
        "## Recipe Discovery Rules",
        r"does not silently skip",
    )
    assert_section_matches(
        sections,
        "## Verification",
        r"test-dovecot-source-contract\.py",
    )
    assert_section_matches(
        sections,
        "## Verification",
        r"test-dovecot-build-contract\.py",
    )


if __name__ == "__main__":
    main()
