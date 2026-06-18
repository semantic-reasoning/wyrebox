#include "wyrebox-daemon-runtime.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdbFixture;

static char *
create_unprepared_catalog_path (void)
{
  g_autofree char *dir = NULL;
  g_autoptr (GError) error = NULL;

  dir = g_dir_make_tmp ("wyrebox-daemon-runtime-catalog-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return g_build_filename (dir, "catalog.duckdb", NULL);
}

static char *
create_journal_root (void)
{
  g_autofree char *dir = NULL;
  g_autoptr (GError) error = NULL;

  dir = g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return g_steal_pointer (&dir);
}

static void
append_runtime_journal_record (const char *journal_root,
    guint64 *out_offset, guint64 *out_sequence)
{
  static const guint8 payload[] = { 0x72, 0x75, 0x6e, 0x74, 0x69, 0x6d,
    0x65
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload_bytes,
          out_offset, out_sequence, &error));
  g_assert_no_error (error);
}

static char *
create_catalog_path (void)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autofree char *journal_root = create_journal_root ();

  dir = g_dir_make_tmp ("wyrebox-daemon-runtime-catalog-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  migration = wyrebox_schema_migration_new ();
  g_assert_true (wyrebox_schema_migration_run_store_to_current_with_journal
      (migration, store, reader, FALSE, &error));
  g_assert_no_error (error);

  return g_steal_pointer (&path);
}

static void
    runtime_set_materialization_checkpoint_fields
    (WyreboxSchemaMigrationMetadataState * state, guint64 journal_offset,
    guint64 journal_sequence)
{
  state->materialization_checkpoint_present = TRUE;
  state->materialization_checkpoint_journal_offset = journal_offset;
  state->materialization_checkpoint_sequence = journal_sequence;
}

static void
save_catalog_schema_state (const char *path,
    const WyreboxSchemaMigrationMetadataState *state)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_save (store, state, &error));
  g_assert_no_error (error);
}

static void
load_catalog_schema_state (const char *path,
    WyreboxSchemaMigrationMetadataState *out_state)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, out_state, &error));
  g_assert_no_error (error);
}

static void
open_duckdb_fixture (const char *path, TestDuckdbFixture *fixture)
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

static void
execute_sql (duckdb_connection connection, const char *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
}

static guint64
query_uint64 (duckdb_connection connection, const char *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static void
seed_two_account_catalog (const char *catalog_path)
{
  TestDuckdbFixture fixture = { 0 };

  open_duckdb_fixture (catalog_path, &fixture);
  execute_sql (fixture.connection,
      "INSERT INTO accounts (account_id) VALUES "
      "('account-2'), ('account-1');");
  execute_sql (fixture.connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('mail-1', 'account-1', 'object-1', 1, 1),"
      "('mail-2', 'account-1', 'object-2', 2, 2),"
      "('mail-3', 'account-2', 'object-3', 3, 3);");
  close_duckdb_fixture (&fixture);
}

static guint64
query_catalog_uint64 (const char *catalog_path, const char *sql)
{
  TestDuckdbFixture fixture = { 0 };
  guint64 value = 0;

  open_duckdb_fixture (catalog_path, &fixture);
  value = query_uint64 (fixture.connection, sql);
  close_duckdb_fixture (&fixture);

  return value;
}

static void
remove_catalog_path (const char *catalog_path)
{
  g_autofree char *catalog_dir = g_path_get_dirname (catalog_path);

  remove_tree (catalog_dir);
}

static char *
object_path_for_key (const char *root_dir, const char *object_key)
{
  const char *hex = object_key + strlen ("sha256:");
  g_autofree char *prefix = g_strndup (hex, 2);
  g_autofree char *filename = g_strdup_printf ("%s.eml", hex);

  return g_build_filename (root_dir,
      "objects", "sha256", prefix, filename, NULL);
}

static char *
journal_segment_path (const char *journal_root)
{
  return g_build_filename (journal_root, "00000000000000000000.wbj", NULL);
}

static void
corrupt_journal_segment_checksum (const char *journal_root,
    guint64 record_offset)
{
  g_autofree char *segment_path = journal_segment_path (journal_root);
  g_autofree gchar *contents = NULL;
  gsize length = 0;

  g_assert_true (g_file_get_contents (segment_path, &contents, &length, NULL));
  g_assert_cmpuint (length, >, record_offset + 32);
  contents[record_offset + 32] ^= 0x01;
  g_assert_true (g_file_set_contents (segment_path, contents, (gssize) length,
          NULL));
}

static void
ingest_runtime_preflight_message (const char *journal_root,
    const char *object_root, WyreboxEmlIngestResult *out_result)
{
  static const guint8 message[] =
      "From: sender@example.test\r\n"
      "To: recipient@example.test\r\n"
      "Subject: Runtime preflight\r\n"
      "Message-ID: <runtime-preflight@example.test>\r\n"
      "Date: Fri, 12 Jun 2026 10:00:00 +0000\r\n" "\r\n" "body\r\n";
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = g_bytes_new_static (message, sizeof (message) - 1);
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
          out_result, &error));
  g_assert_no_error (error);
}

