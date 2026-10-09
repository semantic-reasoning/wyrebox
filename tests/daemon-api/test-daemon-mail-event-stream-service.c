#include "wyrebox-daemon-mail-event-stream-service.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-flag-changed-payload.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>
#include <string.h>

typedef struct
{
    gboolean called;
} Fixture;

static gboolean
stream_event_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailEventStreamRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
    Fixture *fixture = user_data;
    const char payload[] =
        "offset=42\nsequence=7\nevent_type=MessageDelivered\n";
    g_autoptr (GBytes) bytes = g_bytes_new_static (payload, sizeof payload - 1);

    g_assert_cmpstr (identity->request_id, ==, "request-mail-event");
    g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
    g_assert_cmpstr (identity->account_identity, ==, "account-1");
    g_assert_cmpstr (request->account_identity, ==, "account-1");
    g_assert_cmpstr (request->mailbox_identity, ==, "mailbox-inbox");
    g_assert_cmpstr (request->event_type, ==, "MessageDelivered");
    g_assert_cmpuint (request->after_journal_offset, ==, 41);
    g_assert_cmpuint (request->after_journal_sequence, ==, 6);
    g_assert_cmpuint (request->after_unix_us, ==, 1000);
    g_assert_cmpuint (request->before_unix_us, ==, 2000);

    fixture->called = TRUE;
    return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
               identity->request_id, NULL, "mail-event-stream",
               identity->correlation_id, 0, bytes, TRUE, error);
}

static void
test_mail_event_stream_service_handles_identity (void)
{
    g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) response = { 0 };
    g_autoptr (GError) error = NULL;
    Fixture fixture = { FALSE };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-mail-event", "dovecot", "account-1", "dovecot-storage",
        "corr-1", &error));
    g_assert_no_error (error);

    g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
        "account-1", "mailbox-inbox", NULL, "MessageDelivered", 41, 6,
        1000, 2000, &error));
    g_assert_no_error (error);

    service = wyrebox_daemon_mail_event_stream_service_new
            (stream_event_fixture, &fixture, NULL);
    g_assert_nonnull (service);

    g_assert_true (wyrebox_daemon_mail_event_stream_service_handle_identity
            (service, &identity, &request, &response, &error));
    g_assert_no_error (error);
    g_assert_true (fixture.called);
    g_assert_cmpint (response.kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_cmpstr (response.stream_chunk.request_id, ==,
        "request-mail-event");
    g_assert_cmpstr (response.stream_chunk.query_id, ==, "mail-event-stream");
    g_assert_cmpstr (response.stream_chunk.correlation_id, ==, "corr-1");
    g_assert_false (response.stream_chunk.message_id != NULL);
    g_assert_true (response.stream_chunk.end_of_stream);
    {
        const char expected[] =
            "offset=42\nsequence=7\nevent_type=MessageDelivered\n";
        g_autoptr (GBytes) expected_bytes =
            g_bytes_new_static (expected, sizeof expected - 1);

        g_assert_true (g_bytes_equal (response.stream_chunk.bytes,
            expected_bytes));
    }
}

#define OBJECT_KEY \
        "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct
{
    char *journal_root_dir;
    WyreboxJournalWriter *writer;
    guint64 offsets[4];
    guint64 sequences[4];
} JournalFixture;

static void
journal_fixture_clear (JournalFixture *fixture)
{
    g_clear_object (&fixture->writer);
    g_clear_pointer (&fixture->journal_root_dir, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (JournalFixture, journal_fixture_clear)

static void
append_record (JournalFixture *fixture, guint index,
    WyreboxJournalEventType event_type, GBytes *payload)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_journal_writer_append (fixture->writer, event_type,
        payload, &fixture->offsets[index], &fixture->sequences[index],
        &error));
    g_assert_no_error (error);
}

static GBytes *
delivered_payload (const char *account_identity, const char *delivery_id)
{
    g_autoptr (GError) error = NULL;
    const gchar *const recipients[] = { "alice@example.com", NULL };
    GBytes *payload =
        wyrebox_message_delivered_payload_encode_with_identity (OBJECT_KEY,
            10, NULL, 0, delivery_id, NULL, account_identity, NULL, recipients,
            &error);

    g_assert_no_error (error);
    return payload;
}

