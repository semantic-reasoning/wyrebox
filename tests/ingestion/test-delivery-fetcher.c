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
} TestDuckdb;

static void
test_duckdb_clear (TestDuckdb *duckdb)
{
  if (duckdb->connection != NULL)
    duckdb_disconnect (&duckdb->connection);
  if (duckdb->database != NULL)
    duckdb_close (&duckdb->database);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TestDuckdb, test_duckdb_clear)
/* *INDENT-ON* */

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

static TestDuckdb
open_test_catalog (const gchar *catalog_path)
{
  TestDuckdb duckdb = { 0 };

  g_assert_cmpint (duckdb_open (catalog_path, &duckdb.database), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (duckdb.database, &duckdb.connection), ==,
      DuckDBSuccess);

  return duckdb;
}

static void
exec_catalog_sql (TestDuckdb *duckdb, const gchar *sql)
{
  duckdb_result result;

  g_assert_cmpint (duckdb_query (duckdb->connection, sql, &result), ==,
      DuckDBSuccess);
  duckdb_destroy_result (&result);
}

static guint64
query_catalog_count (const gchar *catalog_path, const gchar *table_name)
{
  g_auto (TestDuckdb) duckdb = open_test_catalog (catalog_path);
  g_autofree gchar *sql = NULL;
  duckdb_result result;
  guint64 count = 0;

  sql = g_strdup_printf ("SELECT COUNT(*) FROM %s;", table_name);
  g_assert_cmpint (duckdb_query (duckdb.connection, sql, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  count = (guint64) duckdb_value_uint64 (&result, 0, 0);
  duckdb_destroy_result (&result);

  return count;
}

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-delivery-fetcher-catalog-XXXXXX", &error);
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
          5, 6, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US,
          6, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS,
          8, wyrebox_schema_migration_get_current_schema_version (), &error));
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
ingest_bytes (WyreboxEmlIngestor *ingestor, GBytes *bytes)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxEmlIngestResult) result = { 0 };

  g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, bytes,
          &result, &error));
  g_assert_no_error (error);
  g_assert_nonnull (result.object_key);
  g_assert_cmpuint (result.journal_sequence, >, 0);
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
    const gchar *object_root, guint64 uidvalidity, guint64 uid, GError **error)
{
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryFetcher) fetcher = NULL;

  object_store = wyrebox_local_object_store_new (object_root, error);
  if (object_store == NULL)
    return NULL;

  fetcher = wyrebox_delivery_fetcher_new_duckdb (catalog_path, object_store,
      error);
  if (fetcher == NULL)
    return NULL;

  return wyrebox_delivery_fetcher_fetch_bytes (fetcher,
      "account-1", "mailbox-inbox", uidvalidity, uid, error);
}

static GBytes *
fetch_reopened_namespace (const gchar *catalog_path,
    const gchar *object_root,
    WyreboxDeliveryFetcherNamespaceKind namespace_kind,
    const gchar *namespace_id, guint64 uidvalidity, guint64 uid, GError **error)
{
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryFetcher) fetcher = NULL;

  object_store = wyrebox_local_object_store_new (object_root, error);
  if (object_store == NULL)
    return NULL;

  fetcher = wyrebox_delivery_fetcher_new_duckdb (catalog_path, object_store,
      error);
  if (fetcher == NULL)
    return NULL;

  return wyrebox_delivery_fetcher_fetch_namespace_bytes (fetcher,
      "account-1", namespace_kind, namespace_id, uidvalidity, uid, error);
}