static void
append_fact_journal_record (WyreboxJournalWriter *writer,
    const char *scope_id, const char *message_id)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_keyword", scope_id,
          (const char *[]) { message_id, "view-projects", NULL }, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal
      (&mutation, writer, &journal_offset, &journal_sequence, &error));
  g_assert_no_error (error);
}

static void
test_runtime_validate_delivery_storage_accepts_valid_delivery (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  ingest_runtime_preflight_message (journal_root, object_root, &result);

  g_assert_true (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_no_error (error);

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_accepts_missing_journal (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_no_error (error);

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_rejects_uninitialized_object_root (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_autofree char *objects_dir = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  objects_dir = g_build_filename (object_root, "objects", "sha256", NULL);
  g_assert_false (g_file_test (objects_dir, G_FILE_TEST_EXISTS));

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_false (g_file_test (objects_dir, G_FILE_TEST_EXISTS));

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_rejects_missing_object (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autofree char *object_path = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  ingest_runtime_preflight_message (journal_root, object_root, &result);
  object_path = object_path_for_key (object_root, result.object_key);
  g_assert_cmpint (g_remove (object_path), ==, 0);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "startup delivery storage"));
  g_assert_nonnull (strstr (error->message, result.object_key));

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_rejects_corrupt_object (void)
{
  static const char corrupt[] = "corrupt object bytes";
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autofree char *object_path = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  ingest_runtime_preflight_message (journal_root, object_root, &result);
  object_path = object_path_for_key (object_root, result.object_key);
  g_assert_true (g_file_set_contents (object_path, corrupt, strlen (corrupt),
          &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "startup delivery storage"));
  g_assert_nonnull (strstr (error->message, "SHA-256"));

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_rejects_unsafe_journal_suffix (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-journal-XXXXXX", NULL);
  g_autofree char *object_root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-objects-XXXXXX", NULL);
  g_auto (WyreboxEmlIngestResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  g_autofree char *segment_path = NULL;

  g_assert_nonnull (journal_root);
  g_assert_nonnull (object_root);
  ingest_runtime_preflight_message (journal_root, object_root, &result);
  segment_path = journal_segment_path (journal_root);
  g_assert_cmpint (truncate (segment_path, 17), ==, 0);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      (journal_root, object_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "unsafe suffix"));
  g_assert_nonnull (strstr (error->message, "startup delivery storage"));

  remove_tree (journal_root);
  remove_tree (object_root);
}

static void
test_runtime_validate_delivery_storage_rejects_invalid_args (void)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      (NULL, "/tmp", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "journal root"));
  g_clear_error (&error);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      ("", "/tmp", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "journal root"));
  g_clear_error (&error);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      ("/tmp", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "object root"));
  g_clear_error (&error);

  g_assert_false (wyrebox_daemon_runtime_validate_delivery_storage
      ("/tmp", "", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "object root"));
}

