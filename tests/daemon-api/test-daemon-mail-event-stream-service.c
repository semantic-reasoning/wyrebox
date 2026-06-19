#include "wyrebox-daemon-mail-event-stream-service.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-journal-writer.h"

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
  const char payload[] = "offset=42\nsequence=7\nevent_type=MessageDelivered\n";
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
  g_assert_cmpstr (response.stream_chunk.request_id, ==, "request-mail-event");
  g_assert_cmpstr (response.stream_chunk.query_id, ==, "mail-event-stream");
  g_assert_cmpstr (response.stream_chunk.correlation_id, ==, "corr-1");
  g_assert_false (response.stream_chunk.message_id != NULL);
  g_assert_true (response.stream_chunk.end_of_stream);
  {
    const char expected[] =
        "offset=42\nsequence=7\nevent_type=MessageDelivered\n";
    g_autoptr (GBytes) expected_bytes =
        g_bytes_new_static (expected, sizeof expected - 1);

    g_assert_true (g_bytes_equal (response.stream_chunk.bytes, expected_bytes));
  }
}

static void
test_mail_event_stream_service_reads_journal (void)
{
  g_autofree char *journal_root_dir = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonMailEventStreamService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;
  const char payload_bytes[] = "journal-message";
  const char *tmpdir = g_get_tmp_dir ();

  journal_root_dir = g_build_filename (tmpdir, "wyrebox-mail-event-XXXXXX",
      NULL);
  journal_root_dir = g_mkdtemp (journal_root_dir);
  g_assert_nonnull (journal_root_dir);

  writer = wyrebox_journal_writer_new (journal_root_dir, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  payload = g_bytes_new_static (payload_bytes, sizeof payload_bytes - 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  service = wyrebox_daemon_mail_event_stream_service_new_from_journal_root
      (journal_root_dir, &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-mail-event", "dovecot", "account-1", "dovecot-storage",
          "corr-2", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", NULL, NULL, "MessageDelivered", 0, 0, 0, 0, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_mail_event_stream_service_handle_identity
      (service, &identity, &request, &response, &error));
  g_assert_no_error (error);
  g_assert_cmpint (response.kind, ==,
      WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (response.stream_chunk.query_id, ==, "mail-event-stream");
  g_assert_true (g_bytes_equal (response.stream_chunk.bytes, payload));
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
  g_test_add_func ("/daemon/mail-event-stream-service/reads-journal",
      test_mail_event_stream_service_reads_journal);
  g_test_add_func ("/daemon/mail-event-stream-service/denies-unknown-clients",
      test_mail_event_stream_service_denies_unknown_clients);

  return g_test_run ();
}
