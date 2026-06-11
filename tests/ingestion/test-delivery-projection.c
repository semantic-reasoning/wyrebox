#include "wyrebox-delivery-projection.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

static const char *missing_object_key =
    "sha256:"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

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
assert_delivery_record_fields (const WyreboxDeliveryProjectionRecord *record,
    const char *object_key, guint64 sequence, guint64 size_bytes,
    const char *message_id, const char *subject, const char *from,
    const char *to, const char *date_raw, guint duplicate_message_id_count)
{
  g_assert_nonnull (record);
  g_assert_cmpuint (record->journal_sequence, ==, sequence);
  g_assert_cmpstr (record->object_key, ==, object_key);
  g_assert_cmpuint (record->size_bytes, ==, size_bytes);
  g_assert_cmpstr (record->rfc_message_id, ==, message_id);
  g_assert_cmpstr (record->subject, ==, subject);
  g_assert_cmpstr (record->from, ==, from);
  g_assert_cmpstr (record->to, ==, to);
  g_assert_cmpstr (record->date_raw, ==, date_raw);
  g_assert_cmpuint (record->duplicate_message_id_count, ==,
      duplicate_message_id_count);
}

static void
test_replays_two_deliveries_after_reopen (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) first_input = NULL;
  g_autoptr (GBytes) second_input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };
  g_autoptr (WyreboxLocalObjectStore) reopened_store = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  first_input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  second_input = load_fixture_bytes (fixture_dir, "duplicate-message-id.eml");

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, first_input,
          &first_result, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (first_result.journal_sequence, ==, 1);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, second_input,
          &second_result, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (second_result.journal_sequence, ==, 2);
  g_assert_cmpstr (first_result.object_key, !=, second_result.object_key);

  g_clear_object (&store);
  g_clear_object (&writer);
  g_clear_object (&ingestor);

  reopened_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reopened_store);
  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, reopened_store);
  g_assert_nonnull (projection);

  g_assert_true (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_no_error (error);

  g_assert_nonnull (list.records);
  g_assert_cmpuint (list.records->len, ==, 2);

  WyreboxDeliveryProjectionRecord *first = g_ptr_array_index (list.records, 0);
  WyreboxDeliveryProjectionRecord *second = g_ptr_array_index (list.records, 1);

  g_assert_cmpuint (first->journal_sequence, ==, 1);
  g_assert_cmpstr (first->object_key, ==, first_result.object_key);
  g_assert_cmpuint (second->journal_sequence, ==, 2);
  g_assert_cmpstr (second->object_key, ==, second_result.object_key);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_replay_preserves_duplicate_raw_object_deliveries (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };
  g_autoptr (WyreboxLocalObjectStore) reopened_store = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };

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

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &first_result, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          &second_result, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (first_result.journal_sequence, <,
      second_result.journal_sequence);
  g_assert_cmpuint (first_result.journal_offset, <,
      second_result.journal_offset);
  g_assert_cmpstr (first_result.object_key, ==, second_result.object_key);
  g_assert_cmpuint (first_result.size_bytes, ==, second_result.size_bytes);

  g_clear_object (&store);
  g_clear_object (&writer);
  g_clear_object (&ingestor);

  reopened_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reopened_store);
  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, reopened_store);
  g_assert_nonnull (projection);

  g_assert_true (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (list.records);
  g_assert_cmpuint (list.records->len, ==, 2);

  WyreboxDeliveryProjectionRecord *first = g_ptr_array_index (list.records, 0);
  WyreboxDeliveryProjectionRecord *second = g_ptr_array_index (list.records, 1);

  g_assert_cmpuint (first->journal_sequence, ==, first_result.journal_sequence);
  g_assert_cmpuint (first->journal_offset, ==, first_result.journal_offset);
  g_assert_cmpstr (first->object_key, ==, first_result.object_key);
  g_assert_cmpuint (first->size_bytes, ==, first_result.size_bytes);
  g_assert_cmpuint (second->journal_sequence, ==,
      second_result.journal_sequence);
  g_assert_cmpuint (second->journal_offset, ==, second_result.journal_offset);
  g_assert_cmpstr (second->object_key, ==, second_result.object_key);
  g_assert_cmpuint (second->size_bytes, ==, second_result.size_bytes);
  g_assert_cmpuint (first->journal_sequence, <, second->journal_sequence);
  g_assert_cmpuint (first->journal_offset, <, second->journal_offset);
  g_assert_cmpstr (first->object_key, ==, second->object_key);
  g_assert_cmpuint (first->size_bytes, ==, second->size_bytes);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_replay_preserves_metadata (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "duplicate-message-id.eml");

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input, &result,
          &error));
  g_assert_no_error (error);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, store);
  g_assert_nonnull (projection);

  g_assert_true (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (list.records);
  g_assert_cmpuint (list.records->len, ==, 1);

  WyreboxDeliveryProjectionRecord *record = g_ptr_array_index (list.records, 0);
  assert_delivery_record_fields (record,
      result.object_key,
      1,
      result.size_bytes,
      "<duplicate-message-id@example.test>",
      "Duplicate Message-ID fixture",
      "Duplicate ID <duplicate@example.test>",
      "Intake <intake@example.test>", "Mon, 08 Jun 2026 14:05:00 +0000", 1);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_mixed_stream_ignores_non_message_records (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) first_input = NULL;
  g_autoptr (GBytes) second_input = NULL;
  g_autoptr (GBytes) ignored_payload = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };
  guint64 ignored_offset = 0;
  guint64 ignored_sequence = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  first_input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  second_input = load_fixture_bytes (fixture_dir, "missing-message-id.eml");
  ignored_payload =
      g_bytes_new_static ("ignore this event", sizeof ("ignore this event")
      - 1);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, first_input,
          &first_result, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_KEYWORD_CHANGED,
          ignored_payload, &ignored_offset, &ignored_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (ignored_sequence, ==, 2);

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, second_input,
          &second_result, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (second_result.journal_sequence, ==, 3);
  g_assert_cmpuint (ignored_offset, >, first_result.journal_offset);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, store);
  g_assert_nonnull (projection);

  g_assert_true (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (list.records);
  g_assert_cmpuint (list.records->len, ==, 2);

  WyreboxDeliveryProjectionRecord *first = g_ptr_array_index (list.records, 0);
  WyreboxDeliveryProjectionRecord *second = g_ptr_array_index (list.records, 1);

  g_assert_cmpuint (first->journal_sequence, ==, 1);
  g_assert_cmpstr (first->object_key, ==, first_result.object_key);
  g_assert_cmpuint (second->journal_sequence, ==, 3);
  g_assert_cmpstr (second->object_key, ==, second_result.object_key);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_malformed_payload_fails_projection (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) malformed = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };
  guint64 ignored_offset = 0;
  guint64 ignored_sequence = 0;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  malformed = g_bytes_new_static ("not-a-message-delivered-payload",
      sizeof ("not-a-message-delivered-payload") - 1);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          malformed, &ignored_offset, &ignored_sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (ignored_sequence, ==, 1);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, store);
  g_assert_nonnull (projection);

  g_assert_false (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (g_strstr_len (error->message, -1, "sequence 1"));
  g_assert_null (list.records);

  g_clear_error (&error);
  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_missing_object_reference_fails_projection (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  payload = wyrebox_message_delivered_payload_encode (missing_object_key, 123,
      &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (sequence, ==, 1);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, store);
  g_assert_nonnull (projection);

  g_assert_false (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (g_strstr_len (error->message, -1, missing_object_key));
  g_assert_nonnull (g_strstr_len (error->message, -1, "sequence 1"));
  g_assert_null (list.records);

  g_clear_error (&error);
  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_later_failure_clears_partial_projection (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-projection-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) missing_payload = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryProjection) projection = NULL;
  g_auto (WyreboxDeliveryProjectionList) list = { 0 };
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  guint64 offset = 0;
  guint64 sequence = 0;

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
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input, &result,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (result.journal_sequence, ==, 1);

  missing_payload =
      wyrebox_message_delivered_payload_encode (missing_object_key, 123,
      &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          missing_payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (sequence, ==, 2);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  projection = wyrebox_delivery_projection_new (reader, store);
  g_assert_nonnull (projection);

  g_assert_false (wyrebox_delivery_projection_replay_all (projection, &list,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (g_strstr_len (error->message, -1, "sequence 2"));
  g_assert_null (list.records);

  g_clear_error (&error);
  remove_tree (object_root);
  remove_tree (journal_root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/delivery-projection/two-record-replay",
      test_replays_two_deliveries_after_reopen);
  g_test_add_func ("/ingestion/delivery-projection/"
      "duplicate-raw-object-replay",
      test_replay_preserves_duplicate_raw_object_deliveries);
  g_test_add_func ("/ingestion/delivery-projection/metadata-preserved",
      test_replay_preserves_metadata);
  g_test_add_func ("/ingestion/delivery-projection/"
      "non-message-records-are-ignored",
      test_mixed_stream_ignores_non_message_records);
  g_test_add_func ("/ingestion/delivery-projection/"
      "malformed-payload-fails-projection",
      test_malformed_payload_fails_projection);
  g_test_add_func ("/ingestion/delivery-projection/"
      "missing-object-reference-fails-projection",
      test_missing_object_reference_fails_projection);
  g_test_add_func ("/ingestion/delivery-projection/"
      "later-failure-clears-partial-projection",
      test_later_failure_clears_partial_projection);

  return g_test_run ();
}
