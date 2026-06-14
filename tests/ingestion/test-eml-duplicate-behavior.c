#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-fetcher.h"
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

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdbFixture;

typedef struct
{
  const gchar *first_bytes;
  const gchar *second_bytes;
  const gchar *shared_message_id;
  const gchar *shared_from;
  const gchar *shared_to;
  const gchar *shared_subject;
  const gchar *shared_date;
  gboolean expect_null_message_id;
} DuplicateScenario;

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

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
  duckdb_destroy_prepare (statement);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    duckdb_prepared_statement_clear)
/* *INDENT-ON* */

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

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-duplicate-behavior-catalog-XXXXXX", &error);
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
          2, 3, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
          5, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);

  return g_steal_pointer (&path);
}

static void
remove_catalog (const gchar *path)
{
  g_autofree gchar *dir = g_path_get_dirname (path);

  remove_tree (dir);
}

static WyreboxEmlIngestor *
create_ingestor (const gchar *object_root, const gchar *journal_root)
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

  return g_steal_pointer (&ingestor);
}

static void
run_catchup (const gchar *catalog_path,
    const gchar *object_root, const gchar *journal_root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

  metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (metadata_store);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  object_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

  materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);

  g_assert_true (wyrebox_delivery_catchup_materialize_inbox (metadata_store,
          reader, object_store, materializer, "account-1", &error));
  g_assert_no_error (error);
}