static void
assert_select_projects (const char *catalog_path,
    const char *account_id,
    guint64 expected_message_count,
    guint64 expected_uid_next, guint64 *out_uid_validity)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (catalog_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (catalog);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          account_id, NULL, "Projects", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_no_error (error);
  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_id, ==, "view-projects");
  g_assert_cmpstr (result.mailbox_name, ==, "Projects");
  g_assert_cmpuint (result.message_count, ==, expected_message_count);
  g_assert_cmpuint (result.uid_next, ==, expected_uid_next);
  g_assert_cmpuint (result.uid_validity, !=, 0);

  if (out_uid_validity != NULL)
    *out_uid_validity = result.uid_validity;
}

static void
test_default_socket_path_matches_linux_runtime_contract (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_socket_path (),
      ==, "/run/wyrebox/wyrebox.sock");
}

static void
test_default_socket_dir_is_run_wyrebox (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_runtime_dir (),
      ==, "/run/wyrebox");
}

static void
test_default_fact_dump_dir_is_under_run_wyrebox (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_fact_dump_dir (),
      ==, "/run/wyrebox/facts");
}

static void
test_default_fact_dump_file_is_owned_gfile (void)
{
  g_autoptr (GFile) first = NULL;
  g_autoptr (GFile) second = NULL;
  g_autofree char *first_path = NULL;
  g_autofree char *second_path = NULL;

  first = wyrebox_daemon_runtime_get_default_fact_dump_file ();
  second = wyrebox_daemon_runtime_get_default_fact_dump_file ();

  g_assert_nonnull (first);
  g_assert_nonnull (second);
  g_assert_true (first != second);

  first_path = g_file_get_path (first);
  second_path = g_file_get_path (second);
  g_assert_cmpstr (first_path, ==, "/run/wyrebox/facts");
  g_assert_cmpstr (second_path, ==, "/run/wyrebox/facts");
}

static void
test_default_runtime_paths_share_runtime_root (void)
{
  g_autofree char *socket_dir = NULL;
  g_autofree char *fact_dump_parent = NULL;

  socket_dir =
      g_path_get_dirname (wyrebox_daemon_runtime_get_default_socket_path ());
  fact_dump_parent =
      g_path_get_dirname (wyrebox_daemon_runtime_get_default_fact_dump_dir ());

  g_assert_cmpstr (socket_dir, ==,
      wyrebox_daemon_runtime_get_default_runtime_dir ());
  g_assert_cmpstr (fact_dump_parent, ==,
      wyrebox_daemon_runtime_get_default_runtime_dir ());
}

static void
test_runtime_prepare_catalog_bootstraps_missing_metadata (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, FALSE, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT schema_version FROM schema_metadata "
          "WHERE schema_key = 'schema';"), ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM accounts;"), ==, 0);

  remove_catalog_path (catalog_path);
}

static void
test_runtime_prepare_catalog_preserves_current_checkpoint (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  base.schema_version_present = TRUE;
  base.schema_version = wyrebox_schema_migration_get_current_schema_version ();
  append_runtime_journal_record (journal_root, &journal_offset,
      &journal_sequence);
  runtime_set_materialization_checkpoint_fields (&base, journal_offset,
      journal_sequence);
  save_catalog_schema_state (catalog_path, &base);

  g_assert_true (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, FALSE, &error));
  g_assert_no_error (error);

  load_catalog_schema_state (catalog_path, &loaded);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  remove_catalog_path (catalog_path);
}

static void
test_runtime_prepare_catalog_rejects_future_version (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  base.schema_version_present = TRUE;
  base.schema_version = wyrebox_schema_migration_get_current_schema_version ()
      + 1;
  append_runtime_journal_record (journal_root, &journal_offset,
      &journal_sequence);
  runtime_set_materialization_checkpoint_fields (&base, journal_offset,
      journal_sequence);
  save_catalog_schema_state (catalog_path, &base);

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  load_catalog_schema_state (catalog_path, &loaded);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);

  remove_catalog_path (catalog_path);
}

static void
test_runtime_prepare_catalog_rejects_legacy_without_checkpoint (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  append_runtime_journal_record (journal_root, &journal_offset,
      &journal_sequence);
  runtime_set_materialization_checkpoint_fields (&base, journal_offset,
      journal_sequence);
  save_catalog_schema_state (catalog_path, &base);

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_nonnull (strstr (error->message, "checkpoint precondition"));

  load_catalog_schema_state (catalog_path, &loaded);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);

  remove_catalog_path (catalog_path);
}

