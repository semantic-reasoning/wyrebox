#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = REPO_ROOT / "docs" / "adr" / "0002-dovecot-plugin-boundary.md"

REQUIRED_SECTIONS = [
    "# ADR 0002: Dovecot Plugin Boundary",
    "## Status",
    "## Context",
    "## Decision",
    "## Official Dovecot Evidence",
    "## Rejected Alternatives",
    "## WyreBox-Owned Mailbox Model",
    "## Fetch And Mutation Path",
    "## Virtual Mailboxes",
    "## Dovecot ABI And Allocation Constraints",
    "## Capability Gap And Follow-Up Table",
    "## Consequences",
    "## Follow-Up Decisions",
]

OFFICIAL_LINKS = [
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


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing contract: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def assert_table_row(sections: dict[str, str], label: str, required_text: str) -> None:
    table = sections["## Capability Gap And Follow-Up Table"]
    pattern = rf"^\| {re.escape(label)} \|.*{re.escape(required_text)}.*\|$"
    assert re.search(pattern, table, re.MULTILINE), (
        f"capability table missing {label} row with: {required_text}"
    )


def main() -> None:
    assert ADR_PATH.is_file(), f"missing ADR: {ADR_PATH}"

    text = ADR_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    for link in OFFICIAL_LINKS:
        assert_in_section(sections, "## Official Dovecot Evidence", link)

    assert_section_matches(
        sections,
        "## Official Dovecot Evidence",
        r"mail_storage_vfuncs.*mailbox_vfuncs.*mail_vfuncs",
    )
    assert_section_matches(
        sections,
        "## Official Dovecot Evidence",
        r"`<plugin_name>_version`.*`DOVECOT_ABI_VERSION`",
    )
    assert_section_matches(
        sections,
        "## Official Dovecot Evidence",
        r"virtual mailbox vfuncs.*backend UIDs to virtual UIDs",
    )

    assert_in_section(
        sections,
        "## Decision",
        "WyreBox will provide a real Dovecot mail storage backend.",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"WyreBox owns mailbox identity, UID allocation, UIDVALIDITY, flags, keywords,\s+ordinary mailbox identity, and virtual mailbox identity",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"Raw RFC 5322 message bytes remain immutable WyreBox objects.*object's key.*original stored byte stream",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"STORE flag and keyword updates must not\s+rewrite raw objects",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"Ordinary and virtual mailboxes are both exposed through the storage backend",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"Virtual mailboxes are WyreBox-owned views with their own mailbox identity, UID\s+namespace, UIDNEXT, and UIDVALIDITY",
    )
    assert_section_matches(
        sections,
        "## Decision",
        r"Search and FTS are supporting layers.*not the canonical mailbox backend",
    )

    for alternative in [
        r"Maildir plus WyreBox side indexes is rejected as the canonical path",
        r"Existing Dovecot storage plus FTS or virtual-only integration is rejected as insufficient",
        r"Dovecot LMTP plus existing storage is rejected as the canonical backend",
    ]:
        assert_section_matches(sections, "## Rejected Alternatives", alternative)

    assert_section_matches(
        sections,
        "## WyreBox-Owned Mailbox Model",
        r"mailbox identity.*per-mailbox UID namespace.*UIDNEXT.*UIDVALIDITY.*flags and keywords",
    )
    assert_section_matches(
        sections,
        "## WyreBox-Owned Mailbox Model",
        r"Dovecot may keep protocol caches and indexes.*rebuildable from WyreBox state",
    )

    assert_section_matches(
        sections,
        "## Fetch And Mutation Path",
        r"FETCH.*immutable object key.*original RFC\s+5322 bytes",
    )
    assert_section_matches(
        sections,
        "## Fetch And Mutation Path",
        r"STORE flag and keyword changes.*WyreBox mutations",
    )
    assert_section_matches(
        sections,
        "## Fetch And Mutation Path",
        r"must never rewrite the raw RFC\s+5322 object",
    )

    assert_section_matches(
        sections,
        "## Virtual Mailboxes",
        r"own mailbox identity, UID\s+namespace, UIDNEXT, and UIDVALIDITY",
    )
    assert_section_matches(
        sections,
        "## Virtual Mailboxes",
        r"existing virtual plugin may be evaluated\s+only as a supporting mechanism",
    )

    assert_section_matches(
        sections,
        "## Dovecot ABI And Allocation Constraints",
        r"target Dovecot support version is not fixed.*source-contract spike.*plugin ABI must be pinned",
    )
    assert_section_matches(
        sections,
        "## Dovecot ABI And Allocation Constraints",
        r"GObject wrappers at WyreBox boundaries.*must not\s+fight Dovecot ownership rules",
    )

    for label, required_text in [
        ("LIST", "WyreBox-owned mailbox hierarchy"),
        ("SELECT", "backend mailbox open/status"),
        ("Stable UID/UIDVALIDITY", "WyreBox owns per-mailbox UID allocation"),
        ("Original byte FETCH", "streams immutable RFC 5322 bytes"),
        ("STORE flags/keywords", "must not rewrite raw objects"),
        ("Basic SEARCH", "basic Dovecot SEARCH semantics"),
        ("Virtual view exposure", "first-class WyreBox mailboxes"),
    ]:
        assert_table_row(sections, label, required_text)

    for follow_up in [
        "FTS strategy",
        "Virtual view refresh mechanics",
        "Scripted IMAP validation details",
        "Mailbox storage model contract",
    ]:
        assert_in_section(sections, "## Follow-Up Decisions", follow_up)


if __name__ == "__main__":
    main()
