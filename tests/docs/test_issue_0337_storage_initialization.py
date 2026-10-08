#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
ADR_PATH = (
    REPO_ROOT / "docs" / "adr" / "0003-delivery-materialization-isolation.md"
)
CONTRACTS_DIR = REPO_ROOT / "docs" / "contracts"
RUNTIME_PATH = CONTRACTS_DIR / "linux-runtime.md"
BACKUP_PATH = CONTRACTS_DIR / "backup-restore-workflow.md"
SERVICE_PATH = REPO_ROOT / "systemd" / "wyreboxd.service.in"
STORAGE_SOURCE = REPO_ROOT / "wyrebox" / "daemon" / "wyrebox-daemon-storage.c"
MAIN_SOURCE = REPO_ROOT / "wyrebox" / "daemon" / "wyreboxd-main.c"


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


def check_adr() -> None:
    adr = section_map(ADR_PATH.read_text(encoding="utf-8"))
    decision = "## Decision"
    gaps = "## Consequences And Known Gaps"

    assert_section_matches(adr, "## Status", r"Amended by issue #337")
    assert_section_matches(adr, decision,
        r"creates nothing until both storage markers exist")
    assert_section_matches(adr, decision,
        r"Only `wyreboxd --initialize-storage` writes storage markers")
    assert_section_matches(adr, decision,
        r"both roots hold data or neither does")
    assert_section_matches(adr, decision,
        r"writes the journal marker first.*refuses a lone object store marker")
    assert_section_matches(adr, decision,
        r"storage ID is a UUIDv7 generated with libchronoid")
    assert_section_matches(adr, decision,
        r"different storage IDs.*exits? with `EX_DATAERR` \(65\)")
    assert_section_lacks(adr, decision,
        r"creates the object store root only while the journal has no records")
    assert_section_lacks(adr, gaps, r"\(#337\)")
    assert_section_matches(adr, gaps,
        r"`--initialize-storage` while a volume is not mounted.*initializes "
        r"the mount point")
    assert_section_matches(adr, gaps,
        r"Two concurrent `--initialize-storage` runs")


def check_runtime() -> None:
    runtime = section_map(RUNTIME_PATH.read_text(encoding="utf-8"))
    systemd = "## Systemd Operational Model"
    runbook = "## Delivery Materialization Troubleshooting"

    assert_section_matches(runtime, systemd,
        r"`RequiresMountsFor=/var/lib/wyrebox/journal "
        r"/var/lib/wyrebox/object-store`")
    assert_section_matches(runtime, systemd, r"systemctl edit wyreboxd")
    assert_section_matches(runtime, systemd,
        r"storage markers remain the guard")
    assert_section_matches(runtime, systemd,
        r"install -d -o wyrebox -g wyrebox -m 0750 /var/lib/wyrebox")
    assert_section_matches(runtime, systemd,
        r"runuser -u wyrebox -- wyreboxd --initialize-storage --config "
        r"/etc/wyrebox/wyrebox.conf")
    assert_section_matches(runtime, systemd, r"Never run it from `ExecStartPre=`")
    assert_section_matches(runtime, systemd, r"Upgrading")
    assert_section_matches(runtime, systemd,
        r"systemctl reset-failed wyreboxd")
    assert_section_matches(runtime, systemd,
        r"`--initialize-storage` exits with 0.*75.*65.*78.*71")
    assert_section_matches(runtime, systemd, r"\.tmp-object-")

    assert_section_matches(runtime, runbook,
        r"`EX_DATAERR` \(65\) means.*`storage markers do not match`")
    assert_section_matches(runtime, runbook,
        r"`EX_TEMPFAIL` \(75\) means.*`storage is not initialized`")
    assert_section_matches(runtime, runbook,
        r"`EX_TEMPFAIL` \(75\) means.*`storage marker <path> does not exist "
        r"but <path> does`")


def check_backup() -> None:
    text = BACKUP_PATH.read_text(encoding="utf-8")
    assert re.search(r"storage\s+markers\s+travel\s+with", text), (
        "backup-restore-workflow.md must keep storage markers with their data"
    )


def check_service() -> None:
    service = SERVICE_PATH.read_text(encoding="utf-8")
    assert re.search(
        r"^RequiresMountsFor=/var/lib/wyrebox/journal "
        r"/var/lib/wyrebox/object-store$", service, re.MULTILINE), (
        "wyreboxd.service.in must require the default storage mounts"
    )
    assert re.search(r"^RestartPreventExitStatus=65$", service, re.MULTILINE)
    assert "--initialize-storage" not in service, (
        "the service must never initialize storage on its own"
    )


def check_sources() -> None:
    storage = STORAGE_SOURCE.read_text(encoding="utf-8")
    for literal in [
        '"storage is not initialized: %s and %s do not exist; check that "',
        '"storage marker %s does not exist but %s does; check that the "',
        '"--initialize-storage again if it was interrupted"',
        '"storage markers do not match: journal %s has storage ID %s but "',
    ]:
        assert literal in storage, f"storage message drifted: {literal}"

    main_source = MAIN_SOURCE.read_text(encoding="utf-8")
    assert '{"initialize-storage", 0, 0, G_OPTION_ARG_NONE,' in main_source


def check_linking() -> None:
    consumers = set()
    for build_file in REPO_ROOT.rglob("meson.build"):
        relative = build_file.relative_to(REPO_ROOT)
        if relative.parts[0] in ("subprojects", "build") or \
                relative.parts[0].startswith(("build", "_build")):
            continue
        text = build_file.read_text(encoding="utf-8")
        if "chronoid" in text:
            consumers.add(relative.as_posix())
            assert "get_static_lib" not in text, (
                f"{relative} must not link libchronoid statically"
            )

    assert consumers == {
        "meson.build",
        "wyrebox/meson.build",
        "tests/build-config/meson.build",
        "tests/daemon-api/meson.build",
    }, f"unexpected libchronoid consumers: {sorted(consumers)}"

    daemon_tests = (REPO_ROOT / "tests" / "daemon-api" /
                    "meson.build").read_text(encoding="utf-8")
    assert daemon_tests.count("chronoid_dep") == 1, (
        "only test-daemon-storage may link libchronoid among daemon tests"
    )


def main() -> None:
    check_adr()
    check_runtime()
    check_backup()
    check_service()
    check_sources()
    check_linking()


if __name__ == "__main__":
    main()
