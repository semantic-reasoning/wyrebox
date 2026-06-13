#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import re
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
PLUGIN_SOURCE = REPO_ROOT / "wyrebox" / "dovecot" / "wyrebox-dovecot-plugin.c"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def require(pattern: str, text: str, what: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        raise AssertionError(f"plugin source contract failed: missing {what}: {pattern}")


def forbid(pattern: str, text: str, what: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL) is not None:
        raise AssertionError(f"plugin source contract failed: forbidden {what}: {pattern}")


def main() -> None:
    if not PLUGIN_SOURCE.is_file():
        raise SystemExit(f"plugin source not found: {PLUGIN_SOURCE}")

    text = read_text(PLUGIN_SOURCE)

    require(r"^#include\s+\"mail-storage.h\"$", text, "mail-storage include")
    require(r"^#include\s+\"mail-storage-private.h\"$", text,
            "mail-storage-private include")
    require(
        r"^#include\s+\"module-dir.h\"$",
        text,
        "module-dir include",
    )
    require(
        r"static\s+struct\s+mail_storage\s+wyrebox_mail_storage_class\s*=\s*\{",
        text,
        "wyrebox storage class definition",
    )
    require(r"\.name\s*=\s*\"wyrebox\"", text, "storage class name")
    require(r"\.class_flags\s*=\s*0", text, "storage class flags")
    require(
        r"\.v\s*=\s*\{[\s\S]*?\.add_list\s*=\s*NULL,[\s\S]*?\.mailbox_alloc\s*=\s*NULL",
        text,
        "inert storage vfuncs",
    )
    require(
        r"wyrebox_plugin_init\s*\(\s*struct\s+module\s+\*\s*module\s*\)\s*\{[\s\S]*?mail_storage_class_register\s*\(\s*&wyrebox_mail_storage_class\s*\)",
        text,
        "init-time registration",
    )
    require(
        r"wyrebox_plugin_deinit\s*\(\s*void\s*\)\s*\{[\s\S]*?mail_storage_class_unregister\s*\(\s*&wyrebox_mail_storage_class\s*\)",
        text,
        "deinit-time unregistration",
    )
    forbid(
        r"__attribute__\s*\(\(\s*weak\s*\)\)",
        text,
        "weak symbol declarations",
    )
    forbid(
        r"mail_storage_class_register\s*!=\s*NULL",
        text,
        "registration null-guard",
    )
    forbid(
        r"mail_storage_class_unregister\s*!=\s*NULL",
        text,
        "unregistration null-guard",
    )
    forbid(
        r"^\s*void\s+mail_storage_class_register\s*\(",
        text,
        "manual mail_storage_class_register declaration",
    )
    forbid(
        r"^\s*void\s+mail_storage_class_unregister\s*\(",
        text,
        "manual mail_storage_class_unregister declaration",
    )

    print(f"Dovecot plugin source contract passed: {PLUGIN_SOURCE}")


if __name__ == "__main__":
    main()
    sys.exit(0)
