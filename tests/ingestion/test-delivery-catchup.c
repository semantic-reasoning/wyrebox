#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#define JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"

typedef char *TestDuckdbOwnedString;

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdbFixture;

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

static gchar *
journal_segment_path (const gchar *journal_root)
{
  return g_build_filename (journal_root, JOURNAL_SEGMENT_NAME, NULL);
}

static void
read_journal_segment (const gchar *journal_root,
    guint8 **out_data, gsize *out_size)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *segment_path = journal_segment_path (journal_root);
  g_autofree gchar *contents = NULL;

  g_assert_true (g_file_get_contents (segment_path, &contents, out_size,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (contents);

  *out_data = (guint8 *) g_steal_pointer (&contents);
}

static void
write_journal_segment (const gchar *journal_root, guint8 *data, gsize size)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *segment_path = journal_segment_path (journal_root);

  g_assert_true (g_file_set_contents (segment_path, (const gchar *) data,
          (gssize) size, &error));
  g_assert_no_error (error);
}

static void
truncate_journal_segment (const gchar *journal_root, guint64 size)
{
  g_autofree gchar *segment_path = journal_segment_path (journal_root);

  g_assert_cmpuint (size, <=, G_MAXSSIZE);
  g_assert_cmpint (truncate (segment_path, (off_t) size), ==, 0);
}

static void
corrupt_journal_checksum (const gchar *journal_root, guint64 record_offset)
{
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;

  read_journal_segment (journal_root, &segment, &segment_size);
  g_assert_cmpuint (segment_size, >, record_offset + 32);
  segment[record_offset + 32] ^= 0x01;
  write_journal_segment (journal_root, segment, segment_size);
}

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_owned_string_clear (char **value)
{
  if (value != NULL && *value != NULL)
    duckdb_free (*value);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TestDuckdbOwnedString,
    duckdb_owned_string_clear)
/* *INDENT-ON* */

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
open_duckdb_fixture (const gchar *path, TestDuckdbFixture *fixture)
{
  g_assert_cmpint (duckdb_open (path, &fixture->database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (fixture->database, &fixture->connection),
      ==, DuckDBSuccess);
}

static void
close_duckdb_fixture (TestDuckdbFixture *fixture)
{
  if (fixture->connection != NULL)
    duckdb_disconnect (&fixture->connection);
  if (fixture->database != NULL)
    duckdb_close (&fixture->database);
}

static guint64
query_uint64 (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static gchar *
query_string (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (TestDuckdbOwnedString) value = NULL;

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  value = duckdb_value_varchar (&result, 0, 0);
  return g_strdup (value);
}

static void
assert_table_count (duckdb_connection connection, const gchar *table,
    guint64 expected)
{
  g_autofree gchar *sql = g_strdup_printf ("SELECT COUNT(*) FROM %s;", table);

  g_assert_cmpuint (query_uint64 (connection, sql), ==, expected);
}

static void
assert_materialization_checkpoint (duckdb_connection connection,
    guint64 expected_offset, guint64 expected_sequence)
{
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT COUNT(*) FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, 1);
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT journal_offset FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, expected_offset);
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT journal_sequence FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, expected_sequence);
}

static void
assert_unmaterialized_state (const gchar *catalog_path)
{
  TestDuckdbFixture duckdb = { 0 };

  open_duckdb_fixture (catalog_path, &duckdb);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "message_headers", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "materialization_checkpoint", 0);
  close_duckdb_fixture (&duckdb);
}

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-delivery-catchup-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (), 2,
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);

  return g_steal_pointer (&path);
}

static void
remove_catalog (const gchar *path)
{
  g_autofree gchar *dir = g_path_get_dirname (path);

  remove_tree (dir);
}

static void
ingest_fixture (WyreboxEmlIngestor *ingestor,
    const gchar *fixture_name, WyreboxEmlIngestResult *out_result)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;

  g_assert_true (WYREBOX_IS_EML_INGESTOR (ingestor));
  g_assert_nonnull (fixture_dir);

  input = load_fixture_bytes (fixture_dir, fixture_name);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          out_result, &error));
  g_assert_no_error (error);
}

static WyreboxEmlIngestor *
create_ingestor (const gchar *object_root,
    const gchar *journal_root,
    WyreboxLocalObjectStore **out_object_store,
    WyreboxJournalWriter **out_writer)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  object_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);
  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  ingestor = wyrebox_eml_ingestor_new_with_journal (object_store, writer);
  g_assert_nonnull (ingestor);

  *out_object_store = g_steal_pointer (&object_store);
  *out_writer = g_steal_pointer (&writer);
  return g_steal_pointer (&ingestor);
}

