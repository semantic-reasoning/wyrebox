#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = (
    REPO_ROOT / "docs" / "adr" /
    "0007-daemon-socket-caller-account-binding.md")

REQUIRED_SECTIONS = [
    "# ADR 0007: Daemon Socket Caller Account Binding",
    "## Status",
    "## Context",
    "## Decision",
    "## Alternatives Considered",
    "## Consequences And Known Gaps",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = (matches[index + 1].start()
               if index + 1 < len(matches) else len(text))
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_section_matches(sections: dict[str, str], section: str,
                           pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section],
                     re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def main() -> None:
    assert ADR_PATH.is_file(), f"missing ADR: {ADR_PATH}"
    text = ADR_PATH.read_text(encoding="utf-8")

    for heading in REQUIRED_SECTIONS:
        assert re.search(rf"^{re.escape(heading)}$", text, re.MULTILINE), (
            f"ADR missing heading: {heading}"
        )

    sections = section_map(text)

    assert_section_matches(sections, "## Status", r"issue #364")
    assert_section_matches(sections, "## Context", r"SO_PEERCRED|peer credential")
    assert_section_matches(sections, "## Context", r"shared.?uid|vmail")
    assert_section_matches(sections, "## Context", r"#318")

    decision = "## Decision"
    assert_section_matches(sections, decision, r"peer-account-binding")
    assert_section_matches(sections, decision, r"accounts=\*")
    assert_section_matches(sections, decision, r"Fail closed|fail closed")
    assert_section_matches(sections, decision,
                           r"at least one .*peer-account-binding")
    assert_section_matches(sections, decision,
                           r"process trust|multi-tenant")
    assert_section_matches(sections, decision, r"Exclude delivery ingestion")
    assert_section_matches(sections, decision,
                           r"LIST|SELECT|FETCH|SEARCH|DuckDB")

    assert_section_matches(
        sections, "## Alternatives Considered", r"Per-account Unix sockets")
    assert_section_matches(
        sections, "## Alternatives Considered", r"Login token")
    assert_section_matches(
        sections, "## Alternatives Considered", r"Fail-open")

    assert_section_matches(
        sections, "## Consequences And Known Gaps", r"login-token|#318")


if __name__ == "__main__":
    main()
