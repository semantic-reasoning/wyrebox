#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = (
    REPO_ROOT / "docs" / "adr" / "0003-delivery-materialization-isolation.md"
)
RUNTIME_PATH = REPO_ROOT / "docs" / "contracts" / "linux-runtime.md"


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def assert_section_matches(sections: dict[str, str], section: str,
                           pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section],
                     re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def assert_section_lacks(sections: dict[str, str], section: str,
                         pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert not re.search(flexible_pattern, sections[section],
                         re.IGNORECASE | re.DOTALL), (
        f"{section} still contains: {pattern}"
    )


def main() -> None:
    adr = section_map(ADR_PATH.read_text(encoding="utf-8"))
    runtime = section_map(RUNTIME_PATH.read_text(encoding="utf-8"))
    decision = "## Decision"
    gaps = "## Consequences And Known Gaps"
    runbook = "## Delivery Materialization Troubleshooting"

    assert_section_matches(adr, "## Status", r"Amended by issue #331")

    assert_section_matches(adr, decision,
        r"checkpoint precondition is not met fails with "
        r"`G_IO_ERROR_NOT_SUPPORTED` and exits with 65")
    assert_section_lacks(adr, gaps,
        r"migration precondition failures use `G_IO_ERROR_FAILED`")
    assert_section_matches(runtime, runbook,
        r"`EX_DATAERR` \(65\) means a permanent problem:.*"
        r"catalog migration that needs an offline checkpoint")
    assert_section_matches(runtime, runbook,
        r"`checkpoint precondition not satisfied` or an invalid catalog file, "
        r"restore the catalog from backup or rebuild it")

    assert_section_matches(adr, decision,
        r"lock held by another process is `G_IO_ERROR_BUSY`")
    assert_section_matches(adr, decision,
        r"not a valid or intact DuckDB database is `G_IO_ERROR_INVALID_DATA`")
    assert_section_matches(adr, decision,
        r"unsupported DuckDB storage version is `G_IO_ERROR_NOT_SUPPORTED`")
    assert_section_lacks(adr, gaps,
        r"DuckDB open failures are not split")
    assert_section_matches(adr, gaps,
        r"DuckDB open failures are classified by the text of DuckDB's error "
        r"message")
    assert_section_matches(runtime, runbook,
        r"catalog file that is not a valid or compatible DuckDB database")

    shared_source = (REPO_ROOT / "wyrebox" / "duckdb" /
                     "wyrebox-duckdb-shared.c").read_text(encoding="utf-8")
    for literal in [
        '"Could not set lock on file"',
        '"is not a valid DuckDB database file"',
        '"Corrupt database file"',
        '"Trying to read a database file with version number"',
    ]:
        assert literal in shared_source, (
            f"DuckDB open classification drifted: {literal}"
        )


if __name__ == "__main__":
    main()
