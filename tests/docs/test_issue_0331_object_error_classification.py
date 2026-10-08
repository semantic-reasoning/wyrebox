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

    assert_section_matches(adr, decision,
        r"missing, truncated, or tampered object holds the account")
    assert_section_matches(adr, decision,
        r"one bad object holds every account that references it")
    assert_section_lacks(adr, decision,
        r"projection replay \(including a missing or corrupt raw object\)")
    assert_section_lacks(adr, gaps,
        r"raw object for one delivery still stops every pass")
    assert_section_matches(runtime, runbook,
        r"raw object that is missing, unreadable, or does not match the "
        r"journaled size or SHA-256 key")

    assert_section_matches(adr, decision,
        r"missing root exits with `EX_TEMPFAIL` \(75\) and creates nothing")
    assert_section_matches(adr, decision,
        r"A missing root therefore stops every account and retries")
    assert_section_lacks(adr, gaps, r"unmounted or unreadable as a whole")
    assert_section_matches(runtime, runbook,
        r"`EX_TEMPFAIL` \(75\) means.*object store that is not mounted")

    assert_section_matches(adr, decision,
        r"does not stop startup for a journaled delivery whose raw object is "
        r"missing or does not match")
    assert_section_matches(adr, decision,
        r"cannot be read, for example because of a permission or I/O error, "
        r"exits with `EX_TEMPFAIL` \(75\) at startup")
    assert_section_lacks(adr, gaps,
        r"Delivery storage validation at startup still rejects")
    assert_section_lacks(adr, gaps, r"still exits with `EX_DATAERR` for it")
    assert_section_lacks(adr, gaps,
        r"keeps exiting with `EX_DATAERR` for every error")
    assert_section_matches(runtime, runbook,
        r"`journaled deliveries with missing or corrupt raw objects: <n>, "
        r"first at journal sequence <sequence>")
    assert_section_matches(runtime, runbook,
        r"`EX_TEMPFAIL` \(75\) means.*`failed to read raw object`")
    assert_section_lacks(runtime, runbook,
        r"transient object read errors exiting with 65")

    assert_section_lacks(adr, gaps, r"tracked in #331")
    assert_section_matches(adr, gaps,
        r"already materialized and later lost its raw object is reported only "
        r"by the startup warning")
    assert_section_matches(adr, gaps,
        r"permanently lost keeps every account that references it held until "
        r"the object is restored.*\(#338\)")
    assert_section_matches(adr, gaps,
        r"cannot be read stops startup with `EX_TEMPFAIL`.*\(#339\)")
    assert_section_lacks(adr, gaps, r"same unmounted volume")
    assert_section_matches(runtime, runbook,
        r"materialized before the checkpoint are not held and are reported "
        r"only by this warning")
    assert_section_matches(runtime, runbook,
        r"For `failed to read raw object`, fix the permissions or the disk")

    runtime_source = (REPO_ROOT / "wyrebox" / "daemon" /
                      "wyreboxd-main.c").read_text(encoding="utf-8")
    for literal in [
        '"journaled deliveries with missing or corrupt raw "',
        '", first at journal sequence %"',
    ]:
        assert literal in runtime_source, (
            f"startup object warning drifted: {literal}"
        )

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
