#include "wyrebox-daemon-message-append-request.h"

#include <gio/gio.h>

static gboolean
validate_non_empty_text (const char *value, const char *field_name,
    GError **error)
{
    if (value == NULL || *value == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message append %s is required", field_name);
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (g_ascii_iscntrl (*cursor)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "message append %s must not contain control characters",
                field_name);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
validate_system_flag (const char *value, GError **error)
{
    static const char *valid_system_flags[] = {
        "\\Seen",
        "\\Answered",
        "\\Flagged",
        "\\Deleted",
        "\\Draft",
        NULL
    };

    if (!validate_non_empty_text (value, "system_flags", error))
        return FALSE;

    for (gsize i = 0; valid_system_flags[i] != NULL; i++) {
        if (g_strcmp0 (value, valid_system_flags[i]) == 0)
            return TRUE;
    }

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "message append system_flags contains unsupported system flag");
    return FALSE;
}

static gboolean
validate_user_keyword (const char *value, GError **error)
{
    if (!validate_non_empty_text (value, "user_keywords", error))
        return FALSE;

    if (value[0] == '\\') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message append user_keywords must not contain system flags");
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (!(g_ascii_isalnum (*cursor) || *cursor == '_' || *cursor == '-'
            || *cursor == '.')) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "message append user_keywords contains invalid characters");
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
copy_text_vector (const char *const *values,
    const char *field_name,
    gboolean (*validate_value) (const char *value, GError **error),
    gchar ***out_values, GError **error)
{
    g_auto (GStrv) copy = NULL;

    g_return_val_if_fail (out_values != NULL, FALSE);

    *out_values = NULL;

    if (values == NULL || *values == NULL)
        return TRUE;

    for (gsize i = 0; values[i] != NULL; i++) {
        const char *value = values[i];

        if (!validate_value (value, error))
            return FALSE;

        for (gsize j = 0; j < i; j++) {
            if (g_strcmp0 (values[j], value) == 0) {
                g_set_error (error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "message append %s contains duplicate values",
                    field_name);
                return FALSE;
            }
        }
    }

    copy = g_strdupv ((gchar **)values);
    *out_values = g_steal_pointer (&copy);
    return TRUE;
}

static gboolean
validate_message_bytes (GBytes *message_bytes, GError **error)
{
    gsize size = 0;

    if (message_bytes == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message append message bytes are required");
        return FALSE;
    }

    size = g_bytes_get_size (message_bytes);
    if (size == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message append message bytes must not be empty");
        return FALSE;
    }

    if (size > WYREBOX_DAEMON_MESSAGE_APPEND_MAX_MESSAGE_BYTES) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message append message bytes exceed maximum size of %"
            G_GSIZE_FORMAT " bytes",
            (gsize)WYREBOX_DAEMON_MESSAGE_APPEND_MAX_MESSAGE_BYTES);
        return FALSE;
    }

    return TRUE;
}

void
wyrebox_daemon_message_append_request_clear
    (WyreboxDaemonMessageAppendRequest *request)
{
    if (request == NULL)
        return;

    g_clear_pointer (&request->account_identity, g_free);
    g_clear_pointer (&request->mailbox_id, g_free);
    request->uid_validity = 0;
    g_clear_pointer (&request->message_bytes, g_bytes_unref);
    request->internal_date_unix_us = 0;
    g_strfreev (request->system_flags);
    request->system_flags = NULL;
    g_strfreev (request->user_keywords);
    request->user_keywords = NULL;
}

gboolean
wyrebox_daemon_message_append_request_init
    (WyreboxDaemonMessageAppendRequest *request,
    const char *account_identity, const char *mailbox_id, guint64 uid_validity,
    GBytes *message_bytes, guint64 internal_date_unix_us,
    const char *const *system_flags, const char *const *user_keywords,
    GError **error)
{
    g_auto (WyreboxDaemonMessageAppendRequest) next = { 0 };

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
            "message append uid_validity is required");
        return FALSE;
    }

    if (!validate_message_bytes (message_bytes, error))
        return FALSE;

    if (!copy_text_vector (system_flags, "system_flags", validate_system_flag,
        &next.system_flags, error))
        return FALSE;

    if (!copy_text_vector (user_keywords, "user_keywords",
        validate_user_keyword,
        &next.user_keywords, error))
        return FALSE;

    next.account_identity = g_strdup (account_identity);
    next.mailbox_id = g_strdup (mailbox_id);
    next.uid_validity = uid_validity;
    next.message_bytes = g_bytes_ref (message_bytes);
    next.internal_date_unix_us = internal_date_unix_us;

    wyrebox_daemon_message_append_request_clear (request);
    *request = next;
    next.account_identity = NULL;
    next.mailbox_id = NULL;
    next.message_bytes = NULL;
    next.system_flags = NULL;
    next.user_keywords = NULL;

    return TRUE;
}
