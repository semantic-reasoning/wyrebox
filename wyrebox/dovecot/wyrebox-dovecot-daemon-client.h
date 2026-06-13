#pragma once

#include "wyrebox-daemon-mailbox-select-result.h"
#include <glib-object.h>

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  char *account_id;
  char *mailbox_id;
  guint64 uid_validity;
  guint64 uid;
  char *message_id;
  char *object_id;
} WyreboxDovecotMailboxUidMapRow;

void wyrebox_dovecot_mailbox_uid_map_row_clear (
    WyreboxDovecotMailboxUidMapRow *row);

void wyrebox_dovecot_mailbox_uid_map_row_free (
    WyreboxDovecotMailboxUidMapRow *row);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDovecotMailboxUidMapRow,
    wyrebox_dovecot_mailbox_uid_map_row_clear)

typedef struct
{
  GPtrArray *rows;
} WyreboxDovecotMailboxUidMapSnapshot;

void wyrebox_dovecot_mailbox_uid_map_snapshot_clear (
    WyreboxDovecotMailboxUidMapSnapshot *snapshot);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDovecotMailboxUidMapSnapshot,
    wyrebox_dovecot_mailbox_uid_map_snapshot_clear)

gboolean wyrebox_dovecot_daemon_client_select_mailbox (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_name,
    WyreboxDaemonMailboxSelectResult *out_result,
    GError **error);

gboolean wyrebox_dovecot_daemon_client_list_mailboxes (
    const char *socket_path,
    const char *account_identity,
    const char *namespace_prefix,
    WyreboxDaemonMailboxListResult *out_result,
    GError **error);

gboolean wyrebox_dovecot_daemon_client_load_uid_map (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    WyreboxDaemonMailboxListEntryKind kind,
    guint64 uid_validity,
    WyreboxDovecotMailboxUidMapSnapshot *out_snapshot,
    GError **error);

GBytes *wyrebox_dovecot_daemon_client_fetch_message_bytes (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    const char *expected_message_id,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
