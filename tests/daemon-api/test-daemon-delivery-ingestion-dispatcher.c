#include "wyrebox-daemon-delivery-ingestion-dispatcher.h"
#include "wyrebox-daemon-delivery-ingestion-request.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, name, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static GBytes *
load_fixture_bytes (const char *fixture_dir, const char *name)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_build_filename (fixture_dir, name, NULL);
  g_autofree char *contents = NULL;
  gsize length = 0;

  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);

  return g_bytes_new_take (g_steal_pointer (&contents), length);
}

static char *
compute_sha256_object_key (GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (checksum, data, size);

  return g_strdup_printf ("sha256:%s", g_checksum_get_string (checksum));
}

static void
assert_bytes_equal (GBytes *actual, GBytes *expected)
{
  gsize actual_size = 0;
  gsize expected_size = 0;
  const guint8 *actual_data = g_bytes_get_data (actual, &actual_size);
  const guint8 *expected_data = g_bytes_get_data (expected, &expected_size);

  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected_data, expected_size);
}

static gboolean
ingest_delivery_success (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-delivery");
  g_assert_cmpstr (identity->caller_identity, ==, "postfix");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "postfix");
  g_assert_cmpstr (request->delivery_id, ==, "delivery-123");
  g_assert_cmpstr (request->recipients[0], ==, "alice@example.com");
  g_assert_cmpuint (g_bytes_get_size (request->message_bytes), ==, 12);

  if (was_called != NULL)
    *was_called = TRUE;

  out_result->object_key =
      g_strdup
      ("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  out_result->size_bytes = 10;
  out_result->journal_offset = 4096;
  out_result->journal_sequence = 7;
  return TRUE;
}

static gboolean
fail_delivery_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;
  (void) identity;
  (void) request;
  (void) out_result;

  if (was_called != NULL)
    *was_called = TRUE;

  return FALSE;
}

static gboolean
ingest_delivery_without_object_key (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;
  (void) identity;
  (void) request;

  if (was_called != NULL)
    *was_called = TRUE;

  out_result->object_key = NULL;
  out_result->size_bytes = 10;
  out_result->journal_offset = 0;
  out_result->journal_sequence = 0;
  return TRUE;
}

static void
test_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_success, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-delivery");
  g_assert_cmpstr (frame.correlation_id, ==, "postfix-ingest-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:4096:7");
}

static void
test_dispatcher_rejects_unauthorized_caller_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_success, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "skill",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (fail_delivery_without_error, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_dispatcher_rejects_invalid_delivery_result (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  g_autoptr (GBytes) message = g_bytes_new_static ("message-bytes", 12);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123", "queue-1", NULL, recipients, message, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_delivery_ingestion_service_new
      (ingest_delivery_without_object_key, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_dispatcher_ingestor_backed_service_ingests_delivery (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-delivery-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-delivery-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) stored = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  gboolean eof = FALSE;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  service =
      wyrebox_daemon_delivery_ingestion_service_new_with_ingestor (ingestor);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123",
          "queue-1", "sender@example.com", recipients, input, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-delivery");
  g_assert_cmpstr (frame.correlation_id, ==, "postfix-ingest-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_cmpuint (record.offset, ==, frame.success.journal_offset);
  g_assert_cmpuint (record.sequence, ==, frame.success.journal_sequence);

  g_assert_true (wyrebox_message_delivered_payload_decode (record.payload,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_nonnull (decoded.object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, g_bytes_get_size (input));
  g_assert_cmpstr (decoded.message_id, ==, "<simple-crlf@example.test>");
  g_assert_cmpstr (decoded.subject, ==, "CRLF fixture");
  g_assert_cmpstr (decoded.from, ==, "Alice <alice@example.test>");
  g_assert_cmpstr (decoded.to, ==, "Bob <bob@example.test>");
  g_assert_cmpstr (decoded.date, ==, "Tue, 02 Jun 2026 12:34:56 +0000");
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 0);

  stored = wyrebox_local_object_store_get_bytes (store, decoded.object_key,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (stored);
  assert_bytes_equal (stored, input);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_dispatcher_ingestor_backed_service_reports_parse_failure (void)
{
  static const char malformed[] =
      "From: Alice <alice@example.test>\r\n"
      "Subject: Missing separator\r\n"
      "Message-ID: <missing-separator@example.test>\r\n"
      "This line is still parsed as a header without CRLFCRLF";
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-delivery-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-delivery-journal-XXXXXX", NULL);
  g_autofree char *expected_key = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input =
      g_bytes_new_static (malformed, sizeof (malformed) - 1);
  g_autoptr (GBytes) stored = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) service = NULL;
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  const char *recipients[] = { "alice@example.com", NULL };
  gboolean eof = FALSE;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  expected_key = compute_sha256_object_key (input);
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  service =
      wyrebox_daemon_delivery_ingestion_service_new_with_ingestor (ingestor);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_delivery_ingestion_request_init (&request,
          "delivery-123",
          "queue-1", "sender@example.com", recipients, input, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_delivery_ingestion_dispatch (service,
          "request-delivery",
          "postfix",
          "account-1",
          "postfix", "postfix-ingest-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-delivery");

  stored = wyrebox_local_object_store_get_bytes (store, expected_key, &error);
  g_assert_null (stored);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error (&error);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (object_root);
  remove_tree (journal_root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "handles-valid-envelope", test_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_dispatcher_converts_silent_failure_to_error_frame);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "rejects-invalid-delivery-result",
      test_dispatcher_rejects_invalid_delivery_result);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "ingestor-backed-service-ingests-delivery",
      test_dispatcher_ingestor_backed_service_ingests_delivery);
  g_test_add_func ("/daemon-api/delivery-ingestion-dispatcher/"
      "ingestor-backed-service-reports-parse-failure",
      test_dispatcher_ingestor_backed_service_reports_parse_failure);

  return g_test_run ();
}
