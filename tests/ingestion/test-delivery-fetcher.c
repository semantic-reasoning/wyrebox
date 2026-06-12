#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-fetcher.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

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
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-wrong-uidvalidity",
      test_fetcher_rejects_wrong_uidvalidity);
  g_test_add_func ("/ingestion/delivery-fetcher/rejects-missing-uid",
      test_fetcher_rejects_missing_uid);
  g_test_add_func ("/ingestion/delivery-fetcher/duplicate-deliveries",
      test_fetcher_fetches_duplicate_deliveries_as_distinct_uids);

  return g_test_run ();
}
