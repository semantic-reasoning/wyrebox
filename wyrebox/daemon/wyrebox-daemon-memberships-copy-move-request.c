#include "wyrebox-daemon-memberships-copy-move-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
    if (value == NULL || *value == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move %s is required", field_name);
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (g_ascii_iscntrl (*cursor)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships copy/move %s must not contain control "
                "characters",
                field_name);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
validate_source_uids (const guint64 *source_uids, gsize n_source_uids,
    GError **error)
{
    if (source_uids == NULL || n_source_uids == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move requires at least one source uid");
        return FALSE;
    }

    if (n_source_uids > WYREBOX_DAEMON_MEMBERSHIPS_COPY_MOVE_MAX_UIDS) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move has too many source uids");
        return FALSE;
    }

    for (gsize i = 0; i < n_source_uids; i++) {
        if (source_uids[i] == 0) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships copy/move source uid must be at least 1");
            return FALSE;
        }

        if (i > 0 && source_uids[i] <= source_uids[i - 1]) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships copy/move source uids must be strictly "
                "ascending");
            return FALSE;
        }
    }

    return TRUE;
}

void
wyrebox_daemon_memberships_copy_move_request_clear
    (WyreboxDaemonMembershipsCopyMoveRequest *request)
{
    if (request == NULL)
        return;

    g_clear_pointer (&request->account_identity, g_free);
    g_clear_pointer (&request->source_mailbox_id, g_free);
    request->source_namespace_kind =
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
    request->source_uid_validity = 0;
    g_clear_pointer (&request->dest_mailbox_id, g_free);
    request->dest_uid_validity = 0;
    g_clear_pointer (&request->source_uids, g_free);
    request->n_source_uids = 0;
}

gboolean
wyrebox_daemon_memberships_copy_move_request_init
    (WyreboxDaemonMembershipsCopyMoveRequest *request,
    const char *account_identity, const char *source_mailbox_id,
    WyreboxDaemonMailboxListEntryKind source_namespace_kind,
    guint64 source_uid_validity, const char *dest_mailbox_id,
    guint64 dest_uid_validity, const guint64 *source_uids,
    gsize n_source_uids, GError **error)
{
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) next = { 0 };

    g_return_val_if_fail (request != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!validate_non_empty_text (account_identity, "account_identity", error))
        return FALSE;

    if (!validate_non_empty_text (source_mailbox_id, "source_mailbox_id",
        error))
        return FALSE;

    if (!validate_non_empty_text (dest_mailbox_id, "dest_mailbox_id", error))
        return FALSE;

    if (source_namespace_kind != WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY &&
        source_namespace_kind != WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move source_namespace_kind is invalid");
        return FALSE;
    }

    if (source_uid_validity == 0 || dest_uid_validity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move uidvalidity is required");
        return FALSE;
    }

    if (!validate_source_uids (source_uids, n_source_uids, error))
        return FALSE;

    next.account_identity = g_strdup (account_identity);
    next.source_mailbox_id = g_strdup (source_mailbox_id);
    next.source_namespace_kind = source_namespace_kind;
    next.source_uid_validity = source_uid_validity;
    next.dest_mailbox_id = g_strdup (dest_mailbox_id);
    next.dest_uid_validity = dest_uid_validity;
    next.source_uids = g_new (guint64, n_source_uids);
    memcpy (next.source_uids, source_uids, n_source_uids * sizeof (guint64));
    next.n_source_uids = n_source_uids;

    wyrebox_daemon_memberships_copy_move_request_clear (request);
    *request = next;
    next.account_identity = NULL;
    next.source_mailbox_id = NULL;
    next.dest_mailbox_id = NULL;
    next.source_uids = NULL;

    return TRUE;
}
