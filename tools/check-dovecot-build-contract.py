#!/usr/bin/env python3

"""Validate a configured Dovecot build directory for WyreBox build-header checks."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import re
import sys


REQUIRED_CONFIG_MACROS = (
    "DOVECOT_ABI_VERSION",
    "HAVE__BOOL",
    "HAVE_SOCKLEN_T",
    "OFF_T_MAX",
    "PRIuUOFF_T",
    "SIZEOF_INT",
    "SIZEOF_LONG",
    "SIZEOF_VOID_P",
    "SSIZE_T_MAX",
    "UOFF_T_MAX",
)

PINNED_DOVECOT_ABI_VERSION = "2.3.ABIv21(2.3.21.1)"

UOFF_T_SELECTOR_MACROS = (
    "HAVE_UOFF_T",
    "UOFF_T_INT",
    "UOFF_T_LONG",
    "UOFF_T_LONG_LONG",
)


@dataclass
class ContractIssue:
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate that a configured Dovecot build directory has "
            "the headers and macros WyreBox's Dovecot module compile checks need"
        ),
    )
    parser.add_argument("source_dir")
    parser.add_argument("build_dir")
    return parser.parse_args()


def resolve_dir(raw_dir: str, label: str) -> Path:
    directory = Path(raw_dir)
    if not directory.exists():
        raise FileNotFoundError(f"{label} directory not found: {directory}")
    if not directory.is_dir():
        raise NotADirectoryError(f"{label} path is not a directory: {directory}")
    return directory


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def find_issues_for_required_macros(config_h: str, required_macros: tuple[str, ...]) -> list[ContractIssue]:
    issues = []
    for macro in required_macros:
        if re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\b", config_h, re.MULTILINE) is None:
            issues.append(ContractIssue(f"config.h missing required macro: {macro}"))
    return issues


def find_issues_for_uoff_t_selector(config_h: str) -> list[ContractIssue]:
    for macro in UOFF_T_SELECTOR_MACROS:
        if re.search(rf"^\s*#\s*define\s+{re.escape(macro)}\b", config_h, re.MULTILINE):
            return []

    return [
        ContractIssue(
            "config.h must define one uoff_t selector macro: " +
            ", ".join(UOFF_T_SELECTOR_MACROS),
        ),
    ]


def find_issues_for_abi_version(config_h: str) -> list[ContractIssue]:
    match = re.search(
        r'^\s*#\s*define\s+DOVECOT_ABI_VERSION\s+"([^"]+)"',
        config_h,
        re.MULTILINE,
    )
    if match is not None and match.group(1) == PINNED_DOVECOT_ABI_VERSION:
        return []

    return [
        ContractIssue(
            "config.h DOVECOT_ABI_VERSION must equal "
            f'"{PINNED_DOVECOT_ABI_VERSION}"',
        ),
    ]


def validate_build_config(source_dir: Path, build_dir: Path) -> list[ContractIssue]:
    issues: list[ContractIssue] = []
    if (source_dir / "src" / "lib" / "module-dir.h").is_file() is False:
        issues.append(ContractIssue("source directory is missing src/lib/module-dir.h"))
    if (source_dir / "src" / "lib" / "lib.h").is_file() is False:
        issues.append(ContractIssue("source directory is missing src/lib/lib.h"))

    config_h_path = build_dir / "config.h"
    if not config_h_path.is_file():
        issues.append(ContractIssue(
            f"build directory is missing required config.h: {config_h_path}",
        ))
        return issues

    config_h_text = read_text(config_h_path)
    issues.extend(find_issues_for_required_macros(config_h_text, REQUIRED_CONFIG_MACROS))
    issues.extend(find_issues_for_abi_version(config_h_text))
    issues.extend(find_issues_for_uoff_t_selector(config_h_text))
    return issues


def main() -> int:
    args = parse_args()
    try:
        source_dir = resolve_dir(args.source_dir, "source")
        build_dir = resolve_dir(args.build_dir, "build")
        issues = validate_build_config(source_dir, build_dir)
    except (OSError, ValueError) as error:
        print(f"dovecot build contract check failed: {error}", file=sys.stderr)
        return 1

    if issues:
        print("dovecot build contract check failed", file=sys.stderr)
        for issue in issues:
            print(f" - {issue.message}", file=sys.stderr)
        return 1

    print(
        f"dovecot build contract check passed: source={source_dir}, build={build_dir}",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
