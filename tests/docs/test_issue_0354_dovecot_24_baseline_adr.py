#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0004-dovecot-2.4-build-baseline.md"

REQUIRED_SECTIONS = [
    "# ADR 0004: Dovecot 2.4 Build Baseline On Ubuntu 26.04",
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

    for heading in REQUIRED_SECTIONS:
        assert re.search(rf"^{re.escape(heading)}$", text, re.MULTILINE), (
            f"ADR missing heading: {heading}"
        )

    sections = section_map(text)

    assert_section_matches(sections, "## Status", r"issue #354")
    assert_section_matches(sections, "## Context", r"ADR 0002")
    assert_section_matches(sections, "## Context", r"2\.3\.21\.1")
    assert_section_matches(sections, "## Context", r"2\.3\.ABIv21")
    assert_section_matches(sections, "## Decision", r"Ubuntu 26\.04")
    assert_section_matches(sections, "## Decision", r"Dovecot `?2\.4\.2`?")
    assert_section_matches(sections, "## Decision", r"`2\.4\.ABIv2`")
    assert_section_matches(sections, "## Decision", r"HAVE__BOOL")
    assert_section_matches(sections, "## Decision", r"HAVE_SOCKLEN_T")
    assert_section_matches(sections, "## Decision", r"System mode")
    assert_section_matches(sections, "## Decision", r"dovecot-dev")
    assert_section_matches(
        sections, "## Decision", r"/usr/lib/dovecot/dovecot-config"
    )
    assert_section_matches(sections, "## Decision", r"Fixture mode")
    assert_section_matches(sections, "## Decision", r"GitHub-hosted")
    assert_section_matches(sections, "## Decision", r"`ubuntu-26\.04` runner")
    assert_section_matches(sections, "## Decision", r"CI job")
    assert not re.search(
        r"Containerfile|container definition|builds that image",
        sections["## Decision"],
        re.IGNORECASE,
    ), "ADR Decision must not require a project-built container image"
    assert_section_matches(
        sections, "## Consequences And Known Gaps", r"#355"
    )
    assert_section_matches(
        sections, "## Consequences And Known Gaps", r"ubuntu-24\.04"
    )


if __name__ == "__main__":
    main()