static void
test_runtime_prepare_catalog_migrates_legacy_with_checkpoint (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  append_runtime_journal_record (journal_root, &journal_offset,
      &journal_sequence);
  runtime_set_materialization_checkpoint_fields (&base, journal_offset,
      journal_sequence);
  save_catalog_schema_state (catalog_path, &base);

  g_assert_true (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, TRUE, &error));
  g_assert_no_error (error);

  load_catalog_schema_state (catalog_path, &loaded);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM accounts;"), ==, 0);

  remove_catalog_path (catalog_path);
}

static void
    test_runtime_prepare_catalog_rejects_corrupt_checkpoint_without_migrating
    (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autofree char *catalog_path = create_unprepared_catalog_path ();
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 journal_offset = 0;
  guint64 journal_sequence = 0;

  append_runtime_journal_record (journal_root, &journal_offset,
      &journal_sequence);
  corrupt_journal_segment_checksum (journal_root, journal_offset);

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  runtime_set_materialization_checkpoint_fields (&base, journal_offset,
      journal_sequence);
  save_catalog_schema_state (catalog_path, &base);

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog (journal_root,
          catalog_path, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  load_catalog_schema_state (catalog_path, &loaded);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  remove_catalog_path (catalog_path);
}

static void
test_runtime_prepare_catalog_rejects_invalid_args (void)
{
  g_autofree char *journal_root = create_journal_root ();
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog (NULL, NULL, FALSE,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "journal root"));
  g_clear_error (&error);

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog ("", "", FALSE,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "journal root"));
  g_clear_error (&error);

  g_assert_false (wyrebox_daemon_runtime_prepare_catalog (journal_root, "",
          FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "catalog path"));
}

static void
test_runtime_catch_up_configured_wirelog_views_for_catalog_accounts (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  guint64 account_1_uid_validity = 0;
  guint64 account_2_uid_validity = 0;

  g_assert_nonnull (root);
  seed_two_account_catalog (catalog_path);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_fact_journal_record (writer, "account-1", "mail-1");
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_true
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, catalog_path, &error));
  g_assert_no_error (error);

  assert_select_projects (catalog_path, "account-1", 1, 2,
      &account_1_uid_validity);
  assert_select_projects (catalog_path, "account-2", 0, 1,
      &account_2_uid_validity);
  g_assert_cmpuint (account_1_uid_validity, !=, account_2_uid_validity);

  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE account_id = 'account-1' AND view_id = 'view-projects' "
          "AND message_id = 'mail-1' AND is_visible = TRUE;"), ==, 1);
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE account_id = 'account-2';"), ==, 0);

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

static void
test_runtime_catch_up_configured_wirelog_views_is_idempotent (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  guint64 first_uid_validity = 0;
  guint64 second_uid_validity = 0;

  g_assert_nonnull (root);
  seed_two_account_catalog (catalog_path);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_fact_journal_record (writer, "account-1", "mail-1");
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_true
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, catalog_path, &error));
  g_assert_no_error (error);
  assert_select_projects (catalog_path, "account-1", 1, 2, &first_uid_validity);

  g_assert_true
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, catalog_path, &error));
  g_assert_no_error (error);
  assert_select_projects (catalog_path, "account-1", 1, 2,
      &second_uid_validity);

  g_assert_cmpuint (first_uid_validity, ==, second_uid_validity);
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE account_id = 'account-1' AND view_id = 'view-projects' "
          "AND message_id = 'mail-1' AND is_visible = TRUE;"), ==, 1);

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

static void
test_runtime_catch_up_configured_wirelog_views_zero_accounts (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_true
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, catalog_path, &error));
  g_assert_no_error (error);

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

static void
test_runtime_catch_up_configured_wirelog_views_failure_names_account (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;

  g_assert_nonnull (root);
  seed_two_account_catalog (catalog_path);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  append_fact_journal_record (writer, "account-2", "missing-mail");
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_false
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, catalog_path, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "account-2"));
  g_assert_nonnull (strstr (error->message, "missing-mail"));

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

