#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
SPIKE_PATH = REPO_ROOT / "docs" / "spikes" / "postfix-dovecot-verification.md"

REQUIRED_SECTIONS = [
    "# Postfix/Dovecot Verification Spike",
    "## Status",
    "## Postfix Result",
    "## Dovecot Result",
    "## Evidence Sources",
    "## Capability Verification Matrix",
    "## Acceptance Criteria Verification",
    "## Remaining Follow-Up Gaps",
    "## Start Of Issue 0002",
]

CLOSING_DOCS = [
    "docs/adr/0001-postfix-ingress-boundary.md",
    "docs/adr/0002-dovecot-plugin-boundary.md",
    "docs/contracts/mailbox-storage-model.md",
]

EVIDENCE_LINKS = [
    "https://www.postfix.org/pipe.8.html",
    "https://www.postfix.org/lmtp.8.html",
    "https://www.postfix.org/local.8.html",
    "https://doc.dovecot.org/2.3/developer_manual/design/mail_storage/",
    "https://doc.dovecot.org/2.3/developer_manual/design/plugins/",
    "https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage.h",
    "https://github.com/dovecot/core/blob/main/src/lib-storage/mail-storage-private.h",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}

    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()

    return sections


def table_rows(section: str) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}

    for line in section.splitlines():
        if not line.startswith("| ") or re.match(r"^\| [-: ]+\|", line):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and cells[0] not in {"Capability", "0001 acceptance criterion", "Gap"}:
            rows[cells[0]] = cells[1:]

    return rows


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing required text: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing required pattern: {pattern}"
    )


def assert_row_contains(rows: dict[str, list[str]], label: str, *needles: str) -> None:
    assert label in rows, f"missing table row: {label}"
    row_text = " | ".join(rows[label])
    for needle in needles:
        assert needle in row_text, f"{label} row missing: {needle}"


def main() -> None:
    assert SPIKE_PATH.is_file(), f"missing spike doc: {SPIKE_PATH}"

    text = SPIKE_PATH.read_text(encoding="utf-8")
    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(
        sections,
        "## Status",
        r"Issue 0001 is closed by the existing boundary and storage documents",
    )
    for doc in CLOSING_DOCS:
        assert_in_section(sections, "## Status", f"`{doc}`")
    assert_section_matches(sections, "## Status", r"does not add new architecture decisions")

    assert_section_matches(
        sections,
        "## Postfix Result",
        r"`pipe\(8\)` delivery helper first.*WyreBox LMTP ingress later.*Dovecot LMTP deferred",
    )
    assert_section_matches(
        sections,
        "## Postfix Result",
        r"success only after durable\s+raw object storage and durable journal append",
    )
    assert_section_matches(
        sections,
        "## Postfix Result",
        r"Duplicate delivery idempotency remains a follow-up gap for 0009",
    )

    assert_section_matches(
        sections,
        "## Dovecot Result",
        r"real WyreBox mail storage backend",
    )
    for rejected in [
        r"not Maildir\s+plus side indexes",
        r"not an FTS-only or virtual-only integration",
        r"not Dovecot\s+LMTP plus existing storage as the canonical backend",
    ]:
        assert_section_matches(sections, "## Dovecot Result", rejected)
    assert_section_matches(
        sections,
        "## Dovecot Result",
        r"WyreBox owns mailbox identity.*UIDVALIDITY.*flags.*keywords",
    )
    assert_section_matches(
        sections,
        "## Dovecot Result",
        r"FETCH resolves to immutable WyreBox object keys.*original RFC\s+5322 bytes",
    )
    assert_section_matches(
        sections,
        "## Dovecot Result",
        r"STORE flag and keyword updates.*do not rewrite raw message objects",
    )

    for link in EVIDENCE_LINKS:
        assert_in_section(sections, "## Evidence Sources", link)
    assert_section_matches(
        sections,
        "## Evidence Sources",
        r"Postfix can invoke a\s+local delivery helper and later deliver to LMTP",
    )
    assert_section_matches(
        sections,
        "## Evidence Sources",
        r"Dovecot's lib-storage\s+surface is the boundary",
    )

    capability_rows = table_rows(sections["## Capability Verification Matrix"])
    for label, required in [
        ("LIST", "WyreBox storage backend"),
        ("SELECT", "opens mailboxes"),
        ("UID/UIDVALIDITY", "WyreBox owns per-mailbox UID namespaces"),
        ("FETCH original bytes", "original RFC 5322 bytes byte-for-byte"),
        ("STORE flags/keywords", "without raw object rewrite"),
        ("SEARCH", "FTS only as a supporting layer"),
        ("Virtual views", "first-class WyreBox mailboxes"),
    ]:
        assert_row_contains(capability_rows, label, required)
    assert_row_contains(capability_rows, "UID/UIDVALIDITY", "0003", "0010", "0011")
    assert_row_contains(capability_rows, "Virtual views", "0011")

    acceptance_rows = table_rows(sections["## Acceptance Criteria Verification"])
    for criterion in [
        "Postfix ingress is documented as staged: `pipe(8)` first, LMTP later.",
        "The `pipe(8)` helper has documented success, temporary failure, and permanent failure semantics.",
        "LMTP follow-up requirements are documented without blocking the first implementation.",
        "The Dovecot path is fixed as a WyreBox mail storage backend.",
        "The storage backend path has documented support or gaps for mailbox list, fetch, flags, search, and virtual views.",
        "Virtual mailboxes are treated as WyreBox-owned mailbox views with their own UID namespace and UIDVALIDITY.",
        "The minimum mailbox model is specific enough for schema work to begin.",
        "UID and UIDVALIDITY ownership are explicitly assigned to WyreBox or Dovecot.",
        "Raw message bytes are guaranteed to remain immutable and fetchable by object key.",
        "Flag updates are confirmed to avoid rewriting raw message objects.",
        "Open questions are converted into follow-up issues, not left as vague TODOs.",
    ]:
        assert_row_contains(acceptance_rows, criterion, "Pass")

    gap_rows = table_rows(sections["## Remaining Follow-Up Gaps"])
    mapped_issues = {cell for row in gap_rows.values() for cell in row}
    for issue in ["0002", "0003", "0009", "0010", "0011", "0013"]:
        assert issue in mapped_issues, f"missing follow-up issue mapping: {issue}"
    assert_row_contains(gap_rows, "Postfix `pipe(8)` helper implementation, exit-status mapping, delivery metadata mapping, operational logs, durable success tests, and duplicate delivery idempotency.", "0009")
    assert_row_contains(gap_rows, "Dovecot ABI pin/source-contract spike, ordinary mailbox backend implementation, LIST, SELECT, UID/UIDVALIDITY, FETCH original bytes, STORE flags/keywords, SEARCH, scripted IMAP tests, and ownership documentation.", "0010")

    assert_section_matches(
        sections,
        "## Start Of Issue 0002",
        r"Issue 0002 can start after this spike because the Postfix and Dovecot boundary\s+decisions are captured",
    )


if __name__ == "__main__":
    main()
