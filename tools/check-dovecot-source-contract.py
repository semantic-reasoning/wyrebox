#!/usr/bin/env python3

"""Validate a Dovecot source tree against the pinned source contract."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import os
import re
import sys


PINNED_DOVECOT_VERSION = "2.3.21.1"
PINNED_DOVECOT_ABI_TEMPLATE = "2.3.ABIv21($PACKAGE_VERSION)"

REQUIRED_FILES = [
    Path("configure.ac"),
    Path("config.h.in"),
    Path("src/lib-storage/mail-storage.h"),
    Path("src/lib-storage/mail-storage-private.h"),
    Path("src/lib-storage/mail-storage-hooks.h"),
    Path("src/lib/module-dir.h"),
]

REQUIRED_TYPES = {
    "struct mail_storage": [r"\bstruct\s+mail_storage\b"],
    "struct mailbox": [r"\bstruct\s+mailbox(?!_)\b"],
    "struct mail": [r"\bstruct\s+mail(?!_)\b"],
    "struct mailbox_status uid fields": [
        "uidvalidity",
        "uidnext",
    ],
}

REQUIRED_VFUNC_SYMBOLS = {
    "mail_storage_vfuncs": [
        "add_list",
    ],
    "mailbox_vfuncs": [
        "open",
        "get_status",
        "search_init",
    ],
    "mail_vfuncs": [
        "get_stream",
        "update_flags",
        "update_keywords",
    ],
}

REQUIRED_MODULE_STRUCT_MEMBERS = {
    "module": [
        "init",
        "deinit",
    ],
    "module_dir_load_settings": [
        "abi_version",
        "require_init_funcs",
    ],
}

REQUIRED_MODULE_SYMBOLS = [
    r"\bmodule_get_symbol\b",
    r"\bmodule_get_plugin_name\b",
]

@dataclass
class ContractIssue:
    message: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Validate Dovecot source directory structure and API surface "
            "for WyreBox source-contract checks"
        ),
    )
    parser.add_argument(
        "source_dir",
        nargs="?",
        help=(
            "path to Dovecot source directory; if omitted, "
            "uses WYREBOX_DOVECOT_SOURCE_DIR"
        ),
    )
    return parser.parse_args()


def resolve_source_dir(raw_source_dir: str | None) -> Path:
    source_dir_env = os.environ.get("WYREBOX_DOVECOT_SOURCE_DIR")
    source_dir_path = raw_source_dir if raw_source_dir is not None else source_dir_env
    if not source_dir_path:
        raise ValueError(
            "dovecot source directory required; set the positional "
            "argument or WYREBOX_DOVECOT_SOURCE_DIR"
        )

    source_dir = Path(source_dir_path)

    if not source_dir.exists():
        raise FileNotFoundError(f"dovecot source directory missing: {source_dir}")

    if not source_dir.is_dir():
        raise NotADirectoryError(
            f"dovecot source path is not a directory: {source_dir}"
        )

    return source_dir


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def find_struct_block(text: str, struct_name: str) -> str | None:
    pattern = re.compile(
        rf"struct\s+{re.escape(struct_name)}\s*{{",
        re.MULTILINE,
    )
    match = pattern.search(text)
    if not match:
        return None

    start = match.end()
    depth = 1
    index = start
    while index < len(text):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index]
        index += 1

    return None


def find_issues_for_patterns(path: Path, regexes: list[str], label: str) -> list[ContractIssue]:
    text = read_text(path)
    issues: list[ContractIssue] = []
    for expression in regexes:
        if re.search(expression, text, re.MULTILINE) is None:
            issues.append(ContractIssue(f"{path}: missing {label}: {expression}"))
    return issues


def find_issues_for_struct_fields(path: Path, struct_name: str, fields: list[str]) -> list[ContractIssue]:
    text = read_text(path)
    struct_block = find_struct_block(text, struct_name)
    if struct_block is None:
        return [ContractIssue(f"{path}: missing struct {struct_name}")]

    issues: list[ContractIssue] = []
    for field in fields:
        if re.search(rf"\b{re.escape(field)}\b", struct_block) is None:
            issues.append(ContractIssue(f"{path}: struct {struct_name} missing {field}"))
    return issues


def check_config_h_template(path: Path) -> list[ContractIssue]:
    text = read_text(path)
    if re.search(r"^\s*#undef\s+DOVECOT_ABI_VERSION\b", text, re.MULTILINE):
        return []

    return [
        ContractIssue(
            f"{path}: missing DOVECOT_ABI_VERSION config.h.in template"
        )
    ]


def check_configure_abi_template(path: Path) -> list[ContractIssue]:
    text = read_text(path)
    match = re.search(
        r"AC_DEFINE_UNQUOTED\(\s*\[DOVECOT_ABI_VERSION\]\s*,\s*"
        r'"([^"]+)"',
        text,
        re.MULTILINE,
    )
    if match is not None and match.group(1) == PINNED_DOVECOT_ABI_TEMPLATE:
        return []

    return [
        ContractIssue(
            f"{path}: DOVECOT_ABI_VERSION must use template "
            f"{PINNED_DOVECOT_ABI_TEMPLATE}"
        )
    ]


def find_issues_for_vfuncs(path: Path, struct_name: str, symbols: list[str]) -> list[ContractIssue]:
    text = read_text(path)
    struct_block = find_struct_block(text, struct_name)
    if struct_block is None:
        return [ContractIssue(f"{path}: missing struct {struct_name}")]

    issues: list[ContractIssue] = []
    for symbol in symbols:
        if re.search(rf"\b{re.escape(symbol)}\b", struct_block) is None:
            issues.append(
                ContractIssue(
                    f"{path}: struct {struct_name} missing required vfunc: {symbol}"
                )
            )
    return issues


def check_version(path: Path) -> list[ContractIssue]:
    text = read_text(path)
    match = re.search(
        r"AC_INIT\(\s*\[Dovecot\]\s*,\s*\[([^\]]+)\]",
        text,
        re.MULTILINE,
    )
    if match is None:
        return [
            ContractIssue(
                f"{path}: missing AC_INIT([Dovecot],[<version>]) for pinned version"
            )
        ]

    if match.group(1) != PINNED_DOVECOT_VERSION:
        return [
            ContractIssue(
                f"{path}: unexpected Dovecot version {match.group(1)}; "
                f"expected {PINNED_DOVECOT_VERSION}"
            )
        ]

    return []


def validate_source(source_dir: Path) -> list[ContractIssue]:
    issues: list[ContractIssue] = []

    for relative_path in REQUIRED_FILES:
        if not (source_dir / relative_path).is_file():
            issues.append(
                ContractIssue(
                    f"missing required Dovecot source file: {relative_path}"
                )
            )

    if issues:
        return issues

    configure = source_dir / "configure.ac"
    config_h_in = source_dir / "config.h.in"
    storage = source_dir / "src/lib-storage/mail-storage.h"
    storage_private = source_dir / "src/lib-storage/mail-storage-private.h"
    hooks = source_dir / "src/lib-storage/mail-storage-hooks.h"
    module_dir = source_dir / "src/lib/module-dir.h"

    issues.extend(
        find_issues_for_patterns(configure, [r"\bDOVECOT_ABI_VERSION\b"], "DOVECOT_ABI_VERSION define")
    )
    issues.extend(check_version(configure))
    issues.extend(check_configure_abi_template(configure))
    issues.extend(check_config_h_template(config_h_in))

    issues.extend(
        find_issues_for_patterns(
            hooks,
            [r"\bmail_storage_hooks_add\s*\(\s*struct\s+module\s*\*"],
            "mail_storage_hooks_add(struct module *, ...)",
        )
    )

    for symbol, expressions in REQUIRED_TYPES.items():
        if symbol == "struct mailbox_status uid fields":
            issues.extend(
                find_issues_for_struct_fields(storage, "mailbox_status", expressions)
            )
            continue

        issues.extend(find_issues_for_patterns(storage, expressions, symbol))

    issues.extend(
        find_issues_for_vfuncs(storage_private, "mail_storage_vfuncs", REQUIRED_VFUNC_SYMBOLS["mail_storage_vfuncs"])
    )
    issues.extend(
        find_issues_for_vfuncs(storage_private, "mailbox_vfuncs", REQUIRED_VFUNC_SYMBOLS["mailbox_vfuncs"])
    )
    issues.extend(
        find_issues_for_vfuncs(storage_private, "mail_vfuncs", REQUIRED_VFUNC_SYMBOLS["mail_vfuncs"])
    )
    for struct_name, fields in REQUIRED_MODULE_STRUCT_MEMBERS.items():
        issues.extend(find_issues_for_struct_fields(module_dir, struct_name, fields))
    issues.extend(find_issues_for_patterns(module_dir, REQUIRED_MODULE_SYMBOLS, "module entrypoint ABI declarations"))

    return issues


def main() -> int:
    args = parse_args()
    try:
        source_dir = resolve_source_dir(args.source_dir)
    except (OSError, ValueError) as error:
        print(f"dovecot source contract check failed: {error}", file=sys.stderr)
        return 1

    issues = validate_source(source_dir)

    if issues:
        print("dovecot source contract check failed", file=sys.stderr)
        for issue in issues:
            print(f" - {issue.message}", file=sys.stderr)
        return 1

    print(f"dovecot source contract check passed: {source_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