static void
seed_derived_view_for_inbox_uid (const gchar *catalog_path,
    gboolean is_selectable, gboolean is_visible)
{
  g_auto (TestDuckdb) duckdb = open_test_catalog (catalog_path);
  g_autofree gchar *view_sql = NULL;

  view_sql = g_strdup_printf ("INSERT INTO derived_views ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") VALUES ("
      "'view-important', 'account-1', 'Important', "
      "'rule:important', %s, %s" ");",
      is_selectable ? "TRUE" : "FALSE", is_visible ? "TRUE" : "FALSE");
  exec_catalog_sql (&duckdb, view_sql);

  exec_catalog_sql (&duckdb,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES (" "'account-1', 'derived_view', 'view-important', 3, 21" ");");
  exec_catalog_sql (&duckdb,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") "
      "SELECT 'dvm-visible', account_id, 'view-important', message_id, "
      "1, TRUE, 'hash-visible', 1 "
      "FROM mailbox_memberships "
      "WHERE account_id = 'account-1' "
      "AND mailbox_id = 'mailbox-inbox' " "AND uid = 1;");
  exec_catalog_sql (&duckdb,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") "
      "SELECT 'dvm-hidden', account_id, 'view-important', message_id, "
      "2, FALSE, 'hash-hidden', 2 "
      "FROM mailbox_memberships "
      "WHERE account_id = 'account-1' "
      "AND mailbox_id = 'mailbox-inbox' " "AND uid = 1;");
}

static void
seed_visible_derived_view_for_inbox_uid (const gchar *catalog_path)
{
  seed_derived_view_for_inbox_uid (catalog_path, TRUE, TRUE);
}

static void
seed_cross_account_derived_view_conflict (const gchar *catalog_path)
{
  g_auto (TestDuckdb) duckdb = open_test_catalog (catalog_path);

  exec_catalog_sql (&duckdb,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") "
      "SELECT 'account-2-message', 'account-2', m.object_id, "
      "m.journal_offset + 1000, m.journal_sequence + 1000 "
      "FROM mailbox_memberships mm "
      "JOIN messages m ON m.account_id = mm.account_id "
      "AND m.message_id = mm.message_id "
      "WHERE mm.account_id = 'account-1' "
      "AND mm.mailbox_id = 'mailbox-inbox' " "AND mm.uid = 2;");
  exec_catalog_sql (&duckdb,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES ("
      "'account-2', 'derived_view', 'view-important', 100, 21" ");");
  exec_catalog_sql (&duckdb,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'dvm-account-2-conflict', 'account-2', 'view-important', "
      "'account-2-message', 99, TRUE, 'hash-account-2', 99" ");");
}

static void
test_fetcher_fetches_materialized_inbox_uid_after_reopen (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);

  g_clear_object (&ingestor);
  fetched = fetch_reopened (catalog_path, object_root, 1, 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (fetched);
  assert_bytes_equal (fetched, input);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_fetches_ordinary_and_derived_memberships_as_same_bytes (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) ordinary = NULL;
  g_autoptr (GBytes) derived = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  guint64 objects_before = 0;
  guint64 messages_before = 0;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_visible_derived_view_for_inbox_uid (catalog_path);

  objects_before = query_catalog_count (catalog_path, "objects");
  messages_before = query_catalog_count (catalog_path, "messages");

  g_clear_object (&ingestor);
  ordinary = fetch_reopened (catalog_path, object_root, 1, 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (ordinary);
  derived = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 1,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (derived);
  assert_bytes_equal (ordinary, input);
  assert_bytes_equal (derived, ordinary);
  g_assert_cmpuint (query_catalog_count (catalog_path, "objects"), ==,
      objects_before);
  g_assert_cmpuint (query_catalog_count (catalog_path, "messages"), ==,
      messages_before);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_invisible_derived_view (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_derived_view_for_inbox_uid (catalog_path, TRUE, FALSE);

  g_clear_object (&ingestor);
  fetched = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 1,
      &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_unselectable_derived_view (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_derived_view_for_inbox_uid (catalog_path, FALSE, TRUE);

  g_clear_object (&ingestor);
  fetched = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 1,
      &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_cross_account_derived_view_conflict (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) first_input = NULL;
  g_autoptr (GBytes) second_input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (GBytes) missing = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  first_input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  second_input = load_fixture_bytes (fixture_dir, "html-message.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, first_input);
  ingest_bytes (ingestor, second_input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_visible_derived_view_for_inbox_uid (catalog_path);
  seed_cross_account_derived_view_conflict (catalog_path);

  g_clear_object (&ingestor);
  missing = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 99,
      &error);
  g_assert_null (missing);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  fetched = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 1,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (fetched);
  assert_bytes_equal (fetched, first_input);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_derived_uidvalidity_mismatch (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_visible_derived_view_for_inbox_uid (catalog_path);

  g_clear_object (&ingestor);
  fetched = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 22, 1,
      &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_hidden_derived_membership (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);
  seed_visible_derived_view_for_inbox_uid (catalog_path);

  g_clear_object (&ingestor);
  fetched = fetch_reopened_namespace (catalog_path, object_root,
      WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW, "view-important", 21, 2,
      &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_wrong_uidvalidity (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);

  g_clear_object (&ingestor);
  fetched = fetch_reopened (catalog_path, object_root, 2, 1, &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_rejects_missing_uid (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) fetched = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);

  g_clear_object (&ingestor);
  fetched = fetch_reopened (catalog_path, object_root, 1, 99, &error);
  g_assert_null (fetched);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

static void
test_fetcher_fetches_duplicate_deliveries_as_distinct_uids (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-delivery-fetcher-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) input = NULL;
  g_autoptr (GBytes) first = NULL;
  g_autoptr (GBytes) second = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (object_root, journal_root);
  ingest_bytes (ingestor, input);
  ingest_bytes (ingestor, input);
  run_catchup (catalog_path, object_root, journal_root);

  g_clear_object (&ingestor);
  first = fetch_reopened (catalog_path, object_root, 1, 1, &error);
  g_assert_no_error (error);
  g_assert_nonnull (first);
  second = fetch_reopened (catalog_path, object_root, 1, 2, &error);
  g_assert_no_error (error);
  g_assert_nonnull (second);
  assert_bytes_equal (first, input);
  assert_bytes_equal (second, input);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/delivery-fetcher/fetches-reopened-inbox-uid",
      test_fetcher_fetches_materialized_inbox_uid_after_reopen);
  g_test_add_func ("/ingestion/delivery-fetcher/fetches-derived-view-uid",
      test_fetcher_fetches_ordinary_and_derived_memberships_as_same_bytes);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-invisible-derived-view",
      test_fetcher_rejects_invisible_derived_view);
  g_test_add_func
      ("/ingestion/delivery-fetcher/rejects-unselectable-derived-view",
      test_fetcher_rejects_unselectable_derived_view);
  g_test_add_func
      ("/ingestion/delivery-fetcher/rejects-cross-account-derived-conflict",
      test_fetcher_rejects_cross_account_derived_view_conflict);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-derived-uidvalidity",
      test_fetcher_rejects_derived_uidvalidity_mismatch);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-hidden-derived-uid",
      test_fetcher_rejects_hidden_derived_membership);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-wrong-uidvalidity",
      test_fetcher_rejects_wrong_uidvalidity);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-missing-uid",
      test_fetcher_rejects_missing_uid);
  g_test_add_func ("/ingestion/delivery-fetcher/duplicate-deliveries",
      test_fetcher_fetches_duplicate_deliveries_as_distinct_uids);

  return g_test_run ();
}