/*
 * Journals, in order: a delivery for account-1, a delivery for account-2,
 * a flag change for account-1 and an audit record.
 */
static void
journal_fixture_init (JournalFixture *fixture)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) delivered_1 = delivered_payload ("account-1", "d-1");
    g_autoptr (GBytes) delivered_2 = delivered_payload ("account-2", "d-2");
    g_autoptr (GBytes) flags = NULL;
    g_autoptr (GBytes) audit = g_bytes_new_static ("account-1", 9);
    char *system_flags[] = { (char *)"\\Seen", NULL };
    WyreboxFlagChangedPayload flag_payload = {
        .account_id = (char *)"account-1",
        .mailbox_id = (char *)"inbox:account-1",
        .uidvalidity = 1,
        .uid = 1,
        .mode = WYREBOX_FLAG_CHANGED_MODE_SET,
        .system_flags = system_flags,
    };

    fixture->journal_root_dir =
        g_dir_make_tmp ("wyrebox-mail-event-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->writer = wyrebox_journal_writer_new (fixture->journal_root_dir,
            &error);
    g_assert_no_error (error);

    flags = wyrebox_flag_changed_payload_encode (&flag_payload, &error);
    g_assert_no_error (error);

    append_record (fixture, 0, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
        delivered_1);
    append_record (fixture, 1, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
        delivered_2);
    append_record (fixture, 2, WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, flags);
    append_record (fixture, 3, WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
        audit);
}

static gboolean
read_event (WyreboxDaemonMailEventStreamService *service,
    const char *account_identity, guint64 after_offset, guint64 after_sequence,
    WyreboxDaemonResponseFrame *out_response, GError **error)
{
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };
    g_autoptr (GError) local_error = NULL;

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-mail-event", "dovecot", account_identity, "dovecot-storage",
        "corr-2", &local_error));
    g_assert_no_error (local_error);
    g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
        account_identity, NULL, NULL, NULL, after_offset, after_sequence, 0, 0,
        &local_error));
    g_assert_no_error (local_error);

    return wyrebox_daemon_mail_event_stream_service_handle_identity (service,
               &identity, &request, out_response, error);
}

