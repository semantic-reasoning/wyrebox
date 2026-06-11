#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "linux-runtime.md"

REQUIRED_SECTIONS = [
    "# Linux Runtime Contract",
    "## Status",
    "## Scope",
    "## Daemon Identity And Socket",
    "## Postfix And Dovecot Access Model",
    "## Failure Classification",
    "## Socket Unavailable And Stale Socket Behavior",
    "## Daemon Restart Behavior",
    "## Permission Mismatch Behavior",
    "## Systemd Operational Model",
    "## Filesystem Layout",
    "## State Subdirectories",
    "## Backup And Restore Units",
    "## Canonical State Ownership",
    "## Deferred Work",
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

    assert_section_matches(
        sections,
        "## Status",
        r"Accepted for issue 0002",
    )
    assert_section_matches(
        sections,
        "## Status",
        r"approves `docs/contracts/linux-runtime.md` as a public contract",
    )
    assert_section_matches(
        sections,
        "## Status",
        r"despite the general documentation guidance",
    )

    assert_section_matches(
        sections,
        "## Scope",
        r"This contract defines the Linux runtime contract only",
    )
    for excluded in [
        "systemd unit files",
        "tmpfiles.d",
        "sysusers.d",
        "package scripts",
        "install rules",
        "Postfix or Dovecot configuration files",
        "runtime directory creation code",
        "packaging dependency inventory",
    ]:
        assert_in_section(sections, "## Scope", excluded)

    assert_section_matches(
        sections,
        "## Daemon Identity And Socket",
        r"`wyreboxd` runs as user `wyrebox` and group `wyrebox`",
    )
    assert_section_matches(
        sections,
        "## Daemon Identity And Socket",
        r"default daemon socket is `/run/wyrebox/wyrebox.sock`",
    )
    assert_section_matches(
        sections,
        "## Daemon Identity And Socket",
        r"socket owner is `wyrebox`, socket group is `wyrebox`, and the expected mode is `0660`",
    )

    assert_section_matches(
        sections,
        "## Postfix And Dovecot Access Model",
        r"Postfix helpers and Dovecot plugins connect to `/run/wyrebox/wyrebox.sock` only",
    )
    assert_section_matches(
        sections,
        "## Postfix And Dovecot Access Model",
        r"must never open mutable DuckDB, Wirelog, object store metadata, or journal state directly",
    )
    assert_section_matches(
        sections,
        "## Postfix And Dovecot Access Model",
        r"runtime user or group access.*managed through Unix socket permissions",
    )

    assert_section_matches(
        sections,
        "## Failure Classification",
        r"Permission failures are classified distinctly from daemon success, daemon temporary failure, and daemon permanent failure",
    )
    assert_section_matches(
        sections,
        "## Failure Classification",
        r"Postfix delivery maps the condition to temporary delivery failure and retry",
    )
    assert_section_matches(
        sections,
        "## Failure Classification",
        r"Dovecot fetch and search map the condition to an IMAP-visible temporary backend failure",
    )

    assert_section_matches(
        sections,
        "## Socket Unavailable And Stale Socket Behavior",
        r"Postfix delivery maps socket unavailable to temporary delivery failure and retry",
    )
    assert_section_matches(
        sections,
        "## Socket Unavailable And Stale Socket Behavior",
        r"Dovecot fetch and search map socket unavailable to an IMAP-visible temporary backend failure",
    )
    assert_section_matches(
        sections,
        "## Socket Unavailable And Stale Socket Behavior",
        r"no direct state fallback",
    )
    assert_section_matches(
        sections,
        "## Socket Unavailable And Stale Socket Behavior",
        r"Only `wyreboxd` may remove or replace a stale socket path",
    )

    assert_section_matches(
        sections,
        "## Daemon Restart Behavior",
        r"`wyreboxd` completes journal replay before accepting new socket requests",
    )
    assert_section_matches(
        sections,
        "## Daemon Restart Behavior",
        r"Clients reconnect to the socket after restart",
    )
    assert_section_matches(
        sections,
        "## Daemon Restart Behavior",
        r"Postfix helper must not report delivery success unless it received a durable success response",
    )

    assert_section_matches(
        sections,
        "## Permission Mismatch Behavior",
        r"owner, group, or mode differs from `wyrebox:wyrebox` and `0660`",
    )
    assert_section_matches(
        sections,
        "## Permission Mismatch Behavior",
        r"reported as permission mismatch",
    )
    assert_section_matches(
        sections,
        "## Permission Mismatch Behavior",
        r"must not fall back to direct state access",
    )

    for systemd_contract in [
        "`wyreboxd.service`",
        "`RuntimeDirectory=wyrebox`",
        "`StateDirectory=wyrebox`",
        "`CacheDirectory=wyrebox`",
        "journald first",
        "`LogsDirectory=wyrebox`",
        "`/var/log/wyrebox/`",
        "Socket activation is deferred",
        "Non-systemd Linux support is deferred",
    ]:
        assert_in_section(sections, "## Systemd Operational Model", systemd_contract)

    for path in [
        "`/run/wyrebox/`",
        "`/var/lib/wyrebox/`",
        "`/var/cache/wyrebox/`",
        "`/etc/wyrebox/`",
        "`/var/log/wyrebox/`",
    ]:
        assert_in_section(sections, "## Filesystem Layout", path)

    for state_item in [
        "objects",
        "DuckDB materialized store",
        "canonical journal",
        "Wirelog facts and rules",
        "snapshots",
        "cache",
    ]:
        assert_in_section(sections, "## State Subdirectories", state_item)

    for backup_item in [
        "objects",
        "canonical journal",
        "DuckDB snapshot",
        "Wirelog facts and rules",
        "snapshots",
        "configuration",
    ]:
        assert_in_section(sections, "## Backup And Restore Units", backup_item)

    assert_section_matches(
        sections,
        "## Canonical State Ownership",
        r"Only `wyreboxd` mutates canonical state",
    )
    assert_section_matches(
        sections,
        "## Canonical State Ownership",
        r"DuckDB is a materialized query/index store",
    )
    assert_section_matches(
        sections,
        "## Canonical State Ownership",
        r"Postfix and Dovecot integrations must use daemon API operations",
    )

    for follow_up in [
        "systemd units",
        "package scripts",
        "packaging dependency notes",
    ]:
        assert_in_section(sections, "## Deferred Work", follow_up)

    for forbidden in [
        r"socket activation is enabled",
        r"non-systemd Linux support is supported",
        r"Postfix [^\n.]*open [^\n.]*DuckDB",
        r"Dovecot [^\n.]*open [^\n.]*journal",
        r"DuckDB is the canonical mutation authority",
        r"helpers [^\n.]*remove [^\n.]*stale socket",
    ]:
        assert_forbidden(text, forbidden)


if __name__ == "__main__":
    main()
