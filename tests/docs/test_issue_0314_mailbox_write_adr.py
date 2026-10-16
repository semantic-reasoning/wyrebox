#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0006-mailbox-write-operations.md"

REQUIRED_SECTIONS = [
    "# ADR 0006: Mailbox Write Operations",
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

    assert_section_matches(sections, "## Status", r"issue #314")
    assert_section_matches(sections, "## Context", r"APPEND|COPY|MOVE|EXPUNGE")
    assert_section_matches(sections, "## Context", r"ADR 0003|tombstone")

    decision = "## Decision"
    assert_section_matches(sections, decision, r"tombstone")
    assert_section_matches(sections, decision, r"`is_visible`")
    assert_section_matches(sections, decision, r"`MembershipsExpunged`")
    assert_section_matches(sections, decision, r"`MembershipsCopied`")
    assert_section_matches(sections, decision, r"`MembershipsMoved`")
    assert_section_matches(sections, decision, r"`MessageAppended`")
    assert_section_matches(sections, decision, r"mutation lock|route lock")
    assert_section_matches(sections, decision, r"journal_item")
    assert_section_matches(sections, decision, r"same durability as delivery")
    assert_section_matches(sections, decision, r"no APPEND idempotency")
    assert_section_matches(sections, decision, r"4 MiB|frame limit")
    assert_section_matches(sections, decision, r"MULTIAPPEND.*not atomic")
    assert_section_matches(sections, decision, r"visible ordinary membership")
    assert_section_matches(sections, decision, r"COPY may use a virtual mailbox as a source")
    assert_section_matches(sections, decision, r"same-mailbox")
    assert_section_matches(sections, decision, r"expunges_requested|per-transaction")
    assert_section_matches(sections, decision, r"`.expunge` skips those UIDs")
    assert_section_matches(sections, decision, r"Only the daemon MOVE record")
    assert_section_matches(sections, decision, r"Free the save context")
    assert_section_matches(sections, decision, r"failed\s+`commit_pre`")
    assert_section_matches(sections, decision, r"saved_uids|APPENDUID|COPYUID")
    assert_section_matches(sections, decision, r"sync_append_new|do not mail_index_append")

    assert_section_matches(
        sections, "## Alternatives Considered", r"hard.?delete|membership_removals"
    )
    assert_section_matches(
        sections, "## Alternatives Considered", r"chunked upload|MailboxDeclared"
    )
    assert_section_matches(
        sections, "## Consequences And Known Gaps", r"Trash|Sent|Archive|follow-up"
    )
    assert_section_matches(
        sections, "## Consequences And Known Gaps", r"PR4|view refresh|ordinary membership"
    )


if __name__ == "__main__":
    main()
