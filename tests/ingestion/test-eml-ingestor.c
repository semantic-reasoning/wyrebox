#include "wyrebox-eml-ingestor.h"
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
          &result, &error));
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

  g_assert_true (wyrebox_message_delivered_payload_decode (record.payload,
          &decoded, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (decoded.object_key, ==, result.object_key);
  g_assert_cmpuint (decoded.size_bytes, ==, result.size_bytes);
  g_assert_cmpuint (decoded.internal_date_unix_us, ==, 0);
  g_assert_cmpstr (decoded.message_id, ==, "<simple-crlf@example.test>");
  g_assert_cmpstr (decoded.subject, ==, "CRLF fixture");
  g_assert_cmpstr (decoded.from, ==, "Alice <alice@example.test>");
  g_assert_cmpstr (decoded.to, ==, "Bob <bob@example.test>");
  g_assert_cmpstr (decoded.date, ==, "Tue, 02 Jun 2026 12:34:56 +0000");
  g_assert_cmpuint (decoded.duplicate_message_id_count, ==, 0);

  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

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
      "journaled-rejects-missing-header-separator",
      test_journaled_ingest_rejects_missing_header_separator);
  g_test_add_func ("/ingestion/eml-ingestor/"
      "no-journal-preserves-raw-malformed-bytes",
      test_no_journal_ingest_preserves_raw_malformed_bytes);

  return g_test_run ();
}
