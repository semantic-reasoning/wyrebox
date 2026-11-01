#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
PLUGIN_SOURCE = REPO_ROOT / "wyrebox" / "dovecot" / "wyrebox-dovecot-plugin.c"


def require(pattern: str, text: str, what: str) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        raise AssertionError(f"plugin source contract failed: missing {what}: {pattern}")


def forbid(pattern: str, text: str, what: str) -> None:
    if re.search(pattern, text, re.MULTILINE | re.DOTALL) is not None:
        raise AssertionError(f"plugin source contract failed: forbidden {what}: {pattern}")


def check_state_boundary(text: str) -> None:
    for include in re.findall(r'^#include\s+"([^"]+)"', text, re.MULTILINE):
        if include.startswith("wyrebox-") and include not in {
            "wyrebox-daemon-mailbox-list-result.h",
            "wyrebox-daemon-mailbox-select-result.h",
            "wyrebox-dovecot-daemon-client.h",
        }:
            raise AssertionError(
                "plugin source contract failed: WyreBox state must be reached "
                f"through the daemon client only, found include {include}"
            )
    forbid(r"duckdb|object_store|journal_writer", text,
           "direct DuckDB, object-store, or journal access")
    require(r"wyrebox_dovecot_daemon_client_\w+\s*\(", text,
            "daemon client calls")


def check_exports(text: str) -> None:
    require(
        r"WYREBOX_DOVECOT_PLUGIN_EXPORT\s+const\s+char\s+\*wyrebox_plugin_version"
        r"\s*=\s*DOVECOT_ABI_VERSION;",
        text,
        "exported ABI version",
    )
    require(r"WYREBOX_DOVECOT_PLUGIN_EXPORT\s+void\s+wyrebox_plugin_init\s*\(",
            text, "exported init")
    require(r"WYREBOX_DOVECOT_PLUGIN_EXPORT\s+void\s+wyrebox_plugin_deinit\s*\(",
            text, "exported deinit")
    exported = re.findall(
        r"^WYREBOX_DOVECOT_PLUGIN_EXPORT\s+[^;{(=]*?(\w+)\s*[;({=]",
        text,
        re.MULTILINE,
    )
    if sorted(exported) != [
        "wyrebox_plugin_deinit",
        "wyrebox_plugin_init",
        "wyrebox_plugin_version",
    ]:
        raise AssertionError(
            f"plugin source contract failed: unexpected exports {exported}"
        )


def check_storage(text: str) -> None:
    require(r'\.name\s*=\s*"wyrebox",', text, "storage named wyrebox")
    require(r"\.class_flags\s*=\s*MAIL_STORAGE_CLASS_FLAG_NO_ROOT", text,
            "storage needs no mail_path")
    require(r"mail_storage_class_register\s*\(&wyrebox_mail_storage_class\)",
            text, "storage registration")
    require(r"mail_storage_class_unregister\s*\(&wyrebox_mail_storage_class\)",
            text, "storage unregistration")
    require(r"strcmp\s*\(ns->list->name,\s*WYREBOX_DOVECOT_LIST_NAME\)", text,
            "storage refuses other mailbox list layouts")
    require(r"p_strdup\s*\(storage->pool", text,
            "storage strings allocated from the storage pool")
    require(r"index_storage_destroy\s*\(storage\)", text,
            "index storage teardown")


