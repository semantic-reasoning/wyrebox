#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "error-model.md"

REQUIRED_SECTIONS = [
    "# Error Model Contract",
    "## Status",
    "## Scope",
    "## Error Classes",
    "## Transport And Access Conditions",
    "## Postfix Delivery Mapping",
    "## Dovecot Visible Mapping",
    "## Ambiguous Results",
    "## Durability Boundary",
    "## Out Of Scope",
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
    assert needle in sections[section], f"{section} missing contract text: {needle}"


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

    assert_section_matches(sections, "## Status", r"Accepted for issue 0004")

    for excluded in [
        "Cap'n Proto schema layout",
        "Request envelopes",
        "command schemas",
        "daemon runtime",
        "Postfix and Dovecot implementation internals",
        "TCP, TLS, or remote authentication behavior",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    for error_class in [
        "Temporary failure",
        "Permanent failure",
        "Permission denied",
        "Not found",
        "Conflict",
        "Internal error",
    ]:
        assert_section_matches(sections, "## Error Classes", error_class)

    assert_section_matches(
        sections,
        "## Error Classes",
        r"Permission denied.*distinct.*temporary.*permanent.*success",
    )

    for condition in [
        "Socket unavailable",
        "Stale socket",
        "Connect timeout",
        "Permission mismatch",
        "Response loss",
        "ambiguous communication loss",
    ]:
        assert_section_matches(sections, "## Transport And Access Conditions", condition)

    assert_section_matches(
        sections,
        "## Transport And Access Conditions",
        r"separate.*retry policy path",
    )

    assert_section_matches(
        sections,
        "## Postfix Delivery Mapping",
        r"Transport/access conditions.*temporary delivery failure.*retry",
    )
    assert_section_matches(
        sections,
        "## Postfix Delivery Mapping",
        r"response loss.*connection loss.*communication interruption.*before durable success.*temporary",
    )
    assert_section_matches(
        sections,
        "## Postfix Delivery Mapping",
        r"Permanent failure.*explicit.*(validation|configuration).*non-retryable",
    )
    assert_section_matches(
        sections,
        "## Postfix Delivery Mapping",
        r"delivery success requires durable raw message object storage and durable journal append",
    )

    assert_section_matches(
        sections,
        "## Dovecot Visible Mapping",
        r"LIST.*SELECT.*fetch.*search.*flag.*IMAP-visible temporary backend failure",
    )
    assert_section_matches(
        sections,
        "## Dovecot Visible Mapping",
        r"Not found and conflict.*operation-aware",
    )
    assert_section_matches(
        sections,
        "## Dovecot Visible Mapping",
        r"SELECT.*non-selectable.*Conflict.*selection-state conflict.*not success",
    )
    assert_section_matches(
        sections,
        "## Dovecot Visible Mapping",
        r"No direct state fallback",
    )

    for condition in [
        "Response loss",
        "Connection loss",
        "Request timeout",
        "Daemon restart during request",
    ]:
        assert_section_matches(sections, "## Ambiguous Results", condition)

    assert_section_matches(
        sections,
        "## Ambiguous Results",
        r"never .* success",
    )

    assert_section_matches(
        sections,
        "## Durability Boundary",
        r"delivery success.*durable raw object storage and durable journal append",
    )
    assert_section_matches(
        sections,
        "## Durability Boundary",
        r"flag and fact mutations.*durable journal append",
    )
    assert_section_matches(
        sections,
        "## Durability Boundary",
        r"does?\s+not\s+weaken.*mutation-journal",
    )

    for excluded in [
        "Unix domain socket access wiring details",
        "Full Postfix/Dovecot command behavior",
        "daemon startup/restart internals",
        "Full Cap'n Proto schema layout",
        "LMTP, TCP transport, TLS, and remote auth",
    ]:
        assert_in_section(sections, "## Out Of Scope", excluded)

    for forbidden in [
        r"(?m)^.*direct\s+state\s+fallback[^\n]*(must|should|will|allowed|permitted|enabled|implemented)",
        r"permission\s+mismatch.*as success",
        r"unrestricted\s+SQL",
        r"arbitrary\s+SQL",
        r"Cap'n\s+Proto.*field.*layout",
        r"daemon implementation behavior",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
