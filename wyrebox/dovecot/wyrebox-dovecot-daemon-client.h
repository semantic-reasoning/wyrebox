#pragma once

#include "wyrebox-daemon-error.h"
#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-memberships-expunge-request.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include <glib-object.h>

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

/* Errors the daemon reported in an error response use this domain, with the
 * daemon's WyreboxDaemonErrorClass as the code. Transport and decoding
 * failures stay in G_IO_ERROR. */
#define WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR \
    (wyrebox_dovecot_daemon_client_error_quark ())

GQuark wyrebox_dovecot_daemon_client_error_quark (void);

typedef struct
{
  char *account_id;
  char *mailbox_id;
  guint64 uid_validity;
  guint64 uid;
  char *message_id;
  char *object_id;
  /* System flags followed by user keywords; never NULL, empty for views. */
  GStrv flags;
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

gboolean wyrebox_dovecot_daemon_client_get_mailbox_status (
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
    WyreboxDaemonMailboxListEntryKind kind,
    guint64 uid_validity,
    guint64 mailbox_uid,
    const char *expected_message_id,
    GError **error);

/*
 * Applies one flag/keyword delta to an ordinary mailbox message. Inputs are
 * borrowed. The request is validated locally before any I/O, so invalid
 * flags or keywords fail with G_IO_ERROR_INVALID_ARGUMENT. Daemon refusals use
 * WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR; protocol mismatches use
 * G_IO_ERROR_INVALID_DATA. @out_journal_offset may be NULL; when given it
 * receives the journal offset on success and 0 on failure.
 */
gboolean wyrebox_dovecot_daemon_client_update_flags (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    guint64 mailbox_uid,
    WyreboxDaemonFlagKeywordUpdateMode mode,
    const char *const *system_flags,
    const char *const *user_keywords,
    guint64 *out_journal_offset,
    GError **error);

gboolean wyrebox_dovecot_daemon_client_expunge (
    const char *socket_path,
    const char *account_identity,
    const char *mailbox_id,
    guint64 uid_validity,
    const guint64 *uids,
    gsize n_uids,
    guint64 *out_journal_offset,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