def check_index_mailbox(text: str) -> None:
    require(r"index_storage_mailbox_alloc\s*\(", text,
            "index-backed mailbox allocation")
    require(r"mail_index_sync_begin\s*\(", text,
            "daemon UID map synced into the Dovecot index")
    require(r"index_mailbox_sync_init\s*\(", text, "index sync context")
    require(r"index_mail_init\s*\(", text, "index-backed mail")
    require(r"index_mail_init_stream\s*\(", text,
            "message stream through index_mail")
    require(r"\.is_readonly\s*=\s*wyrebox_dovecot_mailbox_is_readonly", text,
            "views stay read-only")
    require(r"\.expunge\s*=\s*wyrebox_dovecot_mail_expunge", text,
            "expunge recorded on the mail")
    require(r"expunges_requested", text,
            "expunge tracked on the transaction, not the index change bit")
    require(r"wyrebox_dovecot_daemon_client_expunge\s*\(", text,
            "expunge sent through the daemon client")
    require(
        r"wyrebox_dovecot_transaction_commit[\s\S]*?"
        r"wyrebox_dovecot_transaction_push_expunge\s*\([\s\S]*?"
        r"index_transaction_commit\s*\(",
        text,
        "daemon expunge runs before the index commit",
    )
    require(r"mail_index_expunge\s*\(_mail->transaction->itrans", text,
            "untagged EXPUNGE is recorded in the open transaction")
    require(r"MAIL_ERROR_PERM", text,
            "read-only expunge stays a permission error")
    require(r"\.save_begin\s*=\s*wyrebox_dovecot_save_begin,", text,
            "saving stays refused until the daemon owns writes")
    require(r"MAIL_ERROR_NOTPOSSIBLE", text, "read-only refusal")
    require(r"\.transaction_begin\s*=\s*wyrebox_dovecot_transaction_begin", text,
            "transaction tracking flag changes")
    require(r"\.transaction_commit\s*=\s*wyrebox_dovecot_transaction_commit",
            text, "flag changes pushed at transaction commit")
    require(r"mail_index_sync_next\s*\(", text, "index flag records read")
    require(r"wyrebox_dovecot_daemon_client_update_flags\s*\(", text,
            "flag changes sent through the daemon client")
    require(r"index_transaction_rollback\s*\(", text,
            "refused transactions rolled back")
    require(r"parsed_mail_attachment_detection_add_flags", text,
            "attachment $-keywords refused at storage creation")
    forbid(r"\b(malloc|calloc|realloc|strdup)\s*\(", text,
           "libc allocation outside Dovecot pools and GLib")

def check_list_driver(text: str) -> None:
    require(r'#define\s+WYREBOX_DOVECOT_LIST_NAME\s+"wyrebox"', text,
            "list driver name")
    require(r"mailbox_list_register\s*\(&wyrebox_mailbox_list_class\)", text,
            "list driver registration")
    require(r"mailbox_list_unregister\s*\(&wyrebox_mailbox_list_class\)", text,
            "list driver unregistration")
    require(
        r"\.props\s*=\s*MAILBOX_LIST_PROP_NO_ROOT\s*\|\s*"
        r"MAILBOX_LIST_PROP_NO_LIST_INDEX",
        text,
        "list driver props",
    )
    require(r"imap_match_init_multiple\s*\(", text, "Dovecot LIST pattern matcher")
    require(r"g_ascii_strcasecmp\s*\(vname,\s*\"INBOX\"\)", text,
            "INBOX normalized and listed once")
    require(r"WYREBOX_DOVECOT_HIERARCHY_SEP\s+'/'", text, "'/' separator")
    forbid(r"\.add_list\s*=|list->v\.iter_init\s*=|GHashTable\s*\*\w*hook",
           text, "mailbox list iterator hook")
    forbid(r"pattern_match_here", text, "hand-written LIST glob matcher")


def check_socket(text: str) -> None:
    require(r'#define\s+WYREBOX_DOVECOT_SOCKET_ENV\s+"WYREBOX_SOCKET"', text,
            "socket environment variable")
    require(
        r'#define\s+WYREBOX_DOVECOT_DEFAULT_SOCKET\s+"/run/wyrebox/wyrebox\.sock"',
        text,
        "default socket path",
    )
    require(r"g_getenv\s*\(WYREBOX_DOVECOT_SOCKET_ENV\)", text,
            "socket path read from the environment")
    forbid(r"__attribute__\s*\(\(\s*weak\s*\)\)|wyrebox_dovecot_test_\w+", text,
           "test-only weak symbol seam")


def main() -> None:
    if not PLUGIN_SOURCE.is_file():
        raise SystemExit(f"plugin source not found: {PLUGIN_SOURCE}")

    text = PLUGIN_SOURCE.read_text(encoding="utf-8", errors="replace")
    check_state_boundary(text)
    check_exports(text)
    check_storage(text)
    check_index_mailbox(text)
    check_list_driver(text)
    check_socket(text)
    print(f"Dovecot plugin source contract passed: {PLUGIN_SOURCE}")


if __name__ == "__main__":
    main()
