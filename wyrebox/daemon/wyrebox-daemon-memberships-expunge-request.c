#include "wyrebox-daemon-memberships-expunge-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
    if (value == NULL || *value == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships expunge %s is required", field_name);
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (g_ascii_iscntrl (*cursor)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships expunge %s must not contain control characters",
                field_name);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
validate_uids (const guint64 *uids, gsize n_uids, GError **error)
{
    if (uids == NULL || n_uids == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships expunge requires at least one uid");
        return FALSE;
    }

    for (gsize i = 0; i < n_uids; i++) {
        if (uids[i] == 0) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships expunge uid must be at least 1");
            return FALSE;
        }

        if (i > 0 && uids[i] <= uids[i - 1]) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "memberships expunge uids must be strictly ascending");
            return FALSE;
        }
    }

    return TRUE;
}

void
wyrebox_daemon_memberships_expunge_request_clear
    (WyreboxDaemonMembershipsExpungeRequest *request)
{
    if (request == NULL)
        return;

    g_clear_pointer (&request->account_identity, g_free);
    g_clear_pointer (&request->mailbox_id, g_free);
    request->uid_validity = 0;
    g_clear_pointer (&request->uids, g_free);
    request->n_uids = 0;
}

gboolean
wyrebox_daemon_memberships_expunge_request_init
    (WyreboxDaemonMembershipsExpungeRequest *request,
    const char *account_identity, const char *mailbox_id, guint64 uid_validity,
    const guint64 *uids, gsize n_uids, GError **error)
{
    g_auto (WyreboxDaemonMembershipsExpungeRequest) next = { 0 };

    g_return_val_if_fail (request != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!validate_non_empty_text (account_identity, "account_identity", error))
        return FALSE;

    if (!validate_non_empty_text (mailbox_id, "mailbox_id", error))
        return FALSE;

    if (uid_validity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships expunge uid_validity is required");
        return FALSE;
    }

    if (!validate_uids (uids, n_uids, error))
        return FALSE;

    next.account_identity = g_strdup (account_identity);
    next.mailbox_id = g_strdup (mailbox_id);
    next.uid_validity = uid_validity;
    next.uids = g_new (guint64, n_uids);
    memcpy (next.uids, uids, n_uids * sizeof (guint64));
    next.n_uids = n_uids;

    wyrebox_daemon_memberships_expunge_request_clear (request);
    *request = next;
    next.account_identity = NULL;
    next.mailbox_id = NULL;
    next.uids = NULL;

    return TRUE;
}