static gboolean
run_catchup (const gchar *catalog_path,
    const gchar *object_root, const gchar *journal_root, GError **error)
{
  g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

  metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
      error);
  if (metadata_store == NULL)
    return FALSE;
  reader = wyrebox_journal_reader_new (journal_root, error);
  if (reader == NULL)
    return FALSE;
  object_store = wyrebox_local_object_store_new (object_root, error);
  if (object_store == NULL)
    return FALSE;
  materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_delivery_catchup_materialize_inbox (metadata_store, reader,
      object_store, materializer, "account-1", error);
}

static void
assert_inbox_state (const gchar *catalog_path,
    guint64 expected_messages,
    guint64 expected_uidnext,
    guint64 expected_checkpoint_offset, guint64 expected_checkpoint_sequence)
{
  TestDuckdbFixture duckdb = { 0 };
  g_autofree gchar *mailbox_name = NULL;

  open_duckdb_fixture (catalog_path, &duckdb);
  assert_table_count (duckdb.connection, "messages", expected_messages);
  assert_table_count (duckdb.connection, "message_headers", expected_messages);
  assert_table_count (duckdb.connection, "mailbox_memberships",
      expected_messages);
  assert_materialization_checkpoint (duckdb.connection,
      expected_checkpoint_offset, expected_checkpoint_sequence);
  mailbox_name = query_string (duckdb.connection,
      "SELECT imap_name FROM mailboxes WHERE mailbox_id = 'mailbox-inbox' "
      "AND account_id = 'account-1';");
  g_assert_cmpstr (mailbox_name, ==, "INBOX");
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, expected_uidnext);
  close_duckdb_fixture (&duckdb);
}

static void
assert_membership_uid (const gchar *catalog_path,
    const WyreboxEmlIngestResult *result, guint64 expected_uid)
{
  TestDuckdbFixture duckdb = { 0 };
  g_autofree gchar *sql = NULL;

  sql =
      g_strdup_printf
      ("SELECT uid FROM mailbox_memberships WHERE journal_offset = %"
      G_GUINT64_FORMAT " AND journal_sequence = %" G_GUINT64_FORMAT ";",
      result->journal_offset, result->journal_sequence);

  open_duckdb_fixture (catalog_path, &duckdb);
  g_assert_cmpuint (query_uint64 (duckdb.connection, sql), ==, expected_uid);
  close_duckdb_fixture (&duckdb);
}

static void
save_wrong_checkpoint (const gchar *catalog_path,
    const WyreboxEmlIngestResult *first_result)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };

  metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (metadata_store);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset =
      first_result->journal_offset;
  metadata.materialization_checkpoint_sequence =
      first_result->journal_sequence + 100;

  g_assert_true (wyrebox_schema_metadata_store_save (metadata_store,
          &metadata, &error));
  g_assert_no_error (error);
}

static void
assert_unsafe_suffix_error (GError *error,
    const gchar *reason,
    guint64 unsafe_offset, guint64 safe_end_offset,
    gboolean has_last_safe_sequence, guint64 last_safe_sequence)
{
  g_autofree gchar *unsafe_offset_fragment =
      g_strdup_printf ("unsafe offset %" G_GUINT64_FORMAT, unsafe_offset);
  g_autofree gchar *safe_end_offset_fragment =
      g_strdup_printf ("safe end offset %" G_GUINT64_FORMAT, safe_end_offset);
  g_autofree gchar *last_safe_sequence_fragment = NULL;

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "journal unsafe suffix found"));
  g_assert_nonnull (strstr (error->message, "stop reason"));
  g_assert_nonnull (strstr (error->message, reason));
  g_assert_nonnull (strstr (error->message, unsafe_offset_fragment));
  g_assert_nonnull (strstr (error->message, safe_end_offset_fragment));

  if (has_last_safe_sequence) {
    last_safe_sequence_fragment =
        g_strdup_printf ("last safe sequence %" G_GUINT64_FORMAT,
        last_safe_sequence);
  } else {
    last_safe_sequence_fragment = g_strdup ("last safe sequence none");
  }
  g_assert_nonnull (strstr (error->message, last_safe_sequence_fragment));
}

