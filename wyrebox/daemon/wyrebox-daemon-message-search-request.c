#include "wyrebox-daemon-message-search-request.h"

#include <gio/gio.h>
#include <string.h>

static gboolean
validate_text (const char *value, const char *field_name, gsize max_length,
    GError **error)
{
    if (value == NULL || *value == '\0') {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH %s is required", field_name);
        return FALSE;
    }

    if (max_length > 0 && strlen (value) > max_length) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH %s exceeds %" G_GSIZE_FORMAT " bytes", field_name,
            max_length);
        return FALSE;
    }

    if (!g_utf8_validate (value, -1, NULL)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH %s must be valid UTF-8", field_name);
        return FALSE;
    }

    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (g_ascii_iscntrl (*cursor)) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "message SEARCH %s must not contain control characters",
                field_name);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
validate_namespace_kind (WyreboxDaemonMailboxListEntryKind namespace_kind,
    GError **error)
{
    switch (namespace_kind) {
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY:
    case WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL:
        return TRUE;
    default:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH namespace_kind is unknown");
        return FALSE;
    }
}

static gboolean
validate_criterion (const WyreboxDaemonMessageSearchCriterion *criterion,
    GError **error)
{
    switch (criterion->kind) {
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS:
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_FROM_CONTAINS:
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENDER_DOMAIN:
        return validate_text (criterion->text, "criterion text",
                   WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_TEXT_LENGTH, error);
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE:
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_BEFORE:
        if (criterion->text != NULL) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_INVALID_ARGUMENT,
                "message SEARCH date criterion must not carry text");
            return FALSE;
        }
        return TRUE;
    default:
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH criterion kind is unknown");
        return FALSE;
    }
}

static void
clear_criteria (WyreboxDaemonMessageSearchCriterion *criteria,
    guint n_criteria)
{
    for (guint i = 0; i < n_criteria; i++)
        g_free (criteria[i].text);
    g_free (criteria);
}

void
wyrebox_daemon_message_search_request_clear (WyreboxDaemonMessageSearchRequest
    *request)
{
    if (request == NULL)
        return;

    g_clear_pointer (&request->account_identity, g_free);
    g_clear_pointer (&request->mailbox_id, g_free);
    clear_criteria (request->criteria, request->n_criteria);
    request->criteria = NULL;
    request->n_criteria = 0;
    request->namespace_kind = WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
    request->uid_validity = 0;
}

gboolean
wyrebox_daemon_message_search_request_init (WyreboxDaemonMessageSearchRequest
    *request, const char *account_identity, const char *mailbox_id,
    WyreboxDaemonMailboxListEntryKind namespace_kind, guint64 uid_validity,
    const WyreboxDaemonMessageSearchCriterion *criteria, guint n_criteria,
    GError **error)
{
    g_auto (WyreboxDaemonMessageSearchRequest) next = { 0 };

    g_return_val_if_fail (request != NULL, FALSE);
    g_return_val_if_fail (criteria != NULL || n_criteria == 0, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!validate_text (account_identity, "account_identity", 0, error))
        return FALSE;

    if (!validate_text (mailbox_id, "mailbox_id", 0, error))
        return FALSE;

    if (!validate_namespace_kind (namespace_kind, error))
        return FALSE;

    if (uid_validity == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH uid_validity is required");
        return FALSE;
    }

    if (n_criteria > WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "message SEARCH accepts at most %d criteria",
            WYREBOX_DAEMON_MESSAGE_SEARCH_MAX_CRITERIA);
        return FALSE;
    }

    for (guint i = 0; i < n_criteria; i++) {
        if (!validate_criterion (&criteria[i], error))
            return FALSE;
    }

    next.account_identity = g_strdup (account_identity);
    next.mailbox_id = g_strdup (mailbox_id);
    next.namespace_kind = namespace_kind;
    next.uid_validity = uid_validity;
    if (n_criteria > 0) {
        next.criteria = g_new0 (WyreboxDaemonMessageSearchCriterion,
                n_criteria);
        next.n_criteria = n_criteria;
        for (guint i = 0; i < n_criteria; i++) {
            next.criteria[i].kind = criteria[i].kind;
            next.criteria[i].text = g_strdup (criteria[i].text);
            next.criteria[i].unix_us = criteria[i].unix_us;
        }
    }

    wyrebox_daemon_message_search_request_clear (request);
    *request = next;
    memset (&next, 0, sizeof (next));

    return TRUE;
}
