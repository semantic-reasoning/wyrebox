#include "wyrebox-delivery-replay-validator.h"
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

static char *
object_path_for_key (const char *root_dir, const char *object_key)
{
  const char *hex = object_key + strlen ("sha256:");
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (root_dir, "objects", "sha256", prefix, filename,
      NULL);
}

static void
overwrite_object (const char *object_root, const char *object_key,
    const guint8 *data, gsize size, GError **error)
{
  g_autofree char *path = object_path_for_key (object_root, object_key);

  g_assert_true (g_file_set_contents (path, (const gchar *) data,
          (gssize) size, error));
}

static void
test_journaled_ingest_validates_delivered_object_reference (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-replay-validator-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-replay-validator-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };

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
          &result, &error));
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  validator = wyrebox_delivery_replay_validator_new (reader, store);
  g_assert_nonnull (validator);
  g_assert_true (wyrebox_delivery_replay_validator_validate_all (validator,
          &error));
  g_assert_no_error (error);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_message_delivered_missing_object_fails_validation (void)
{
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-replay-validator-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-replay-validator-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
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

  payload = wyrebox_message_delivered_payload_encode (missing_object_key,
      123, &error);
  g_assert_no_error (error);
  g_assert_nonnull (payload);

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  validator = wyrebox_delivery_replay_validator_new (reader, store);
  g_assert_nonnull (validator);

  g_assert_false (wyrebox_delivery_replay_validator_validate_all (validator,
          &error));
  g_assert_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
      WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_MISSING_OBJECT);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_message_delivered_size_mismatch_fails_validation (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-replay-validator-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-replay-validator-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
  g_autoptr (GBytes) mismatched_payload = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  gsize input_size = 0;
  guint64 offset = 0;
  guint64 sequence = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  (void) g_bytes_get_data (input, &input_size);
  g_assert_cmpuint (input_size, >, 0);

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

  mismatched_payload = wyrebox_message_delivered_payload_encode
      (result.object_key, input_size - 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (mismatched_payload);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED,
          mismatched_payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (sequence, ==, 2);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  validator = wyrebox_delivery_replay_validator_new (reader, store);
  g_assert_nonnull (validator);
  g_assert_false (wyrebox_delivery_replay_validator_validate_all (validator,
          &error));
  g_assert_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
      WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_SIZE_MISMATCH);
  g_assert_nonnull (g_strstr_len (error->message, -1, "sequence 2"));
  g_assert_nonnull (g_strstr_len (error->message, -1, "size"));
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_message_delivered_hash_mismatch_fails_validation (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-replay-validator-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-replay-validator-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  gsize input_size = 0;
  const guint8 *input_data = NULL;
  g_autofree guint8 *corrupted = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  input_data = g_bytes_get_data (input, &input_size);
  g_assert_cmpuint (input_size, >, 0);
  corrupted = g_memdup2 (input_data, input_size);
  g_assert_nonnull (corrupted);
  corrupted[0] ^= 0xFF;

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

  overwrite_object (object_root, result.object_key, corrupted, input_size,
      &error);
  g_assert_no_error (error);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  validator = wyrebox_delivery_replay_validator_new (reader, store);
  g_assert_nonnull (validator);
  g_assert_false (wyrebox_delivery_replay_validator_validate_all (validator,
          &error));
  g_assert_error (error, WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR,
      WYREBOX_DELIVERY_REPLAY_VALIDATOR_ERROR_HASH_MISMATCH);
  g_assert_nonnull (g_strstr_len (error->message, -1, "sequence 1"));
  g_assert_nonnull (g_strstr_len (error->message, -1, "SHA-256"));
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
}

static void
test_non_message_delivered_records_are_skipped (void)
{
  static const char ignored_payload[] = "not a MessageDelivered payload";
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-replay-validator-objects-XXXXXX", NULL);
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-replay-validator-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) payload =
      g_bytes_new_static (ignored_payload, sizeof (ignored_payload) - 1);
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
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

  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_FLAG_CHANGED,
          payload, &offset, &sequence, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (offset, ==, 0);
  g_assert_cmpuint (sequence, ==, 1);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  validator = wyrebox_delivery_replay_validator_new (reader, store);
  g_assert_nonnull (validator);
  g_assert_true (wyrebox_delivery_replay_validator_validate_all (validator,
          &error));
  g_assert_no_error (error);

  remove_tree (object_root);
  remove_tree (journal_root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/delivery-replay-validator/"
      "journaled-ingest-validates-delivered-object-reference",
      test_journaled_ingest_validates_delivered_object_reference);
  g_test_add_func ("/ingestion/delivery-replay-validator/"
      "message-delivered-missing-object-fails-validation",
      test_message_delivered_missing_object_fails_validation);
  g_test_add_func ("/ingestion/delivery-replay-validator/"
      "message-delivered-size-mismatch-fails-validation",
      test_message_delivered_size_mismatch_fails_validation);
  g_test_add_func ("/ingestion/delivery-replay-validator/"
      "message-delivered-hash-mismatch-fails-validation",
      test_message_delivered_hash_mismatch_fails_validation);
  g_test_add_func ("/ingestion/delivery-replay-validator/"
      "non-message-delivered-records-are-skipped",
      test_non_message_delivered_records_are_skipped);

  return g_test_run ();
}