static char *
event_text (const WyreboxDaemonResponseFrame *response)
{
    gsize size = 0;
    const char *data = NULL;

    g_assert_cmpint (response->kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_cmpstr (response->stream_chunk.query_id, ==,
        "mail-event-stream");
    g_assert_false (response->stream_chunk.end_of_stream);
    data = g_bytes_get_data (response->stream_chunk.bytes, &size);
    return g_strndup (data, size);
}

static void
assert_end_of_stream (const WyreboxDaemonResponseFrame *response)
{
    g_assert_cmpint (response->kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_true (response->stream_chunk.end_of_stream);
    g_assert_cmpuint (response->stream_chunk.chunk_index, ==, 0);
    g_assert_true (response->stream_chunk.bytes == NULL ||
        g_bytes_get_size (response->stream_chunk.bytes) == 0);
}

static void
test_mail_event_stream_service_walks_account_events (void)
{
    g_auto (JournalFixture) fixture = { 0 };
    g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
    g_autoptr (GError) error = NULL;

    journal_fixture_init (&fixture);
    service = wyrebox_daemon_mail_event_stream_service_new_from_journal_writer
            (fixture.journal_root_dir, fixture.writer, &error);
    g_assert_no_error (error);

    {
        g_auto (WyreboxDaemonResponseFrame) response = { 0 };
        g_autofree char *event = NULL;
        g_autofree char *expected = NULL;

        g_assert_true (read_event (service, "account-1", 0, 0, &response,
            &error));
        g_assert_no_error (error);
        event = event_text (&response);
        g_assert_cmpuint (response.stream_chunk.chunk_index, ==,
            fixture.offsets[0]);
        expected = g_strdup_printf ("wyrebox-mail-event/1\n"
                "offset=%" G_GUINT64_FORMAT "\n"
                "sequence=%" G_GUINT64_FORMAT "\n"
                "event_type=MessageDelivered\n"
                "account=account-1\n"
                "delivery_id=d-1\n"
                "size_bytes=10\n"
                "internal_date_unix_us=0\n", fixture.offsets[0],
                fixture.sequences[0]);
        g_assert_cmpstr (event, ==, expected);
    }
    {
        g_auto (WyreboxDaemonResponseFrame) response = { 0 };
        g_autofree char *event = NULL;

        g_assert_true (read_event (service, "account-1", fixture.offsets[0],
            fixture.sequences[0], &response, &error));
        g_assert_no_error (error);
        event = event_text (&response);
        g_assert_cmpuint (response.stream_chunk.chunk_index, ==,
            fixture.offsets[2]);
        g_assert_nonnull (strstr (event, "event_type=FlagChanged\n"));
    }
    {
        g_auto (WyreboxDaemonResponseFrame) response = { 0 };

        g_assert_true (read_event (service, "account-1", fixture.offsets[2],
            fixture.sequences[2], &response, &error));
        g_assert_no_error (error);
        assert_end_of_stream (&response);
    }
    {
        g_auto (WyreboxDaemonResponseFrame) response = { 0 };
        g_autofree char *event = NULL;

        g_assert_true (read_event (service, "account-2", 0, 0, &response,
            &error));
        g_assert_no_error (error);
        event = event_text (&response);
        g_assert_cmpuint (response.stream_chunk.chunk_index, ==,
            fixture.offsets[1]);
        g_assert_nonnull (strstr (event, "delivery_id=d-2\n"));
    }
}

static void
test_mail_event_stream_service_reads_from_journal_root (void)
{
    g_auto (JournalFixture) fixture = { 0 };
    g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
    g_auto (WyreboxDaemonResponseFrame) response = { 0 };
    g_autoptr (GError) error = NULL;

    journal_fixture_init (&fixture);
    service = wyrebox_daemon_mail_event_stream_service_new_from_journal_root
            (fixture.journal_root_dir, &error);
    g_assert_no_error (error);

    g_assert_true (read_event (service, "account-2", fixture.offsets[1],
        fixture.sequences[1], &response, &error));
    g_assert_no_error (error);
    assert_end_of_stream (&response);
}

static void
test_mail_event_stream_service_rejects_corrupt_record (void)
{
    g_auto (JournalFixture) fixture = { 0 };
    g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
    g_auto (WyreboxDaemonResponseFrame) response = { 0 };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) garbage = g_bytes_new_static ("garbage", 7);
    guint64 offset = 0;
    guint64 sequence = 0;

    journal_fixture_init (&fixture);
    g_assert_true (wyrebox_journal_writer_append (fixture.writer,
        WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, garbage, &offset, &sequence,
        &error));
    g_assert_no_error (error);
    service = wyrebox_daemon_mail_event_stream_service_new_from_journal_writer
            (fixture.journal_root_dir, fixture.writer, &error);
    g_assert_no_error (error);

    g_assert_false (read_event (service, "account-1", fixture.offsets[2],
        fixture.sequences[2], &response, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_mail_event_stream_service_denies_unknown_clients (void)
{
    g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
    g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
    g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) response = { 0 };
    g_autoptr (GError) error = NULL;
    Fixture fixture = { FALSE };

    g_assert_true (wyrebox_daemon_request_identity_init (&identity,
        "request-mail-event", "unknown-tool", "account-1", NULL, NULL,
        &error));
    g_assert_no_error (error);

    g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
        "account-1", NULL, NULL, NULL, 0, 0, 0, 0, &error));
    g_assert_no_error (error);

    service = wyrebox_daemon_mail_event_stream_service_new
            (stream_event_fixture, &fixture, NULL);
    g_assert_nonnull (service);

    g_assert_false (wyrebox_daemon_mail_event_stream_service_handle_identity
            (service, &identity, &request, &response, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_false (fixture.called);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon/mail-event-stream-service/handles-identity",
        test_mail_event_stream_service_handles_identity);
    g_test_add_func ("/daemon/mail-event-stream-service/walks-account-events",
        test_mail_event_stream_service_walks_account_events);
    g_test_add_func ("/daemon/mail-event-stream-service/reads-journal-root",
        test_mail_event_stream_service_reads_from_journal_root);
    g_test_add_func ("/daemon/mail-event-stream-service/rejects-corrupt-record",
        test_mail_event_stream_service_rejects_corrupt_record);
    g_test_add_func ("/daemon/mail-event-stream-service/denies-unknown-clients",
        test_mail_event_stream_service_denies_unknown_clients);

    return g_test_run ();
}