static GBytes *
fetch_reopened (const gchar *catalog_path,
    const gchar *object_root, guint64 uid)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryFetcher) fetcher = NULL;
  GBytes *bytes = NULL;

  object_store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

  fetcher = wyrebox_delivery_fetcher_new_duckdb (catalog_path, object_store,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (fetcher);

  bytes = wyrebox_delivery_fetcher_fetch_bytes (fetcher,
      "account-1", "mailbox-inbox", 1, uid, &error);
  g_assert_no_error (error);
  g_assert_nonnull (bytes);

  return bytes;
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

static guint64
query_count_with_one_text (duckdb_connection connection,
    const gchar *sql, const gchar *value)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_prepare (connection, sql, &statement), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 1, value), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_execute_prepared (statement, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static guint64
query_count_with_core_metadata (duckdb_connection connection,
    const DuplicateScenario *scenario)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_prepare (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE from_addr = ? AND to_addr = ? AND subject = ? "
          "AND date_raw = ?;", &statement), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 1, scenario->shared_from),
      ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 2, scenario->shared_to),
      ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 3, scenario->shared_subject),
      ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 4, scenario->shared_date),
      ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_execute_prepared (statement, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static void
assert_materialized_duplicate_state (const gchar *catalog_path,
    const DuplicateScenario *scenario)
{
  TestDuckdbFixture duckdb = { 0 };

  open_duckdb_fixture (catalog_path, &duckdb);

  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(*) FROM objects;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(DISTINCT object_id) FROM objects;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(*) FROM messages;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(DISTINCT message_id) FROM messages;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(*) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(DISTINCT uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT MIN(uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT MAX(uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 3);
  g_assert_cmpuint (query_count_with_core_metadata (duckdb.connection,
          scenario), ==, 2);

  if (scenario->expect_null_message_id) {
    g_assert_cmpuint (query_uint64 (duckdb.connection,
            "SELECT COUNT(*) FROM message_headers "
            "WHERE rfc_message_id IS NULL;"), ==, 2);
  } else {
    g_assert_cmpuint (query_count_with_one_text (duckdb.connection,
            "SELECT COUNT(*) FROM message_headers WHERE rfc_message_id = ?;",
            scenario->shared_message_id), ==, 2);
  }

  close_duckdb_fixture (&duckdb);
}

static void
assert_duplicate_scenario (const DuplicateScenario *scenario)
{
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-duplicate-behavior-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-duplicate-behavior-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) first_input =
      g_bytes_new_static (scenario->first_bytes,
      strlen (scenario->first_bytes));
  g_autoptr (GBytes) second_input =
      g_bytes_new_static (scenario->second_bytes,
      strlen (scenario->second_bytes));
  g_autoptr (GBytes) first_fetched = NULL;
  g_autoptr (GBytes) second_fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_auto (WyreboxEmlIngestResult) first_result = { 0 };
  g_auto (WyreboxEmlIngestResult) second_result = { 0 };

  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  ingestor = create_ingestor (object_root, journal_root);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, first_input,
          &first_result, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, second_input,
          &second_result, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (first_result.object_key, !=, second_result.object_key);
  g_assert_cmpuint (first_result.journal_sequence, ==, 1);
  g_assert_cmpuint (second_result.journal_sequence, ==, 2);

  g_clear_object (&ingestor);

  run_catchup (catalog_path, object_root, journal_root);
  assert_materialized_duplicate_state (catalog_path, scenario);
  first_fetched = fetch_reopened (catalog_path, object_root, 1);
  second_fetched = fetch_reopened (catalog_path, object_root, 2);
  assert_bytes_equal (first_fetched, first_input);
  assert_bytes_equal (second_fetched, second_input);

  g_clear_pointer (&first_fetched, g_bytes_unref);
  g_clear_pointer (&second_fetched, g_bytes_unref);

  run_catchup (catalog_path, object_root, journal_root);
  assert_materialized_duplicate_state (catalog_path, scenario);
  first_fetched = fetch_reopened (catalog_path, object_root, 1);
  second_fetched = fetch_reopened (catalog_path, object_root, 2);
  assert_bytes_equal (first_fetched, first_input);
  assert_bytes_equal (second_fetched, second_input);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_same_rfc_message_id_different_bytes (void)
{
  static const DuplicateScenario scenario = {
    .first_bytes =
        "From: Sender One <sender@example.test>\r\n"
        "To: Recipient <recipient@example.test>\r\n"
        "Subject: Duplicate Message-ID matrix\r\n"
        "Date: Tue, 09 Jun 2026 10:00:00 +0000\r\n"
        "Message-ID: <shared-rfc-id@example.test>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=us-ascii\r\n"
        "\r\n" "First body for shared RFC Message-ID.\r\n",
    .second_bytes =
        "From: Sender One <sender@example.test>\r\n"
        "To: Recipient <recipient@example.test>\r\n"
        "Subject: Duplicate Message-ID matrix\r\n"
        "Date: Tue, 09 Jun 2026 10:00:00 +0000\r\n"
        "Message-ID: <shared-rfc-id@example.test>\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=us-ascii\r\n"
        "\r\n" "Second body for shared RFC Message-ID.\r\n",
    .shared_message_id = "<shared-rfc-id@example.test>",
    .shared_from = "Sender One <sender@example.test>",
    .shared_to = "Recipient <recipient@example.test>",
    .shared_subject = "Duplicate Message-ID matrix",
    .shared_date = "Tue, 09 Jun 2026 10:00:00 +0000",
    .expect_null_message_id = FALSE,
  };

  assert_duplicate_scenario (&scenario);
}

static void
test_same_core_metadata_different_bytes_without_message_id (void)
{
  static const DuplicateScenario scenario = {
    .first_bytes =
        "From: Shared Core <shared-core@example.test>\r\n"
        "To: Recipient <recipient@example.test>\r\n"
        "Subject: Shared core metadata matrix\r\n"
        "Date: Tue, 09 Jun 2026 11:00:00 +0000\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=us-ascii\r\n"
        "\r\n" "First body without Message-ID.\r\n",
    .second_bytes =
        "From: Shared Core <shared-core@example.test>\r\n"
        "To: Recipient <recipient@example.test>\r\n"
        "Subject: Shared core metadata matrix\r\n"
        "Date: Tue, 09 Jun 2026 11:00:00 +0000\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: text/plain; charset=us-ascii\r\n"
        "\r\n" "Second body without Message-ID.\r\n",
    .shared_message_id = NULL,
    .shared_from = "Shared Core <shared-core@example.test>",
    .shared_to = "Recipient <recipient@example.test>",
    .shared_subject = "Shared core metadata matrix",
    .shared_date = "Tue, 09 Jun 2026 11:00:00 +0000",
    .expect_null_message_id = TRUE,
  };

  assert_duplicate_scenario (&scenario);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/duplicate-behavior/same-rfc-message-id",
      test_same_rfc_message_id_different_bytes);
  g_test_add_func ("/ingestion/duplicate-behavior/same-core-metadata",
      test_same_core_metadata_different_bytes_without_message_id);

  return g_test_run ();
}
