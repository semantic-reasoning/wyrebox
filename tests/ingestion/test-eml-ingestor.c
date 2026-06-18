#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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

static char *
compute_sha256_object_key (GBytes *bytes)
{
  gsize size = 0;
  const guint8 *data = g_bytes_get_data (bytes, &size);
  g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

  g_checksum_update (checksum, data, size);

  return g_strdup_printf ("sha256:%s", g_checksum_get_string (checksum));
}

static guint
count_message_delivered_records (const char *journal_root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  guint count = 0;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  while (TRUE) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
      g_assert_no_error (error);
      g_assert_true (eof);
      return count;
    }

    if (record.event_type == WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED)
      count++;
  }
}

typedef struct
{
  WyreboxJournalWriter *writer;
  const char *object_key;
  guint64 size_bytes;
  const char *delivery_id;
  const char *queue_id;
  const char *account_identity;
  const char *envelope_sender;
  const gchar *const *recipients;
  GMutex mutex;
  GCond cond;
  gboolean entered;
  gboolean release;
  gboolean success;
  GError *error;
  guint64 journal_offset;
  guint64 journal_sequence;
} GuardedAppendThreadContext;

typedef struct
{
  WyreboxEmlIngestor *ingestor;
  GBytes *bytes;
  const char *delivery_id;
  const char *queue_id;
  const char *account_identity;
  const char *envelope_sender;
  const gchar *const *recipients;
  WyreboxEmlIngestResult result;
  gboolean success;
  GError *error;
} DeliveryIngestThreadContext;

static gboolean
append_message_delivered_after_release (const char *journal_root_dir,
    gpointer user_data, GBytes **out_payload, guint64 *out_offset,
    guint64 *out_sequence, GError **error)
{
  GuardedAppendThreadContext *context = user_data;
  g_autoptr (GBytes) payload = NULL;

  (void) journal_root_dir;
  (void) out_offset;
  (void) out_sequence;

  payload = wyrebox_message_delivered_payload_encode_with_identity
      (context->object_key, context->size_bytes, NULL, 123,
      context->delivery_id, context->queue_id, context->account_identity,
      context->envelope_sender, context->recipients, error);
  if (payload == NULL)
    return FALSE;

  g_mutex_lock (&context->mutex);
  context->entered = TRUE;
  g_cond_broadcast (&context->cond);
  while (!context->release)
    g_cond_wait (&context->cond, &context->mutex);
  g_mutex_unlock (&context->mutex);

  *out_payload = g_steal_pointer (&payload);
  return TRUE;
}

static gpointer
guarded_append_thread (gpointer user_data)
{
  GuardedAppendThreadContext *context = user_data;

  context->success = wyrebox_journal_writer_append_guarded (context->writer,
      WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
      append_message_delivered_after_release, context,
      &context->journal_offset, &context->journal_sequence, &context->error);

  return NULL;
}

static gpointer
delivery_ingest_thread (gpointer user_data)
{
  DeliveryIngestThreadContext *context = user_data;

  context->success = wyrebox_eml_ingestor_ingest_delivery_bytes
      (context->ingestor, context->bytes, context->delivery_id,
      context->queue_id, context->account_identity, context->envelope_sender,
      context->recipients, &context->result, &context->error);

  return NULL;
}

static void
test_ingests_raw_fixture_into_object_store (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *root = g_dir_make_tmp ("wyrebox-eml-ingestor-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) output = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  gsize input_size = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  input_size = g_bytes_get_size (input);
  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  ingestor = wyrebox_eml_ingestor_new (store);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &result, &error));
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);
  g_assert_true (g_regex_match_simple ("^sha256:[0-9a-f]{64}$",
          result.object_key, 0, 0));
  g_assert_cmpuint (result.size_bytes, ==, input_size);
  g_assert_cmpuint (result.journal_offset, ==, 0);
  g_assert_cmpuint (result.journal_sequence, ==, 0);

  output = wyrebox_local_object_store_get_bytes (store, result.object_key,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, input);

  remove_tree (root);
}

