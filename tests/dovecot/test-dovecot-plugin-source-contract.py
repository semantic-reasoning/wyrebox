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


def function_body(name: str, text: str) -> str:
    header = rf"\b{name}\s*\(\s*void\s*\)\s*\{{"
    match = re.search(header, text, re.MULTILINE)
    if match is None:
        raise AssertionError(
            f"plugin source contract failed: missing function: {name}"
        )

    depth = 1
    i = match.end()

    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[match.end():i]
        i += 1

    raise AssertionError(
        f"plugin source contract failed: unbalanced braces in {name}"
    )


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
        r"struct\s+wyrebox_dovecot_storage\s*\{\s*[\s\S]*?struct\s+mail_storage\s+storage;\s*}\s*;",
        text,
        "wyrebox storage wrapper struct",
    )
    require(
        r"struct\s+wyrebox_dovecot_mailbox\s*\{\s*[\s\S]*?"
        r"struct\s+mailbox\s+mailbox;\s*}\s*;",
        text,
        "wyrebox mailbox wrapper struct",
    )
    require(
        r"wyrebox_dovecot_storage_alloc\s*\(\s*void\s*\)\s*\{[\s\S]*?"
        r"pool_alloconly_create\s*\(\s*\"wyrebox storage\"[\s\S]*?\)",
        text,
        "allocator uses pool_alloconly_create",
    )
    require(
        r"storage->storage\s*=\s*wyrebox_mail_storage_class;\s*[\s\S]*?"
        r"storage->storage\.pool\s*=\s*pool;",
        text,
        "allocator copies storage class template before pool assignment",
    )
    require(
        r"return\s+&storage->storage;",
        text,
        "allocator returns wrapped storage",
    )
    require(
        r"static\s+void\s+wyrebox_dovecot_mailbox_free"
        r"\s*\(\s*struct\s+mailbox\s+\*box\s*\)",
        text,
        "mailbox free wrapper",
    )
    require(
        r"wyrebox_dovecot_mailbox_open\s*\(\s*struct\s+mailbox\s+\*box\s*\)",
        text,
        "mailbox open wrapper",
    )
    require(
        r"wyrebox_dovecot_mailbox_get_status\s*\(\s*struct\s+mailbox\s+\*box\s*,"
        r"[\s\S]*?struct\s+mailbox_status\s+\*status_r\)",
        text,
        "mailbox get_status wrapper",
    )
    require(
        r"wyrebox_dovecot_mailbox_alloc\s*\(\s*struct\s+mail_storage\s+\*storage\s*,"
        r"[\s\S]*?struct\s+mailbox_list\s+\*list\s*,[\s\S]*?const\s+char\s+\*vname\s*,"
        r"[\s\S]*?enum\s+mailbox_flags\s+flags\)",
        text,
        "mailbox allocator signature",
    )
    require(
        r"pool_alloconly_create\s*\(\s*\"wyrebox mailbox\"[\s\S]*?",
        text,
        "mailbox allocator uses pool_alloconly_create",
    )
    require(
        r"p_new\s*\(\s*pool,\s*struct\s+wyrebox_dovecot_mailbox,\s*1\)",
        text,
        "mailbox allocator uses p_new",
    )
    require(
        r"->mailbox\.pool\s*=\s*pool;\s*[\s\S]*?"
        r"->mailbox\.storage\s*=\s*storage;\s*[\s\S]*?"
        r"->mailbox\.list\s*=\s*list;\s*[\s\S]*?"
        r"->mailbox\.vname\s*=\s*vname;\s*[\s\S]*?"
        r"->mailbox\.name\s*=\s*vname;\s*[\s\S]*?"
        r"->mailbox\.event\s*=\s*NULL;\s*[\s\S]*?"
        r"->mailbox\.mail_vfuncs\s*=\s*NULL;\s*[\s\S]*?"
        r"->mailbox\.vlast\s*=\s*NULL;",
        text,
        "mailbox allocator sets required base fields",
    )
    require(
        r"->mailbox\.v\.open\s*=\s*wyrebox_dovecot_mailbox_open;",
        text,
        "mailbox open vfunc wired",
    )
    require(
        r"->mailbox\.v\.free\s*=\s*wyrebox_dovecot_mailbox_free;",
        text,
        "mailbox free vfunc wired",
    )
    require(
        r"->mailbox\.v\.get_status\s*=\s*wyrebox_dovecot_mailbox_get_status;",
        text,
        "mailbox get_status vfunc wired",
    )
    require(
        r"return\s+&\w+->mailbox;",
        text,
        "mailbox allocator returns mailbox",
    )
    require(
        r"status_r->messages\s*=\s*0;\s*[\s\S]*?"
        r"status_r->uidvalidity\s*=\s*1;\s*[\s\S]*?"
        r"status_r->uidnext\s*=\s*1;",
        text,
        "mailbox get_status sets inert defaults",
    )
    require(
        r"static\s+struct\s+mail_storage\s+wyrebox_mail_storage_class\s*=\s*\{",
        text,
        "wyrebox storage class definition",
    )
    require(r"\.name\s*=\s*\"wyrebox\"", text, "storage class name")
    require(r"\.class_flags\s*=\s*0", text, "storage class flags")
    require(
        r"\.v\s*=\s*\{[\s\S]*?\.add_list\s*=\s*NULL,[\s\S]*?"
        r"\.mailbox_alloc\s*=\s*wyrebox_dovecot_mailbox_alloc",
        text,
        "inert storage vfuncs",
    )
    require(
        r"\.v\s*=\s*\{[\s\S]*?\.alloc\s*=\s*wyrebox_dovecot_storage_alloc,[\s\S]*?"
        r"\.create\s*=\s*wyrebox_dovecot_storage_create,[\s\S]*?"
        r"\.destroy\s*=\s*wyrebox_dovecot_storage_destroy",
        text,
        "wired storage lifecycle vfuncs",
    )
    require(
        r"static\s+void\s+wyrebox_dovecot_storage_destroy\s*\(\s*struct\s+mail_storage\s*\*storage\s*\)\s*\{\s*\(void\)\s*storage;\s*\}",
        text,
        "destroy is no-op",
    )
    if re.search(
        r"return\s+NULL;\s*",
        function_body("wyrebox_dovecot_storage_alloc", text),
    ) is not None:
        raise AssertionError(
            "plugin source contract failed: forbidden storage allocator "
            "null fallback"
        )
    forbid(
        r"pool_unref\s*\(\s*&storage->pool\s*\)",
        text,
        "storage destroy releases pool",
    )
    forbid(
        r"pool_unref\s*\(\s*&box->pool\s*\)",
        text,
        "mailbox free directly releases pool",
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
