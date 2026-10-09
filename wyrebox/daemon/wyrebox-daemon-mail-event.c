#include "wyrebox-daemon-mail-event.h"

#include "wyrebox-derived-view-membership-changed-payload.h"
#include "wyrebox-flag-changed-payload.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-message-delivered-payload.h"

static void
append_value (GString *event, const char *value)
{
    for (const guchar *cursor = (const guchar *)value; *cursor != '\0';
        cursor++) {
        if (*cursor == '%' || *cursor < 0x20 || *cursor == 0x7f)
            g_string_append_printf (event, "%%%02X", *cursor);
        else
            g_string_append_c (event, (char)*cursor);
    }
}

static void
append_field (GString *event, const char *key, const char *value)
{
    g_string_append (event, key);
    g_string_append_c (event, '=');
    append_value (event, value);
    g_string_append_c (event, '\n');
}

static void
append_uint64_field (GString *event, const char *key, guint64 value)
{
    g_string_append_printf (event, "%s=%" G_GUINT64_FORMAT "\n", key, value);
}

static void
append_strv_field (GString *event, const char *key, char **values)
{
    g_string_append (event, key);
    g_string_append_c (event, '=');
    for (gsize i = 0; values != NULL && values[i] != NULL; i++) {
        if (i > 0)
            g_string_append_c (event, ' ');
        append_value (event, values[i]);
    }
    g_string_append_c (event, '\n');
}

static GString *
new_event (const WyreboxJournalRecord *record, const char *account)
{
    GString *event = g_string_new (WYREBOX_DAEMON_MAIL_EVENT_MAGIC "\n");

    append_uint64_field (event, "offset", record->offset);
    append_uint64_field (event, "sequence", record->sequence);
    append_field (event, "event_type",
        wyrebox_journal_event_type_to_string (record->event_type));
    append_field (event, "account", account);
    return event;
}

static GBytes *
finish_event (GString *event)
{
    gsize len = event->len;

    return g_bytes_new_take (g_string_free (event, FALSE), len);
}

static gboolean
project_delivery (const WyreboxJournalRecord *record,
    const char *account_identity, GBytes **out_event, GError **error)
{
    g_auto (WyreboxMessageDeliveredPayload) payload = { 0 };
    GString *event = NULL;

    if (!wyrebox_message_delivered_payload_decode (record->payload, &payload,
        error))
        return FALSE;

    if (g_strcmp0 (payload.account_identity, account_identity) != 0)
        return TRUE;

    event = new_event (record, payload.account_identity);
    if (payload.delivery_id != NULL)
        append_field (event, "delivery_id", payload.delivery_id);
    append_uint64_field (event, "size_bytes", payload.size_bytes);
    append_uint64_field (event, "internal_date_unix_us",
        payload.internal_date_unix_us);
    *out_event = finish_event (event);
    return TRUE;
}

static const char *
flag_mode_name (WyreboxFlagChangedMode mode)
{
    switch (mode) {
    case WYREBOX_FLAG_CHANGED_MODE_SET:
        return "set";
    case WYREBOX_FLAG_CHANGED_MODE_CLEAR:
        return "clear";
    case WYREBOX_FLAG_CHANGED_MODE_REPLACE:
        return "replace";
    default:
        g_assert_not_reached ();
    }
}

static gboolean
project_flag_change (const WyreboxJournalRecord *record,
    const char *account_identity, GBytes **out_event, GError **error)
{
    g_auto (WyreboxFlagChangedPayload) payload = { 0 };
    GString *event = NULL;

    if (!wyrebox_flag_changed_payload_decode (record->payload, &payload,
        error))
        return FALSE;

    if (g_strcmp0 (payload.account_id, account_identity) != 0)
        return TRUE;

    event = new_event (record, payload.account_id);
    append_field (event, "mailbox_id", payload.mailbox_id);
    append_uint64_field (event, "uidvalidity", payload.uidvalidity);
    append_uint64_field (event, "uid", payload.uid);
    append_field (event, "mode", flag_mode_name (payload.mode));
    append_strv_field (event, "system_flags", payload.system_flags);
    append_strv_field (event, "user_keywords", payload.user_keywords);
    *out_event = finish_event (event);
    return TRUE;
}

static gboolean
project_derived_view_membership (const WyreboxJournalRecord *record,
    const char *account_identity, GBytes **out_event, GError **error)
{
    g_auto (WyreboxDerivedViewMembershipChangedPayload) payload = { 0 };
    GString *event = NULL;

    if (!wyrebox_derived_view_membership_changed_payload_decode
            (record->payload, &payload, error))
        return FALSE;

    if (g_strcmp0 (payload.account_id, account_identity) != 0)
        return TRUE;

    event = new_event (record, payload.account_id);
    append_field (event, "view_id", payload.view_id);
    append_uint64_field (event, "uidvalidity", payload.uidvalidity);
    append_uint64_field (event, "uid", payload.uid);
    append_field (event, "message_id", payload.message_id);
    append_field (event, "is_visible", payload.is_visible ? "true" : "false");
    *out_event = finish_event (event);
    return TRUE;
}

gboolean
wyrebox_daemon_mail_event_project (const WyreboxJournalRecord *record,
    const char *account_identity, GBytes **out_event, GError **error)
{
    g_autoptr (GError) local_error = NULL;
    gboolean ok = TRUE;

    g_return_val_if_fail (record != NULL, FALSE);
    g_return_val_if_fail (account_identity != NULL, FALSE);
    g_return_val_if_fail (out_event != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    *out_event = NULL;

    switch (record->event_type) {
    case WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED:
        ok = project_delivery (record, account_identity, out_event,
                &local_error);
        break;
    case WYREBOX_JOURNAL_EVENT_FLAG_CHANGED:
        ok = project_flag_change (record, account_identity, out_event,
                &local_error);
        break;
    case WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED:
        ok = project_derived_view_membership (record, account_identity,
                out_event, &local_error);
        break;
    default:
        return TRUE;
    }

    if (!ok) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "mail event %s record at offset %" G_GUINT64_FORMAT
            " does not decode: %s",
            wyrebox_journal_event_type_to_string (record->event_type),
            record->offset, local_error != NULL ? local_error->message :
            "unknown error");
        return FALSE;
    }

    return TRUE;
}
