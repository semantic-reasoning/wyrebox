#include "wyrebox-daemon-mail-event.h"
#include "wyrebox-derived-view-membership-changed-payload.h"
#include "wyrebox-flag-changed-payload.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>
#include <string.h>

#define OBJECT_KEY \
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static GBytes *
delivered_payload (const char *account_identity)
{
    g_autoptr (GError) error = NULL;
    const gchar *const recipients[] = {
        "alice@example.com", "bob@example.net", NULL
    };
    GBytes *payload =
        wyrebox_message_delivered_payload_encode_with_identity (OBJECT_KEY,
            321, NULL, 1700000000000000, "delivery-1", "queue-1",
            account_identity, "sender@example.org", recipients, &error);

    g_assert_no_error (error);
    g_assert_nonnull (payload);
    return payload;
}

static GBytes *
flag_payload (const char *account_id, const char *mailbox_id)
{
    g_autoptr (GError) error = NULL;
    char *system_flags[] = { (char *)"\\Seen", (char *)"\\Flagged", NULL };
    char *user_keywords[] = { NULL };
    WyreboxFlagChangedPayload payload = {
        .account_id = (char *)account_id,
        .mailbox_id = (char *)mailbox_id,
        .uidvalidity = 7,
        .uid = 42,
        .mode = WYREBOX_FLAG_CHANGED_MODE_SET,
        .system_flags = system_flags,
        .user_keywords = user_keywords,
    };
    GBytes *bytes = wyrebox_flag_changed_payload_encode (&payload, &error);

    g_assert_no_error (error);
    g_assert_nonnull (bytes);
    return bytes;
}

static GBytes *
derived_view_payload (const char *account_id)
{
    g_autoptr (GError) error = NULL;
    WyreboxDerivedViewMembershipChangedPayload payload = {
        .account_id = (char *)account_id,
        .view_id = (char *)"view-important",
        .message_id = (char *)"message-1",
        .membership_id = (char *)"membership-1",
        .rule_version_hash = (char *)"rule-hash-1",
        .uid = 3,
        .uidvalidity = 9,
        .is_visible = FALSE,
        .materialized_at_unix_us = 10,
    };
    GBytes *bytes =
        wyrebox_derived_view_membership_changed_payload_encode (&payload,
            &error);

    g_assert_no_error (error);
    g_assert_nonnull (bytes);
    return bytes;
}

static char *
project (WyreboxJournalEventType event_type, GBytes *payload,
    const char *account_identity)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) event = NULL;
    WyreboxJournalRecord record = {
        .offset = 128,
        .sequence = 5,
        .event_type = event_type,
        .payload = payload,
    };
    gsize size = 0;
    const char *data = NULL;

    g_assert_true (wyrebox_daemon_mail_event_project (&record,
        account_identity, &event, &error));
    g_assert_no_error (error);
    if (event == NULL)
        return NULL;

    data = g_bytes_get_data (event, &size);
    return g_strndup (data, size);
}

static void
test_mail_event_projects_delivery_without_internal_fields (void)
{
    g_autoptr (GBytes) payload = delivered_payload ("account-1");
    g_autofree char *event =
        project (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
            "account-1");

    g_assert_cmpstr (event, ==,
        "wyrebox-mail-event/1\n"
        "offset=128\n"
        "sequence=5\n"
        "event_type=MessageDelivered\n"
        "account=account-1\n"
        "delivery_id=delivery-1\n"
        "size_bytes=321\n" "internal_date_unix_us=1700000000000000\n");
    g_assert_null (strstr (event, "example"));
    g_assert_null (strstr (event, "sha256"));
}