static void
test_runtime_catch_up_configured_wirelog_views_rejects_invalid_args (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-runtime-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (NULL, catalog_path, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "fact mutation service"));
  g_clear_error (&error);

  g_assert_false
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "catalog path"));
  g_clear_error (&error);

  g_assert_false
      (wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
      (service, "", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_nonnull (strstr (error->message, "catalog path"));

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/runtime/default-socket-path",
      test_default_socket_path_matches_linux_runtime_contract);
  g_test_add_func ("/daemon-api/runtime/default-runtime-dir",
      test_default_socket_dir_is_run_wyrebox);
  g_test_add_func ("/daemon-api/runtime/default-fact-dump-dir",
      test_default_fact_dump_dir_is_under_run_wyrebox);
  g_test_add_func ("/daemon-api/runtime/default-fact-dump-file-owned",
      test_default_fact_dump_file_is_owned_gfile);
  g_test_add_func ("/daemon-api/runtime/default-paths-share-root",
      test_default_runtime_paths_share_runtime_root);
  g_test_add_func ("/daemon-api/runtime/prepare-catalog/missing-metadata",
      test_runtime_prepare_catalog_bootstraps_missing_metadata);
  g_test_add_func ("/daemon-api/runtime/prepare-catalog/current-checkpoint",
      test_runtime_prepare_catalog_preserves_current_checkpoint);
  g_test_add_func ("/daemon-api/runtime/prepare-catalog/future-version",
      test_runtime_prepare_catalog_rejects_future_version);
  g_test_add_func
      ("/daemon-api/runtime/prepare-catalog/legacy-without-checkpoint",
      test_runtime_prepare_catalog_rejects_legacy_without_checkpoint);
  g_test_add_func
      ("/daemon-api/runtime/prepare-catalog/legacy-with-checkpoint",
      test_runtime_prepare_catalog_migrates_legacy_with_checkpoint);
  g_test_add_func
      ("/daemon-api/runtime/prepare-catalog/rejects-corrupt-checkpoint-without-migrating",
      test_runtime_prepare_catalog_rejects_corrupt_checkpoint_without_migrating);
  g_test_add_func ("/daemon-api/runtime/prepare-catalog/invalid-args",
      test_runtime_prepare_catalog_rejects_invalid_args);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/valid-delivery",
      test_runtime_validate_delivery_storage_accepts_valid_delivery);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/missing-journal",
      test_runtime_validate_delivery_storage_accepts_missing_journal);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/uninitialized-object-root",
      test_runtime_validate_delivery_storage_rejects_uninitialized_object_root);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/missing-object",
      test_runtime_validate_delivery_storage_rejects_missing_object);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/corrupt-object",
      test_runtime_validate_delivery_storage_rejects_corrupt_object);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/unsafe-journal-suffix",
      test_runtime_validate_delivery_storage_rejects_unsafe_journal_suffix);
  g_test_add_func
      ("/daemon-api/runtime/validate-delivery-storage/invalid-args",
      test_runtime_validate_delivery_storage_rejects_invalid_args);
  g_test_add_func ("/daemon-api/runtime/catch-up-configured-wirelog-views",
      test_runtime_catch_up_configured_wirelog_views_for_catalog_accounts);
  g_test_add_func
      ("/daemon-api/runtime/catch-up-configured-wirelog-views/idempotent",
      test_runtime_catch_up_configured_wirelog_views_is_idempotent);
  g_test_add_func
      ("/daemon-api/runtime/catch-up-configured-wirelog-views/zero-accounts",
      test_runtime_catch_up_configured_wirelog_views_zero_accounts);
  g_test_add_func
      ("/daemon-api/runtime/catch-up-configured-wirelog-views/failure-names-account",
      test_runtime_catch_up_configured_wirelog_views_failure_names_account);
  g_test_add_func
      ("/daemon-api/runtime/catch-up-configured-wirelog-views/invalid-args",
      test_runtime_catch_up_configured_wirelog_views_rejects_invalid_args);

  return g_test_run ();
}
