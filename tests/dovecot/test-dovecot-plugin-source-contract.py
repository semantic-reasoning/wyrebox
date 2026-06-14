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
    header = rf"\b{name}\s*\([^)]*\)\s*\{{"
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

    forbid(
        r"\bmailbox_list_sink_\w+\b",
        text,
        "fixture mailbox LIST sink dependency in production plugin",
    )
    require(r"^#include\s+\"mail-storage.h\"$", text, "mail-storage include")
    require(r"^#include\s+\"mail-storage-private.h\"$", text,
            "mail-storage-private include")
    require(r"^#include\s+\"mail-namespace.h\"$", text, "mail-namespace include")
    require(r"^#include\s+\"mail-user.h\"$", text, "mail-user include")
    require(r"^#include\s+\"lib.h\"$", text, "lib include")
    require(
        r"^#include\s+\"module-dir.h\"$",
        text,
        "module-dir include",
    )
    require(
        r"^#include\s+\"wyrebox-daemon-mailbox-select-result.h\"$",
        text,
        "daemon mailbox SELECT result include",
    )
    require(
        r"^#include\s+\"wyrebox-daemon-mailbox-list-result.h\"$",
        text,
        "daemon mailbox LIST result include",
    )
    require(
        r"^#include\s+\"wyrebox-dovecot-daemon-client.h\"$",
        text,
        "Dovecot daemon client include",
    )
    require(
        r"struct\s+istream\s+\*i_stream_create_copy_from_data\s*"
        r"\(\s*const\s+void\s+\*data,\s*size_t\s+size\s*\);",
        text,
        "owned-copy istream constructor declaration",
    )
    require(
        r"wyrebox_dovecot_mail_get_stream\s*\(\s*struct\s+mail\s+\*mail,\s*"
        r"bool\s+get_body,\s*struct\s+message_size\s+\*hdr_size,\s*"
        r"struct\s+message_size\s+\*body_size,\s*struct\s+istream\s+\*\*stream\s*\)",
        text,
        "get_stream signature with message size outputs",
    )
    require(
        r"struct\s+wyrebox_dovecot_storage\s*\{\s*"
        r"struct\s+mail_storage\s+storage;",
        text,
        "wyrebox storage wrapper embeds mail_storage first",
    )
    require(
        r"struct\s+wyrebox_dovecot_storage\s*\{\s*[\s\S]*?"
        r"struct\s+mail_storage\s+storage;\s*[\s\S]*?"
        r"char\s+\*socket_path;\s*[\s\S]*?"
        r"char\s+\*account_identity;\s*[\s\S]*?"
        r"}\s*;",
        text,
        "plugin-owned daemon config state fields",
    )
    require(
        r"struct\s+wyrebox_dovecot_mailbox\s*\{\s*[\s\S]*?"
        r"struct\s+mailbox\s+mailbox;\s*[\s\S]*?"
        r"WyreboxDaemonMailboxSelectResult\s+select_result;\s*[\s\S]*?"
        r"int\s+select_result_valid;\s*[\s\S]*?"
        r"WyreboxDovecotMailboxUidMapSnapshot\s+uid_map_snapshot;\s*[\s\S]*?"
        r"}\s*;",
        text,
        "wyrebox mailbox wrapper embeds SELECT result state",
    )
    require(
        r"struct\s+wyrebox_dovecot_mail\s*\{\s*[\s\S]*?"
        r"struct\s+mail\s+mail;\s*[\s\S]*?"
        r"struct\s+istream\s+\*stream;\s*[\s\S]*?"
        r"}\s*;",
        text,
        "wyrebox mail wrapper embeds cached stream state",
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
        r"static\s+gboolean\s+wyrebox_dovecot_mailbox_refresh_select_result",
        text,
        "mailbox open uses shared SELECT refresh helper",
    )
    require(
        r"typedef\s+gboolean\s+\(\*WyreboxDovecotMailboxListPublishFunc\)"
        r"\s*\(\s*struct\s+mailbox_list\s+\*\s*list,\s*"
        r"const\s+char\s+\*name,\s*char\s+hierarchy_delimiter,\s*"
        r"gboolean\s+selectable,\s*"
        r"enum\s+mailbox_list_child_state\s+child_state,\s*"
        r"const\s+char\s+\*special_use,\s*gpointer\s+user_data\s*\)",
        text,
        "mailbox LIST publisher callback seam",
    )
    require(
        r"gboolean\s+wyrebox_dovecot_publish_mailbox_list_result"
        r"\s*\(\s*struct\s+mailbox_list\s+\*list,\s*"
        r"const\s+WyreboxDaemonMailboxListResult\s+\*result,\s*"
        r"WyreboxDovecotMailboxListPublishFunc\s+publisher,\s*"
        r"gpointer\s+publisher_data,\s*"
        r"GError\s+\*\*\s*error\s*\)",
        text,
        "mailbox LIST publication adapter",
    )
    mailbox_list_publish_body = function_body(
        "wyrebox_dovecot_publish_mailbox_list_result", text)
    for required_fragment in [
        "wyrebox_daemon_mailbox_list_result_get_n_entries (result)",
        "wyrebox_daemon_mailbox_list_result_get_entry (result, i)",
        "entry->mailbox_name",
        "entry->hierarchy_delimiter",
        "entry->is_selectable",
        "entry->special_use",
        "publisher (list",
        "publisher_data",
    ]:
        if required_fragment not in mailbox_list_publish_body:
            raise AssertionError(
                "plugin source contract failed: mailbox LIST adapter missing "
                f"publication fragment: {required_fragment}"
            )
    require(
        r"wyrebox_dovecot_map_mailbox_list_child_state\s*\([^)]*\)\s*\{"
        r"[\s\S]*?WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN"
        r"[\s\S]*?MAILBOX_LIST_CHILD_STATE_UNKNOWN"
        r"[\s\S]*?WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN"
        r"[\s\S]*?MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN"
        r"[\s\S]*?WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN"
        r"[\s\S]*?MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN",
        text,
        "mailbox LIST adapter maps child state enum values",
    )
    for required_fragment in [
        "list == NULL",
        "result == NULL",
        "publisher == NULL",
        "result->entries == NULL",
        "entry->hierarchy_delimiter[1] != '\\0'",
        "G_IO_ERROR_FAILED",
    ]:
        if required_fragment not in mailbox_list_publish_body:
            raise AssertionError(
                "plugin source contract failed: mailbox LIST adapter missing "
                f"validation fragment: {required_fragment}"
            )
    require(
        r"wyrebox_dovecot_mailbox_refresh_select_result\s*\(\s*struct\s+mailbox\s+\*box,\s*GError\s+\*\*\s*\*?error\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mailbox_clear_cache\s*\(\s*wbox\s*\);\s*"
        r"[\s\S]*?wyrebox_dovecot_daemon_client_select_mailbox\s*\(\s*"
        r"storage->socket_path,\s*storage->account_identity,\s*box->vname,\s*"
        r"&select_result,\s*error\)[\s\S]*?"
        r"wyrebox_daemon_mailbox_select_result_init\s*\(\s*&wbox->select_result,\s*"
        r"select_result\.kind,\s*select_result\.mailbox_id,\s*"
        r"select_result\.mailbox_name,\s*select_result\.uid_validity,\s*"
        r"select_result\.uid_next,\s*select_result\.message_count,\s*error\)[\s\S]*?"
        r"[\s\S]*?wyrebox_dovecot_daemon_client_load_uid_map\s*\(\s*"
        r"storage->socket_path,\s*storage->account_identity,\s*"
        r"select_result\.mailbox_id,\s*"
        r"select_result\.kind,\s*"
        r"select_result\.uid_validity,\s*&uid_map_snapshot,\s*error\)[\s\S]*?"
        r"wbox->uid_map_snapshot\.rows\s*=\s*g_steal_pointer\s*\(\s*&uid_map_snapshot\.rows\s*\);\s*"
        r"wbox->select_result_valid\s*=\s*1;",
        text,
        "mailbox SELECT refresh helper clears stale state and loads UID map",
    )
    require(
        r"wyrebox_dovecot_mailbox_open\s*\(\s*struct\s+mailbox\s+\*box\s*\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mailbox_set_opened\s*\(\s*box,\s*FALSE\);\s*"
        r"[\s\S]*?if\s*\(\s*!\s*wyrebox_dovecot_mailbox_refresh_select_result\s*\(\s*box,\s*&error\)\)\s*\{[\s\S]*?"
        r"mail_storage_set_error\s*\(\s*box->storage,\s*MAIL_ERROR_NOTPOSSIBLE,\s*"
        r"wyrebox_dovecot_mailbox_error_message\s*\(\s*error\s*\)\s*\)\s*;\s*"
        r"[\s\S]*?return\s+-1;\s*\}\s*[\s\S]*?"
        r"wyrebox_dovecot_mailbox_set_opened\s*\(\s*box,\s*TRUE\)\s*;\s*[\s\S]*?"
        r"return\s+0;",
        text,
        "mailbox open sets opened state only after successful SELECT refresh",
    )
    require(
        r"wyrebox_dovecot_mailbox_enable\s*\(\s*struct\s+mailbox\s+\*box,\s*"
        r"enum\s+mailbox_feature\s+features\s*\)",
        text,
        "mailbox enable helper exists",
    )
    require(
        r"wyrebox_dovecot_mailbox_enable\s*\(\s*struct\s+mailbox\s+\*box,\s*enum\s+"
        r"mailbox_feature\s+features\s*\)\s*\{[\s\S]*?"
        r"box->enabled_features\s*\|\=\s*features;\s*[\s\S]*?"
        r"return\s+0;",
        text,
        "mailbox enable updates enabled_features and succeeds",
    )
    require(
        r"wyrebox_dovecot_mailbox_close\s*\(\s*struct\s+mailbox\s+\*box\s*\)",
        text,
        "mailbox close helper exists",
    )
    require(
        r"wyrebox_dovecot_mailbox_close\s*\(\s*struct\s+mailbox\s+\*box\s*\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mailbox_set_opened\s*\(\s*box,\s*FALSE\);\s*"
        r"[\s\S]*?wyrebox_dovecot_mailbox_clear_cache\s*\(\s*wbox\s*\);\s*"
        r"[\s\S]*?wbox->select_result_valid\s*=\s*0;",
        text,
        "mailbox close clears cached SELECT state and UID map",
    )
    require(
        r"wyrebox_dovecot_mailbox_close\s*\(\s*struct\s+mailbox\s+\*box\s*\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mailbox_set_opened\s*\(\s*box,\s*FALSE\);\s*"
        r"[\s\S]*?wyrebox_dovecot_mailbox_clear_cache\s*\(\s*wbox\s*\);\s*"
        r"[\s\S]*?wbox->select_result_valid\s*=\s*0;",
        text,
        "mailbox close resets opened state",
    )
    require(
        r"wyrebox_dovecot_mailbox_sync_init\s*\(\s*struct\s+mailbox\s+\*box\s*,\s*"
        r"enum\s+mailbox_sync_flags\s+flags\)",
        text,
        "mailbox sync_init helper exists",
    )
    require(
        r"wyrebox_dovecot_mailbox_sync_init\s*\(\s*struct\s+mailbox\s+\*box,\s*"
        r"enum\s+mailbox_sync_flags\s+flags\s*\)\s*\{[\s\S]*?"
        r"struct\s+mailbox_sync_context\s+\*ctx;\s*"
        r"[\s\S]*?ctx\s*=\s*malloc\s*\(\s*sizeof\s*\(\s*\*ctx\s*\)\s*\);\s*"
        r"[\s\S]*?ctx->box\s*=\s*box;\s*[\s\S]*?ctx->flags\s*=\s*flags;\s*[\s\S]*?"
        r"return\s+ctx;",
        text,
        "mailbox sync_init allocates and initializes sync context",
    )
    require(
        r"wyrebox_dovecot_mailbox_sync_next\s*\(\s*struct\s+mailbox_sync_context\s+\*ctx,\s*"
        r"struct\s+mailbox_sync_rec\s+\*sync_rec_r\s*\)\s*\{[\s\S]*?return\s+FALSE;",
        text,
        "mailbox sync_next returns FALSE/no changes",
    )
    require(
        r"wyrebox_dovecot_mailbox_sync_deinit\s*\(\s*struct\s+mailbox_sync_context\s+\*ctx,\s*"
        r"struct\s+mailbox_sync_status\s+\*status_r\s*\)\s*\{[\s\S]*?"
        r"if\s*\(\s*status_r\s*!=\s*NULL\)\s*\{[\s\S]*?memset\s*\(\s*status_r,\s*0,\s*sizeof\s*\(\s*\*status_r\s*\)\s*\);\s*\}\s*[\s\S]*?free\s*\(\s*ctx\s*\);\s*return\s+0;",
        text,
        "mailbox sync_deinit zeros sync status and frees context",
    )
    mailbox_open_body = function_body("wyrebox_dovecot_mailbox_open", text)
    for forbidden_call in [
        r"\bconnect\s*\(",
        r"\bsocket\s*\(",
        r"\bwyrebox_dovecot_storage_get_daemon_",
    ]:
        if re.search(forbidden_call, mailbox_open_body) is not None:
            raise AssertionError(
                "plugin source contract failed: mailbox open must not use "
                f"unsupported daemon helper path: {forbidden_call}"
            )
    get_stream_body = function_body("wyrebox_dovecot_mail_get_stream", text)
    if "i_stream_create_copy_from_data (data, size)" not in get_stream_body:
        raise AssertionError(
            "plugin source contract failed: get_stream must copy daemon bytes "
            "into the returned stream"
        )
    if "i_stream_create_from_data" in get_stream_body:
        raise AssertionError(
            "plugin source contract failed: get_stream must not return a "
            "borrowed daemon byte stream"
        )
    for required_fragment in [
        "wyrebox_dovecot_message_size_scan (data, size, hdr_size, body_size)",
        "wyrebox_dovecot_mail_clear_stream (wmail)",
        "wmail->stream = i_stream_create_copy_from_data (data, size)",
        "*stream = wmail->stream",
    ]:
        if required_fragment not in get_stream_body:
            raise AssertionError(
                "plugin source contract failed: get_stream missing size "
                f"fragment: {required_fragment}"
            )
    require(
        r"static\s+void\s+wyrebox_dovecot_mail_close"
        r"\s*\(\s*struct\s+mail\s+\*mail\s*\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mail_clear_stream\s*\(\s*wmail\s*\);",
        text,
        "mail close clears cached stream",
    )
    require(
        r"static\s+void\s+wyrebox_dovecot_mail_free"
        r"\s*\(\s*struct\s+mail\s+\*mail\s*\)\s*\{[\s\S]*?"
        r"wyrebox_dovecot_mail_clear_stream\s*\(\s*wmail\s*\);[\s\S]*?"
        r"free\s*\(\s*wmail\s*\);",
        text,
        "mail free clears cached stream and releases wrapper",
    )
    require(
        r"static\s+struct\s+mail\s+\*"
        r"\s*wyrebox_dovecot_mail_alloc\s*\(\s*"
        r"struct\s+mailbox_transaction_context\s+\*transaction,\s*"
        r"enum\s+mail_fetch_field\s+wanted_fields,\s*"
        r"struct\s+mailbox_header_lookup_ctx\s+\*wanted_headers\s*\)",
        text,
        "mail allocator vfunc signature",
    )
    require(
        r"wyrebox_dovecot_mailbox_get_status\s*\(\s*struct\s+mailbox\s+\*box\s*,"
        r"[\s\S]*?struct\s+mailbox_status\s+\*status_r\)",
        text,
        "mailbox get_status wrapper",
    )
    require(
        r"if\s*\(!wbox->select_result_valid\s*&&\s*"
        r"!\s*wyrebox_dovecot_mailbox_refresh_select_result\s*\(\s*box,\s*&error\)\)\s*\{[\s\S]*?"
        r"mail_storage_set_error\s*\(\s*box->storage,\s*"
        r"MAIL_ERROR_NOTPOSSIBLE,\s*"
        r"wyrebox_dovecot_mailbox_error_message\s*\(\s*error\s*\)\s*\);\s*[\s\S]*?"
        r"return\s+-1;\s*\}",
        text,
        "mailbox get_status lazily refreshes SELECT state",
    )
    require(
        r"status_r->messages\s*=\s*wbox->select_result.message_count;\s*[\s\S]*?"
        r"status_r->uidvalidity\s*=\s*wbox->select_result.uid_validity;\s*[\s\S]*?"
        r"status_r->uidnext\s*=\s*wbox->select_result.uid_next;",
        text,
        "mailbox get_status reflects cached SELECT uid state",
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
        r"->mailbox\.vname\s*=\s*p_strdup\s*\(\s*pool,\s*vname\s*\);\s*[\s\S]*?"
        r"->mailbox\.name\s*=\s*p_strdup\s*\(\s*pool,\s*name\s*\);\s*[\s\S]*?"
        r"->mailbox\.event\s*=\s*event_create\s*\(\s*storage->event\s*\);\s*[\s\S]*?"
        r"->mailbox\.mail_vfuncs\s*=\s*&wyrebox_dovecot_mail_vfuncs;\s*[\s\S]*?"
        r"->mailbox\.vlast\s*=\s*NULL;",
        text,
        "mailbox allocator sets required base fields",
    )
    require(
        r"mailbox_list_get_storage_name\s*\(",
        text,
        "mailbox storage-name helper used",
    )
    require(
        r"p_array_init\s*\(\s*&wbox->mailbox\.search_results,\s*pool,\s*16\s*\);",
        text,
        "mailbox search_results initialized",
    )
    require(
        r"p_array_init\s*\(\s*&wbox->mailbox\.module_contexts,\s*pool,\s*5\s*\);",
        text,
        "mailbox module_contexts initialized",
    )
    require(
        r"wyrebox_daemon_mailbox_select_result_clear\s*\(\s*&wbox->select_result\s*\);\s*"
        r"[\s\S]*?wbox->select_result_valid\s*=\s*0;",
        text,
        "mailbox SELECT result state initialized",
    )
    require(
        r"->mailbox\.event\s*=\s*event_create\s*\(\s*storage->event\s*\);",
        text,
        "mailbox event initialized with event_create",
    )
    forbid(
        r"->mailbox\.event\s*=\s*NULL;\s*",
        text,
        "mailbox event initialized to NULL",
    )
    require(
        r"event_unref\s*\(\s*&box->event\s*\);",
        text,
        "mailbox free releases event",
    )
    require(
        r"wyrebox_dovecot_mailbox_free\s*\(\s*struct\s+mailbox\s+\*box\s*\)"
        r"\s*\{[\s\S]*?"
        r"struct\s+wyrebox_dovecot_mailbox\s+\*wbox\s*="
        r"\s*\(struct\s+wyrebox_dovecot_mailbox\s+\*\)\s*box;\s*"
        r"[\s\S]*?"
        r"wyrebox_dovecot_mailbox_clear_cache\s*\(\s*wbox\s*\);"
        r"[\s\S]*?"
        r"wbox->select_result_valid\s*=\s*0;",
        text,
        "mailbox free clears cached state",
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
        r"->mailbox\.v\.enable\s*=\s*wyrebox_dovecot_mailbox_enable;",
        text,
        "mailbox enable vfunc wired",
    )
    require(
        r"->mailbox\.v\.close\s*=\s*wyrebox_dovecot_mailbox_close;",
        text,
        "mailbox close vfunc wired",
    )
    require(
        r"->mailbox\.v\.sync_init\s*=\s*wyrebox_dovecot_mailbox_sync_init;",
        text,
        "mailbox sync_init vfunc wired",
    )
    require(
        r"->mailbox\.v\.sync_next\s*=\s*wyrebox_dovecot_mailbox_sync_next;",
        text,
        "mailbox sync_next vfunc wired",
    )
    require(
        r"->mailbox\.v\.sync_deinit\s*=\s*wyrebox_dovecot_mailbox_sync_deinit;",
        text,
        "mailbox sync_deinit vfunc wired",
    )
    require(
        r"->mailbox\.v\.mail_alloc\s*=\s*wyrebox_dovecot_mail_alloc;",
        text,
        "mailbox mail_alloc vfunc wired",
    )
    require(
        r"static\s+const\s+struct\s+mail_vfuncs\s+wyrebox_dovecot_mail_vfuncs"
        r"\s*=\s*\{[\s\S]*?\.close\s*=\s*wyrebox_dovecot_mail_close,"
        r"[\s\S]*?\.free\s*=\s*wyrebox_dovecot_mail_free,"
        r"[\s\S]*?\.get_stream\s*=\s*wyrebox_dovecot_mail_get_stream,",
        text,
        "mail lifecycle and fetch vfuncs wired",
    )
    require(
        r"->mailbox\.opened\s*=\s*FALSE;",
        text,
        "mailbox opened state initialized false",
    )
    require(
        r"->mailbox\.enabled_features\s*=\s*0;",
        text,
        "mailbox enabled_features initialized zero",
    )
    require(
        r"return\s+&\w+->mailbox;",
        text,
        "mailbox allocator returns mailbox",
    )
    require(
        r"status_r->messages\s*=\s*wbox->select_result\.message_count;",
        text,
        "mailbox get_status reflects cached SELECT message count",
    )
    forbid(
        r"WyreBox mailbox open is not implemented yet",
        text,
        "obsolete open not-implemented message",
    )
    require(
        r"static\s+struct\s+mail_storage\s+wyrebox_mail_storage_class\s*=\s*\{",
        text,
        "wyrebox storage class definition",
    )
    require(r"\.name\s*=\s*\"wyrebox\"", text, "storage class name")
    require(r"\.class_flags\s*=\s*0", text, "storage class flags")
    require(
        r"static\s+void\s+wyrebox_dovecot_storage_add_list"
        r"\s*\(\s*struct\s+mail_storage\s+\*storage,\s*"
        r"struct\s+mailbox_list\s+\*list\s*\)",
        text,
        "mailbox LIST hook installer",
    )
    add_list_body = function_body("wyrebox_dovecot_storage_add_list", text)
    for required_fragment in [
        "context->previous_vfuncs = list->v;",
        "context->previous_vlast = list->vlast;",
        "context->socket_path = g_strdup (wstorage->socket_path);",
        "context->account_identity = g_strdup (wstorage->account_identity);",
        "list->vlast = &context->previous_vfuncs;",
        "list->v.iter_init = wyrebox_dovecot_mailbox_list_iter_init;",
        "list->v.iter_next = wyrebox_dovecot_mailbox_list_iter_next;",
        "list->v.iter_deinit = wyrebox_dovecot_mailbox_list_iter_deinit;",
    ]:
        if required_fragment not in add_list_body:
            raise AssertionError(
                "plugin source contract failed: add_list missing hook "
                f"installer fragment: {required_fragment}"
            )
    for forbidden_fragment in [
        "wyrebox_dovecot_daemon_client_list_mailboxes",
        "wyrebox_dovecot_publish_mailbox_list_result",
        "wyrebox_dovecot_daemon_client",
        "g_socket_client_connect",
    ]:
        if forbidden_fragment in add_list_body:
            raise AssertionError(
                "plugin source contract failed: add_list must not perform "
                f"daemon LIST/socket work: {forbidden_fragment}"
            )
    iter_init_body = function_body(
        "wyrebox_dovecot_mailbox_list_iter_init", text)
    if "wyrebox_dovecot_daemon_client_list_mailboxes" not in iter_init_body:
        raise AssertionError(
            "plugin source contract failed: iter_init must perform daemon LIST"
        )
    iter_append_body = function_body(
        "wyrebox_dovecot_mailbox_list_iter_append_entry", text)
    iter_flags_body = function_body(
        "wyrebox_dovecot_mailbox_list_flags_from_entry", text)
    for required_fragment in [
        "hook_context->socket_path",
        "hook_context->account_identity",
        "\"\"",
        "&result",
        "context->ctx.list = list;",
        "context->ctx.flags = flags;",
    ]:
        if required_fragment not in text:
            raise AssertionError(
                "plugin source contract failed: LIST iterator missing "
                f"fragment: {required_fragment}"
            )
    for required_fragment in [
        "info->vname = g_strdup (entry->mailbox_name);",
        "info->special_use = g_strdup (entry->special_use);",
        "flags |= MAILBOX_NOSELECT;",
        "flags |= MAILBOX_CHILDREN;",
        "flags |= MAILBOX_NOCHILDREN;",
        "return &context->entries[context->next_entry++];",
    ]:
        if required_fragment not in text:
            raise AssertionError(
                "plugin source contract failed: daemon LIST entry mapping "
                f"missing fragment: {required_fragment}"
            )
    for required_fragment in [
        "wyrebox_dovecot_mailbox_list_pattern_matches",
        "wyrebox_dovecot_map_mailbox_list_child_state (entry->child_state",
        "g_steal_pointer (&entries)",
        "context->entries = g_steal_pointer (&entries);",
        "context->n_entries = n_published_entries;",
    ]:
        if required_fragment not in iter_init_body:
            raise AssertionError(
                "plugin source contract failed: daemon LIST iterator missing "
                f"validation/filter publication fragment: {required_fragment}"
            )
    for required_fragment in [
        "wyrebox_dovecot_map_mailbox_list_child_state (entry->child_state",
        "MAILBOX_LIST_ITER_RETURN_NO_FLAGS",
        "MAILBOX_LIST_ITER_RETURN_SPECIALUSE",
    ]:
        if required_fragment not in iter_append_body:
            raise AssertionError(
                "plugin source contract failed: daemon LIST iterator mapper "
                f"missing flag/child-state fragment: {required_fragment}"
            )
    for required_fragment in [
        "MAILBOX_LIST_ITER_RETURN_NO_FLAGS",
        "MAILBOX_LIST_ITER_RETURN_CHILDREN",
    ]:
        if required_fragment not in iter_flags_body:
            raise AssertionError(
                "plugin source contract failed: daemon LIST iterator flags "
                f"missing return-flag fragment: {required_fragment}"
            )
    require(
        r"\.v\s*=\s*\{[\s\S]*?\.add_list\s*=\s*"
        r"wyrebox_dovecot_storage_add_list,[\s\S]*?"
        r"\.mailbox_alloc\s*=\s*wyrebox_dovecot_mailbox_alloc",
        text,
        "storage vfuncs wire add_list hook installer",
    )
    require(
        r"\.v\s*=\s*\{[\s\S]*?\.alloc\s*=\s*wyrebox_dovecot_storage_alloc,[\s\S]*?"
        r"\.create\s*=\s*wyrebox_dovecot_storage_create,[\s\S]*?"
        r"\.destroy\s*=\s*wyrebox_dovecot_storage_destroy",
        text,
        "wired storage lifecycle vfuncs",
    )
    require(
        r"extern\s+const\s+char\s+\*wyrebox_dovecot_test_daemon_socket_path"
        r"\s+__attribute__\s*\(\s*\(\s*weak\s*\)\s*\)\s*;",
        text,
        "test-only weak daemon socket path seam",
    )
    require(
        r"wyrebox_dovecot_socket_path\s*\(\s*void\s*\)\s*\{[\s\S]*?"
        r"&wyrebox_dovecot_test_daemon_socket_path\s*!=\s*NULL[\s\S]*?"
        r"wyrebox_dovecot_test_daemon_socket_path\s*!=\s*NULL[\s\S]*?"
        r"wyrebox_dovecot_test_daemon_socket_path\[0\]\s*!=\s*'\\0'[\s\S]*?"
        r"return\s+wyrebox_dovecot_test_daemon_socket_path;[\s\S]*?"
        r"return\s+\"/run/wyrebox/wyrebox\.sock\";",
        text,
        "socket path helper preserves default and allows test weak override",
    )
    forbid(
        r"WYREBOX_DOVECOT_PLUGIN_EXPORT\s+void\s+"
        r"wyrebox_dovecot_plugin_set_daemon_socket_path_for_testing",
        text,
        "exported test socket path setter",
    )
    require(
        r"wyrebox_dovecot_storage_create\s*\(\s*struct\s+mail_storage\s+\*storage\s*,"
        r"[\s\S]*?struct\s+mail_namespace\s+\*ns,"
        r"[\s\S]*?const\s+char\s+\*\*error_r\s*\)\s*\{[\s\S]*?"
        r"struct\s+wyrebox_dovecot_storage\s+\*wstorage\s*="
        r"\s*\(struct\s+wyrebox_dovecot_storage\s+\*\)\s*storage;\s*"
        r"[\s\S]*?const\s+char\s+\*account_identity;\s*[\s\S]*?"
        r"if\s*\(\s*ns\s*==\s*NULL\s*\|\|\s*ns->user\s*==\s*NULL\s*\|\|\s*"
        r"ns->user->username\s*==\s*NULL\s*\|\|\s*"
        r"ns->user->username\[0\]\s*==\s*'\\0'\s*\)\s*\{[\s\S]*?"
        r"if\s*\(\s*error_r\s*!=\s*NULL\s*\)[\s\S]*?"
        r"\*error_r\s*=\s*\"Dovecot namespace user identity is unavailable\";"
        r"[\s\S]*?return\s+-1;\s*[\s\S]*?"
        r"account_identity\s*=\s*ns->user->username;\s*[\s\S]*?"
        r"wstorage->socket_path\s*=\s*[\s\S]*?"
        r"\(\s*wyrebox_dovecot_socket_path\s*\(\s*\)\s*\);[\s\S]*?"
        r"wstorage->account_identity\s*=\s*[\s\S]*?"
        r"\(\s*account_identity\s*\);[\s\S]*?"
        r"if\s*\(\s*wstorage->socket_path\s*==\s*NULL\s*\|\|\s*"
        r"wstorage->account_identity\s*==\s*NULL\s*\)\s*\{[\s\S]*?"
        r"if\s*\(\s*error_r\s*!=\s*NULL\s*\)[\s\S]*?"
        r"\*error_r\s*=\s*\"Failed to allocate WyreBox Dovecot storage state\";"
        r"[\s\S]*?return\s+-1;\s*[\s\S]*?"
        r"return\s+0;\s*\}",
        text,
        "create initializes plugin-owned daemon config state",
    )
    forbid(
        r"dovecot-account-identity-unavailable",
        text,
        "placeholder account identity",
    )
    require(
        r"wyrebox_dovecot_strdup\s*\(\s*const\s+char\s+\*str\s*\)\s*\{[\s\S]*?"
        r"malloc\s*\(\s*size\s*\)[\s\S]*?"
        r"memcpy\s*\(\s*copy\s*,\s*str\s*,\s*size\s*\)",
        text,
        "create copies daemon config state into plugin-owned memory",
    )
    require(
        r"static\s+void\s+wyrebox_dovecot_storage_destroy\s*\(\s*struct\s+mail_storage\s*\*storage\s*\)\s*\{[\s\S]*?"
        r"struct\s+wyrebox_dovecot_storage\s+\*wstorage\s*="
        r"\s*\(struct\s+wyrebox_dovecot_storage\s+\*\)\s*storage;\s*"
        r"[\s\S]*?free\s*\(\s*wstorage->socket_path\s*\);\s*"
        r"[\s\S]*?free\s*\(\s*wstorage->account_identity\s*\);"
        r"[\s\S]*?wstorage->socket_path\s*=\s*NULL;"
        r"[\s\S]*?wstorage->account_identity\s*=\s*NULL;"
        r"[\s\S]*?\}",
        text,
        "destroy releases only plugin-owned daemon config state",
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
