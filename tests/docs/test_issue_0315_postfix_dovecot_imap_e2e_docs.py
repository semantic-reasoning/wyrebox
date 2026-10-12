#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_CONTRACT = REPO_ROOT / "docs" / "contracts" / "dovecot-build-contract.md"
FIXTURE_BUNDLE = REPO_ROOT / "docs" / "contracts" / "dovecot-fixture-bundle.md"


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
    assert section in sections, f"missing section: {section}"
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def check_build_contract() -> None:
    sections = section_map(BUILD_CONTRACT.read_text(encoding="utf-8"))
    status = "## Status"
    inputs = "## Build Inputs"

    assert_section_matches(sections, status, r"issue #315")
    assert_section_matches(sections, inputs, r"`postfix dovecot imap e2e`")
    assert_section_matches(
        sections, inputs, r"tests/dovecot/test-postfix-dovecot-imap-e2e\.c"
    )
    assert_section_matches(sections, inputs, r"wyrebox-postfix-pipe")
    assert_section_matches(sections, inputs, r"wyrebox-postfix-lmtp-delivery")
    assert_section_matches(sections, inputs, r"pipe\(8\)")
    assert_section_matches(sections, inputs, r"lmtp\(8\)")
    assert_section_matches(sections, inputs, r"master\(8\)")
    assert_section_matches(sections, inputs, r"rootless Dovecot master")
    assert_section_matches(sections, inputs, r"imap-login")
    assert "`LOGIN`" in sections[inputs], f"{inputs} must name the `LOGIN` command"
    assert_section_matches(sections, inputs, r"byte-exact")
    assert_section_matches(sections, inputs, r"restarts `wyreboxd`")
    assert_section_matches(sections, inputs, r"UIDVALIDITY")
    assert_section_matches(sections, inputs, r"/usr/sbin/dovecot")
    assert_section_matches(sections, inputs, r"/usr/lib/dovecot/imap-login")
    assert_section_matches(sections, inputs, r"`/usr/lib/dovecot/imap`")
    assert_section_matches(sections, inputs, r"non-root")
    assert_section_matches(sections, inputs, r"WYREBOX_E2E_REQUIRE_RUNTIME=1")


def check_fixture_bundle() -> None:
    sections = section_map(FIXTURE_BUNDLE.read_text(encoding="utf-8"))
    status = "## Status"
    inputs = "## Repository Managed Inputs"
    rules = "## Recipe Discovery Rules"

    assert_section_matches(sections, status, r"issue #315")
    assert_section_matches(sections, inputs, r"`postfix dovecot imap e2e`")
    assert_section_matches(sections, inputs, r"/usr/sbin/dovecot")
    assert_section_matches(sections, inputs, r"/usr/lib/dovecot/imap-login")
    assert_section_matches(sections, inputs, r"dovecot-core")
    assert_section_matches(
        sections, inputs, r"`/usr/lib/dovecot/imap-login` from `dovecot-imapd`"
    )
    assert_section_matches(
        sections, rules, r"docs issue 0315 postfix dovecot imap e2e recipe"
    )
    assert_section_matches(sections, rules, r"WYREBOX_E2E_REQUIRE_RUNTIME=1")
    assert_section_matches(sections, rules, r"fails instead of skipping")


def main() -> None:
    assert BUILD_CONTRACT.is_file(), f"missing contract: {BUILD_CONTRACT}"
    assert FIXTURE_BUNDLE.is_file(), f"missing contract: {FIXTURE_BUNDLE}"
    check_build_contract()
    check_fixture_bundle()


if __name__ == "__main__":
    main()
