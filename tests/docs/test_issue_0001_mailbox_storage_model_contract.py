#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "mailbox-storage-model.md"

REQUIRED_SECTIONS = [
    "# Mailbox Storage Model Contract",
    "## Status",
    "## Scope",
    "## Account Identity And Ownership Boundary",
    "## Ordinary Mailbox Identity",
    "## Virtual Mailbox Identity",
    "## Object Message And Membership Model",
    "## Immutable Object Fetch Contract",
    "## Membership Mutation Contract",
    "## Canonical State And Rebuildable Support State",
    "## Deferred Schema Details",
    "## Follow-Up Decisions",
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


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains forbidden or contradictory language: {pattern}"
    )


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"

    text = CONTRACT_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(text)

    assert_section_matches(
        sections,
        "## Scope",
        r"This contract defines the stable IMAP-visible storage model only",
    )
    for excluded in [
        "DuckDB table DDL",
        "journal event format",
        "object-store path layout",
        "Cap'n Proto API schema",
        "Dovecot vfunc or plugin scaffolding",
        "UID allocator algorithm",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    assert_section_matches(
        sections,
        "## Account Identity And Ownership Boundary",
        r"An account is the owner boundary for ordinary mailboxes, virtual mailboxes,\s+messages, immutable objects, and mailbox memberships",
    )
    assert_section_matches(
        sections,
        "## Account Identity And Ownership Boundary",
        r"WyreBox owns the account-scoped mailbox identity state exposed through Dovecot",
    )

    assert_section_matches(
        sections,
        "## Ordinary Mailbox Identity",
        r"Ordinary mailboxes have stable WyreBox mailbox identity and hierarchy names",
    )
    assert_section_matches(
        sections,
        "## Ordinary Mailbox Identity",
        r"Each ordinary mailbox has its own UID namespace, UIDNEXT, and UIDVALIDITY",
    )
    assert_section_matches(
        sections,
        "## Ordinary Mailbox Identity",
        r"WyreBox owns UIDVALIDITY",
    )

    assert_section_matches(
        sections,
        "## Virtual Mailbox Identity",
        r"Virtual mailboxes are first-class WyreBox mailbox views",
    )
    assert_section_matches(
        sections,
        "## Virtual Mailbox Identity",
        r"Each virtual mailbox has independent mailbox identity, UID namespace, UIDNEXT,\s+and UIDVALIDITY",
    )
    assert_section_matches(
        sections,
        "## Virtual Mailbox Identity",
        r"Virtual mailbox membership is derived from WyreBox or Wirelog state",
    )

    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"object, message, and mailbox membership are distinct model entities",
    )
    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"An object is the immutable stored original RFC 5322 byte stream identified by\s+an object key",
    )
    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"A message records WyreBox metadata that maps to one immutable object",
    )
    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"A mailbox membership links one message into one ordinary or virtual mailbox",
    )
    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"Mailbox membership owns the mailbox-scoped UID, flags, keywords, internal date,\s+RFC 822 size, and possibly MODSEQ",
    )
    assert_section_matches(
        sections,
        "## Object Message And Membership Model",
        r"Raw objects are shared across ordinary and virtual memberships without duplication",
    )

    assert_section_matches(
        sections,
        "## Immutable Object Fetch Contract",
        r"Raw RFC 5322 message bytes are immutable",
    )
    assert_section_matches(
        sections,
        "## Immutable Object Fetch Contract",
        r"FETCH returns the original bytes byte-for-byte by object key",
    )
    assert_section_matches(
        sections,
        "## Immutable Object Fetch Contract",
        r"flags, keywords, mailbox moves, facts, virtual views, and derived indexes must\s+not rewrite the raw object",
    )

    assert_section_matches(
        sections,
        "## Membership Mutation Contract",
        r"STORE flag and keyword changes append a WyreBox mutation and do not rewrite\s+the raw object",
    )
    assert_section_matches(
        sections,
        "## Membership Mutation Contract",
        r"membership mutation changes membership-owned state",
    )

    assert_section_matches(
        sections,
        "## Canonical State And Rebuildable Support State",
        r"`wyreboxd` is the only writer of the canonical mutation journal, DuckDB\s+materialized state, Wirelog runtime state, and WyreBox object-store metadata",
    )
    assert_section_matches(
        sections,
        "## Canonical State And Rebuildable Support State",
        r"DuckDB is not the canonical mutation authority",
    )
    assert_section_matches(
        sections,
        "## Canonical State And Rebuildable Support State",
        r"Dovecot indexes, search indexes, and caches are rebuildable support state",
    )

    for deferred in [
        "DuckDB table DDL",
        "journal event format",
        "object-store path layout",
        "Cap'n Proto API schema",
        "Dovecot vfunc/plugin scaffolding",
        "UID allocator algorithm",
    ]:
        assert_in_section(sections, "## Deferred Schema Details", deferred)

    for follow_up in [
        "Exact MODSEQ policy",
        "Special-use and subscriptions",
        "Virtual refresh mechanics",
        "Later schema tables",
    ]:
        assert_in_section(sections, "## Follow-Up Decisions", follow_up)

    for forbidden in [
        r"DuckDB is the canonical mutation authority",
        r"Dovecot [^\n.]*owns UIDVALIDITY",
        r"raw objects? [^\n.]*rewritten[^\n.]*flags",
        r"Maildir [^\n.]*canonical storage model",
        r"virtual mailboxes [^\n.]*share [^\n.]*UID namespace",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
