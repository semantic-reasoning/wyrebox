#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0005-dovecot-index-backed-storage.md"

REQUIRED_SECTIONS = [
    "# ADR 0005: Dovecot Index-Backed Storage Runtime",
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

    assert_section_matches(sections, "## Status", r"issue #355")
    assert_section_matches(sections, "## Status", r"Follow-up changes")
    assert_section_matches(sections, "## Context", r"ADR 0004")
    assert_section_matches(sections, "## Context", r"lib-index")

    decision = "## Decision"
    assert_section_matches(sections, decision, r"MAIL_STORAGE_CLASS_FLAG_NO_ROOT")
    assert_section_matches(sections, decision, r"index_storage_mailbox_alloc")
    assert_section_matches(sections, decision, r"index_storage_mailbox_open")
    assert_section_matches(sections, decision, r"index_mail_init_stream")
    assert_section_matches(sections, decision, r"in-memory")
    assert_section_matches(
        sections, decision, r"UIDVALIDITY change inside one session fails the sync"
    )
    assert_section_matches(sections, decision, r"is_inconsistent")
    assert_section_matches(sections, decision, r"only after the index commit")
    assert_section_matches(sections, decision, r"\[NONEXISTENT\]")
    assert_section_matches(sections, decision, r"\[UNAVAILABLE\]")
    assert_section_matches(sections, decision, r"WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR")
    assert_section_matches(sections, decision, r"no namespace prefix")
    assert_section_matches(sections, decision, r"STATUS")
    assert_section_matches(sections, decision, r"mailbox_list_register")
    assert_section_matches(sections, decision, r"mailbox_list_layout = wyrebox")
    assert_section_matches(sections, decision, r"`/` as the separator")
    assert_section_matches(sections, decision, r"refuses to start with any other")
    assert_section_matches(sections, decision, r"Garbage after '\{'")
    assert_section_matches(sections, decision, r"mail_driver = wyrebox")
    assert_section_matches(sections, decision, r"WYREBOX_SOCKET")
    assert_section_matches(sections, decision, r"import_environment")
    assert_section_matches(sections, decision, r"/run/wyrebox/wyrebox\.sock")
    assert_section_matches(sections, decision, r"read-only")
    assert_section_matches(sections, decision, r"daemon client")
    assert_section_matches(sections, decision, r"Fixture mode")
    assert_section_matches(sections, decision, r"supersedes ADR 0004")
    assert_section_matches(sections, decision, r"smoke tests are retired")
    assert_section_matches(sections, decision, r"non-root")
    assert_section_matches(sections, decision, r"missing-mailbox and daemon-down")
    assert_section_matches(sections, decision, r"reports a skip")
    assert re.search(
        r"^\s*mail_plugins \{\n\s*wyrebox = yes\n\s*\}$",
        sections[decision],
        re.MULTILINE,
    ), "ADR must show multi-line Dovecot 2.4 config blocks"
    assert re.search(
        r"^\s*import_environment \{\n\s*WYREBOX_SOCKET = \S+\n\s*\}$",
        sections[decision],
        re.MULTILINE,
    ), "ADR must show the multi-line socket import_environment block"

    assert_section_matches(sections, decision, r"\[READ-WRITE\]")
    assert_section_matches(sections, decision, r"FlagKeywordUpdateRequest")
    assert_section_matches(sections, decision, r"transaction_commit")
    assert_section_matches(sections, decision, r"\[CANNOT\]")
    assert_section_matches(sections, decision, r"Expunge ignored")
    assert_section_matches(sections, decision, r"add-flags")
    assert re.search(
        r"^\s*mail_attachment_detection_options =\s*$",
        sections[decision],
        re.MULTILINE,
    ), "ADR config example must disable attachment keywords"

    gaps = "## Consequences And Known Gaps"
    assert not re.search(r"until #313", sections[gaps]), (
        "Known Gaps must not claim flag changes wait for #313"
    )
    assert_section_matches(sections, gaps, r"#374")
    assert_section_matches(sections, gaps, r"PERMANENTFLAGS")
    assert_section_matches(sections, gaps, r"round trip")
    assert_section_matches(sections, gaps, r"#375")
    assert_section_matches(sections, gaps, r"STORE on a view is untested")
    assert_section_matches(sections, gaps, r"CONDSTORE")
    assert_section_matches(sections, gaps, r"autoexpunge")
    assert_section_matches(sections, gaps, r"#314")
    assert_section_matches(sections, gaps, r"blocking")
    assert_section_matches(sections, gaps, r"compile coverage")
    assert not re.search(r"unseen", sections[gaps], re.IGNORECASE), (
        "Known Gaps must not claim every message appears unseen"
    )
    assert_section_matches(sections, decision, r"daemon flags and keywords")
    assert_section_matches(sections, gaps, r"INTERNALDATE")
    assert_section_matches(sections, gaps, r"LSUB")
    assert_section_matches(sections, gaps, r"timeout")
    assert_section_matches(sections, gaps, r"account")
    assert_section_matches(sections, gaps, r"interactive")


if __name__ == "__main__":
    main()