static void
test_mail_event_skips_other_accounts (void)
{
    g_autoptr (GBytes) delivered = delivered_payload ("account-2");
    g_autoptr (GBytes) flags = flag_payload ("account-2", "inbox:account-2");
    g_autoptr (GBytes) view = derived_view_payload ("account-2");
    g_autofree char *delivered_event = NULL;
    g_autofree char *flag_event = NULL;
    g_autofree char *view_event = NULL;

    delivered_event = project (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
            delivered, "account-1");
    flag_event = project (WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, flags,
            "account-1");
    view_event = project (WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED,
            view, "account-1");
    g_assert_null (delivered_event);
    g_assert_null (flag_event);
    g_assert_null (view_event);
}

static void
test_mail_event_skips_delivery_without_account (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) payload =
        wyrebox_message_delivered_payload_encode (OBJECT_KEY, 10, &error);
    g_autofree char *event = NULL;

    g_assert_no_error (error);
    event = project (WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload,
            "account-1");
    g_assert_null (event);
}

static void
test_mail_event_projects_flag_change_with_escaping (void)
{
    g_autoptr (GBytes) payload = flag_payload ("account-1", "inbox%\n1");
    g_autofree char *event =
        project (WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, payload, "account-1");

    g_assert_cmpstr (event, ==,
        "wyrebox-mail-event/1\n"
        "offset=128\n"
        "sequence=5\n"
        "event_type=FlagChanged\n"
        "account=account-1\n"
        "mailbox_id=inbox%25%0A1\n"
        "uidvalidity=7\n"
        "uid=42\n"
        "mode=set\n" "system_flags=\\Seen \\Flagged\n" "user_keywords=\n");
}

static void
test_mail_event_projects_derived_view_membership (void)
{
    g_autoptr (GBytes) payload = derived_view_payload ("account-1");
    g_autofree char *event =
        project (WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED,
            payload, "account-1");

    g_assert_cmpstr (event, ==,
        "wyrebox-mail-event/1\n"
        "offset=128\n"
        "sequence=5\n"
        "event_type=DerivedViewMembershipChanged\n"
        "account=account-1\n"
        "view_id=view-important\n"
        "uidvalidity=9\n"
        "uid=3\n" "message_id=message-1\n" "is_visible=false\n");
}

static void
test_mail_event_skips_unstreamed_types (void)
{
    static const WyreboxJournalEventType types[] = {
        WYREBOX_JOURNAL_EVENT_KEYWORD_CHANGED,
        WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
        WYREBOX_JOURNAL_EVENT_FACT_RETRACTED,
        WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
    };
    g_autoptr (GBytes) payload = g_bytes_new_static ("account-1", 9);

    for (gsize i = 0; i < G_N_ELEMENTS (types); i++) {
        g_autofree char *event = project (types[i], payload, "account-1");

        g_assert_null (event);
    }
}

static void
test_mail_event_rejects_corrupt_payload (void)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) payload = g_bytes_new_static ("garbage", 7);
    g_autoptr (GBytes) event = NULL;
    WyreboxJournalRecord record = {
        .offset = 0,
        .sequence = 1,
        .event_type = WYREBOX_JOURNAL_EVENT_FLAG_CHANGED,
        .payload = payload,
    };

    g_assert_false (wyrebox_daemon_mail_event_project (&record, "account-1",
        &event, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_null (event);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon-api/mail-event/delivery-without-internal-fields",
        test_mail_event_projects_delivery_without_internal_fields);
    g_test_add_func ("/daemon-api/mail-event/skips-other-accounts",
        test_mail_event_skips_other_accounts);
    g_test_add_func ("/daemon-api/mail-event/skips-delivery-without-account",
        test_mail_event_skips_delivery_without_account);
    g_test_add_func ("/daemon-api/mail-event/flag-change-with-escaping",
        test_mail_event_projects_flag_change_with_escaping);
    g_test_add_func ("/daemon-api/mail-event/derived-view-membership",
        test_mail_event_projects_derived_view_membership);
    g_test_add_func ("/daemon-api/mail-event/skips-unstreamed-types",
        test_mail_event_skips_unstreamed_types);
    g_test_add_func ("/daemon-api/mail-event/rejects-corrupt-payload",
        test_mail_event_rejects_corrupt_payload);

    return g_test_run ();
}