static void
test_no_checkpoint_materializes_two_deliveries (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

  g_assert_true (run_catchup (catalog_path, object_root, journal_root, &error));
  g_assert_no_error (error);

  assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
      second.journal_sequence);
  assert_membership_uid (catalog_path, &first, 1);
  assert_membership_uid (catalog_path, &second, 2);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_partial_trailing_record_fails_without_materializing (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };
  g_auto (WyreboxEmlIngestResult) third = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
  ingest_fixture (ingestor, "html-message.eml", &third);
  g_clear_object (&ingestor);
  g_clear_object (&writer);

  truncate_journal_segment (journal_root, third.journal_offset + 17);

  g_assert_false (run_catchup (catalog_path, object_root, journal_root,
          &error));
  assert_unsafe_suffix_error (error, "partial-header", third.journal_offset,
      third.journal_offset, TRUE, second.journal_sequence);
  assert_unmaterialized_state (catalog_path);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_checksum_mismatch_fails_without_materializing (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };
  g_auto (WyreboxEmlIngestResult) third = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
  ingest_fixture (ingestor, "html-message.eml", &third);
  g_clear_object (&ingestor);
  g_clear_object (&writer);

  corrupt_journal_checksum (journal_root, third.journal_offset);

  g_assert_false (run_catchup (catalog_path, object_root, journal_root,
          &error));
  assert_unsafe_suffix_error (error, "checksum-mismatch",
      third.journal_offset, third.journal_offset, TRUE,
      second.journal_sequence);
  assert_unmaterialized_state (catalog_path);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_first_record_corruption_fails_without_materializing (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
  g_clear_object (&ingestor);
  g_clear_object (&writer);

  corrupt_journal_checksum (journal_root, first.journal_offset);

  g_assert_false (run_catchup (catalog_path, object_root, journal_root,
          &error));
  assert_unsafe_suffix_error (error, "checksum-mismatch",
      first.journal_offset, 0, FALSE, 0);
  assert_unmaterialized_state (catalog_path);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_existing_checkpoint_materializes_suffix (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };
  g_auto (WyreboxEmlIngestResult) third = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

  g_assert_true (run_catchup (catalog_path, object_root, journal_root, &error));
  g_assert_no_error (error);
  assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
      second.journal_sequence);

  ingest_fixture (ingestor, "html-message.eml", &third);

  g_assert_true (run_catchup (catalog_path, object_root, journal_root, &error));
  g_assert_no_error (error);

  assert_inbox_state (catalog_path, 3, 4, third.journal_offset,
      third.journal_sequence);
  assert_membership_uid (catalog_path, &first, 1);
  assert_membership_uid (catalog_path, &second, 2);
  assert_membership_uid (catalog_path, &third, 3);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_checkpoint_at_eof_leaves_state_unchanged (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

  g_assert_true (run_catchup (catalog_path, object_root, journal_root, &error));
  g_assert_no_error (error);
  g_assert_true (run_catchup (catalog_path, object_root, journal_root, &error));
  g_assert_no_error (error);

  assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
      second.journal_sequence);
  assert_membership_uid (catalog_path, &first, 1);
  assert_membership_uid (catalog_path, &second, 2);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_bad_checkpoint_seek_fails_without_materializing (void)
{
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first = { 0 };
  g_auto (WyreboxEmlIngestResult) second = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root,
      &ingest_object_store, &writer);
  ingest_fixture (ingestor, "simple-crlf.eml", &first);
  ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
  save_wrong_checkpoint (catalog_path, &first);

  g_assert_false (run_catchup (catalog_path, object_root, journal_root,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  open_duckdb_fixture (catalog_path, &duckdb);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  assert_materialization_checkpoint (duckdb.connection, first.journal_offset,
      first.journal_sequence + 100);
  close_duckdb_fixture (&duckdb);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/delivery-catchup/no-checkpoint",
      test_no_checkpoint_materializes_two_deliveries);
  g_test_add_func ("/ingestion/delivery-catchup/partial-trailing-record",
      test_partial_trailing_record_fails_without_materializing);
  g_test_add_func ("/ingestion/delivery-catchup/checksum-mismatch",
      test_checksum_mismatch_fails_without_materializing);
  g_test_add_func ("/ingestion/delivery-catchup/first-record-corruption",
      test_first_record_corruption_fails_without_materializing);
  g_test_add_func ("/ingestion/delivery-catchup/existing-checkpoint",
      test_existing_checkpoint_materializes_suffix);
  g_test_add_func ("/ingestion/delivery-catchup/checkpoint-at-eof",
      test_checkpoint_at_eof_leaves_state_unchanged);
  g_test_add_func ("/ingestion/delivery-catchup/bad-checkpoint-seek",
      test_bad_checkpoint_seek_fails_without_materializing);

  return g_test_run ();
}