static void
test_ingests_raw_fixture_and_appends_message_delivered_journal (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) output = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) decoded = { 0 };
  gboolean eof = FALSE;
  gint64 before_ingest_us = 0;
  gint64 after_ingest_us = 0;
  gsize input_size = 0;
  const char *input_data = NULL;
  const char *message_id = NULL;
  const char *subject = NULL;
  guint64 message_id_span_start = 0;
  guint64 message_id_span_end = 0;
  guint64 subject_span_start = 0;
  guint64 subject_span_end = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  input_size = g_bytes_get_size (input);
  input_data = g_bytes_get_data (input, &input_size);
  message_id = g_strstr_len (input_data, input_size,
      "Message-ID: <simple-crlf@example.test>\r\n");
  g_assert_nonnull (message_id);
  subject = g_strstr_len (input_data, input_size, "Subject: CRLF fixture\r\n");
  g_assert_nonnull (subject);
  message_id_span_start = (guint64) (message_id - input_data);
  message_id_span_end = message_id_span_start +
      (guint64) strlen ("Message-ID: <simple-crlf@example.test>");
  subject_span_start = (guint64) (subject - input_data);
  subject_span_end = subject_span_start +
      (guint64) strlen ("Subject: CRLF fixture");
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);

  before_ingest_us = g_get_real_time ();
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &result, &error));
  after_ingest_us = g_get_real_time ();
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);
  g_assert_true (g_regex_match_simple ("^sha256:[0-9a-f]{64}$",
          result.object_key, 0, 0));
  g_assert_cmpuint (result.size_bytes, ==, input_size);
  g_assert_cmpuint (result.journal_offset, ==, 0);
  g_assert_cmpuint (result.journal_sequence, ==, 1);

  output = wyrebox_local_object_store_get_bytes (store, result.object_key,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, input);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_cmpuint (record.offset, ==, result.journal_offset);
  g_assert_cmpuint (record.sequence, ==, result.journal_sequence);
  g_assert_cmpmem (g_bytes_get_data (record.payload, NULL), 8, "WYREMDP4", 8);

  g_assert_true (wyrebox_message_delivered_payload_decode (record.payload,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, result.object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, result.size_bytes);
  g_assert_cmpuint (decoded.internal_date_unix_us, >=,
      (guint64) before_ingest_us);
  g_assert_cmpuint (decoded.internal_date_unix_us, <=,
      (guint64) after_ingest_us);
  g_assert_cmpstr (decoded.message_id, ==, "<simple-crlf@example.test>");
  g_assert_cmpstr (decoded.subject, ==, "CRLF fixture");
  g_assert_cmpstr (decoded.from, ==, "Alice <alice@example.test>");
  g_assert_cmpstr (decoded.to, ==, "Bob <bob@example.test>");
  g_assert_cmpstr (decoded.date, ==, "Tue, 02 Jun 2026 12:34:56 +0000");
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 0);
  g_assert_true (decoded.message_id_span_valid);
  g_assert_cmpuint (decoded.message_id_span_start, ==, message_id_span_start);
  g_assert_cmpuint (decoded.message_id_span_end, ==, message_id_span_end);
  g_assert_true (decoded.subject_span_valid);
  g_assert_cmpuint (decoded.subject_span_start, ==, subject_span_start);
  g_assert_cmpuint (decoded.subject_span_end, ==, subject_span_end);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_duplicate_journaled_ingest_appends_distinct_deliveries (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };
  g_auto (WyreboxJournalRecord) first_record = { 0 };
  g_auto (WyreboxJournalRecord) second_record = { 0 };
  g_auto (WyreboxJournalRecord) eof_record = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) first_decoded = { 0 };
  g_auto (WyreboxMessageDeliveredPayload) second_decoded = { 0 };
  gboolean eof = FALSE;
  gsize input_size = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  input_size = g_bytes_get_size (input);
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &first_result, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &second_result, &error));
  g_assert_no_error (error);

  g_assert_nonnull (first_result.object_key);
  g_assert_nonnull (second_result.object_key);
  g_assert_cmpstr (second_result.object_key, ==, first_result.object_key);
  g_assert_cmpuint (first_result.size_bytes, ==, input_size);
  g_assert_cmpuint (second_result.size_bytes, ==, input_size);
  g_assert_cmpuint (first_result.journal_sequence, <,
      second_result.journal_sequence);
  g_assert_cmpuint (first_result.journal_offset, <,
      second_result.journal_offset);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &first_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &second_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);

  g_assert_cmpint (first_record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_cmpint (second_record.event_type,
      ==, WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED);
  g_assert_cmpuint (first_record.offset, ==, first_result.journal_offset);
  g_assert_cmpuint (second_record.offset, ==, second_result.journal_offset);
  g_assert_cmpuint (first_record.sequence, ==, first_result.journal_sequence);
  g_assert_cmpuint (second_record.sequence, ==, second_result.journal_sequence);
  g_assert_cmpuint (first_record.sequence, <, second_record.sequence);

  g_assert_true (wyrebox_message_delivered_payload_decode (first_record.payload,
          &first_decoded, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_message_delivered_payload_decode
      (second_record.payload, &second_decoded, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (first_decoded.object_key, ==, first_result.object_key);
  g_assert_cmpstr (second_decoded.object_key, ==, first_result.object_key);
  g_assert_cmpuint (first_decoded.size_bytes, ==, first_result.size_bytes);
  g_assert_cmpuint (second_decoded.size_bytes, ==, first_result.size_bytes);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &eof_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_identical_delivery_retry_returns_original_receipt (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) retry_result = { 0 };
  const char *recipients[] = { "alice@example.test", "bob@example.test",
    NULL
  };

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

  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", NULL, "account-1", "sender@example.test",
          recipients, &first_result, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "", "account-1", "sender@example.test",
          recipients, &retry_result, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (retry_result.object_key, ==, first_result.object_key);
  g_assert_cmpuint (retry_result.size_bytes, ==, first_result.size_bytes);
  g_assert_cmpuint (retry_result.journal_offset, ==,
      first_result.journal_offset);
  g_assert_cmpuint (retry_result.journal_sequence, ==,
      first_result.journal_sequence);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_delivery_retry_after_writer_reopen_returns_original_receipt (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) retry_result = { 0 };
  const char *recipients[] = { "alice@example.test", NULL };

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

  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-1", "sender@example.test",
          recipients, &first_result, &error));
  g_assert_no_error (error);

  g_clear_object (&ingestor);
  g_clear_object (&writer);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-1", "sender@example.test",
          recipients, &retry_result, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (retry_result.object_key, ==, first_result.object_key);
  g_assert_cmpuint (retry_result.size_bytes, ==, first_result.size_bytes);
  g_assert_cmpuint (retry_result.journal_offset, ==,
      first_result.journal_offset);
  g_assert_cmpuint (retry_result.journal_sequence, ==,
      first_result.journal_sequence);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_delivery_retry_conflict_fails_without_append (void)
{
  static const char changed_message[] =
      "From: Alice <alice@example.test>\r\n"
      "To: Bob <bob@example.test>\r\n"
      "Subject: Changed fixture\r\n"
      "Message-ID: <changed@example.test>\r\n"
      "Date: Tue, 02 Jun 2026 12:34:56 +0000\r\n" "\r\n" "Changed body\r\n";
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) changed_input =
      g_bytes_new_static (changed_message, sizeof (changed_message) - 1);
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) conflict_result = { 0 };
  const char *recipients[] = { "alice@example.test", NULL };
  const char *changed_recipients[] = { "bob@example.test", NULL };

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

  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-1", "sender@example.test",
          recipients, &first_result, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor,
          changed_input, "delivery-123", "queue-1", "account-1",
          "sender@example.test", recipients, &conflict_result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_assert_null (conflict_result.object_key);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  g_assert_false (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-2", "sender@example.test",
          recipients, &conflict_result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_assert_null (conflict_result.object_key);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  g_assert_false (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-1", "sender@example.test",
          changed_recipients, &conflict_result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_assert_null (conflict_result.object_key);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_same_bytes_different_delivery_id_appends_distinct_delivery (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };
  const char *recipients[] = { "alice@example.test", NULL };

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

  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-123", "queue-1", "account-1", "sender@example.test",
          recipients, &first_result, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
          "delivery-456", "queue-1", "account-1", "sender@example.test",
          recipients, &second_result, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (second_result.object_key, ==, first_result.object_key);
  g_assert_cmpuint (second_result.size_bytes, ==, first_result.size_bytes);
  g_assert_cmpuint (first_result.journal_sequence, <,
      second_result.journal_sequence);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 2);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_delivery_retry_scan_and_append_are_guarded (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autofree char *object_key = NULL;
  g_autofree char *stored_object_key = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (GThread) holder_thread = NULL;
  g_autoptr (GThread) contender_thread = NULL;
  GuardedAppendThreadContext holder = { 0 };
  DeliveryIngestThreadContext contender = { 0 };
  const char *recipients[] = { "alice@example.test", NULL };

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  object_key = compute_sha256_object_key (input);
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_local_object_store_put_bytes (store, input,
          &stored_object_key, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (stored_object_key, ==, object_key);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);

  g_mutex_init (&holder.mutex);
  g_cond_init (&holder.cond);
  holder.writer = writer;
  holder.object_key = object_key;
  holder.size_bytes = (guint64) g_bytes_get_size (input);
  holder.delivery_id = "delivery-123";
  holder.queue_id = "queue-1";
  holder.account_identity = "account-1";
  holder.envelope_sender = "sender@example.test";
  holder.recipients = recipients;

  holder_thread = g_thread_new ("guarded-append-holder",
      guarded_append_thread, &holder);

  g_mutex_lock (&holder.mutex);
  while (!holder.entered)
    g_cond_wait (&holder.cond, &holder.mutex);
  g_mutex_unlock (&holder.mutex);

  contender.ingestor = ingestor;
  contender.bytes = input;
  contender.delivery_id = "delivery-123";
  contender.queue_id = "queue-1";
  contender.account_identity = "account-1";
  contender.envelope_sender = "sender@example.test";
  contender.recipients = recipients;
  contender_thread = g_thread_new ("guarded-append-contender",
      delivery_ingest_thread, &contender);

  g_usleep (200000);

  g_mutex_lock (&holder.mutex);
  holder.release = TRUE;
  g_cond_broadcast (&holder.cond);
  g_mutex_unlock (&holder.mutex);

  g_thread_join (g_steal_pointer (&holder_thread));
  g_thread_join (g_steal_pointer (&contender_thread));

  g_assert_true (holder.success);
  g_assert_no_error (holder.error);
  g_assert_true (contender.success);
  g_assert_no_error (contender.error);
  g_assert_cmpstr (contender.result.object_key, ==, object_key);
  g_assert_cmpuint (contender.result.size_bytes, ==,
      (guint64) g_bytes_get_size (input));
  g_assert_cmpuint (contender.result.journal_offset, ==, holder.journal_offset);
  g_assert_cmpuint (contender.result.journal_sequence, ==,
      holder.journal_sequence);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==, 1);

  wyrebox_eml_ingest_result_clear (&contender.result);
  g_clear_error (&holder.error);
  g_clear_error (&contender.error);
  g_cond_clear (&holder.cond);
  g_mutex_clear (&holder.mutex);
  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_journaled_ingest_rejects_missing_header_separator (void)
{
  static const char malformed[] =
      "From: Alice <alice@example.test>\r\n"
      "Subject: Missing separator\r\n"
      "Message-ID: <missing-separator@example.test>\r\n"
      "This line is still parsed as a header without CRLFCRLF";
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-eml-ingestor-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input =
      g_bytes_new_static (malformed, sizeof (malformed) - 1);
  g_autoptr (GBytes) output = NULL;
  g_autofree char *expected_key = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  expected_key = compute_sha256_object_key (input);

  g_assert_false (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &result, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_null (result.object_key);
  g_assert_cmpuint (result.size_bytes, ==, 0);
  g_assert_cmpuint (result.journal_offset, ==, 0);
  g_assert_cmpuint (result.journal_sequence, ==, 0);

  output = wyrebox_local_object_store_get_bytes (store, expected_key, &error);
  g_assert_null (output);
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

static void
test_no_journal_ingest_preserves_raw_malformed_bytes (void)
{
  static const char malformed[] =
      "From: Alice <alice@example.test>\r\n"
      "Subject: Missing separator\r\n"
      "Message-ID: <missing-separator@example.test>\r\n"
      "This object-store-only path does not parse metadata";
  g_autofree char *root = g_dir_make_tmp ("wyrebox-eml-ingestor-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input =
      g_bytes_new_static (malformed, sizeof (malformed) - 1);
  g_autoptr (GBytes) output = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };

  g_assert_nonnull (root);

  store = wyrebox_local_object_store_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  ingestor = wyrebox_eml_ingestor_new (store);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &result, &error));
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);
  g_assert_cmpuint (result.size_bytes, ==, g_bytes_get_size (input));
  g_assert_cmpuint (result.journal_offset, ==, 0);
  g_assert_cmpuint (result.journal_sequence, ==, 0);

  output = wyrebox_local_object_store_get_bytes (store, result.object_key,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (output);
  assert_bytes_equal (output, input);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/eml-ingestor/raw-fixture-into-object-store",
      test_ingests_raw_fixture_into_object_store);
  g_test_add_func ("/ingestion/eml-ingestor/message-delivered-journal",
      test_ingests_raw_fixture_and_appends_message_delivered_journal);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "duplicate-journaled-ingest-appends-distinct-deliveries",
      test_duplicate_journaled_ingest_appends_distinct_deliveries);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "identical-delivery-retry-returns-original-receipt",
      test_identical_delivery_retry_returns_original_receipt);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "delivery-retry-after-writer-reopen-returns-original-receipt",
      test_delivery_retry_after_writer_reopen_returns_original_receipt);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "delivery-retry-conflict-fails-without-append",
      test_delivery_retry_conflict_fails_without_append);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "same-bytes-different-delivery-id-appends-distinct-delivery",
      test_same_bytes_different_delivery_id_appends_distinct_delivery);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "delivery-retry-scan-and-append-are-guarded",
      test_delivery_retry_scan_and_append_are_guarded);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "journaled-rejects-missing-header-separator",
      test_journaled_ingest_rejects_missing_header_separator);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "no-journal-preserves-raw-malformed-bytes",
      test_no_journal_ingest_preserves_raw_malformed_bytes);

  return g_test_run ();
}
