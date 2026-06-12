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
assert_bytes_equal (GBytes *actual, GBytes *expected)
{
  gsize actual_size = 0;
  gsize expected_size = 0;
  const guint8 *actual_data = g_bytes_get_data (actual, &actual_size);
  const guint8 *expected_data = g_bytes_get_data (expected, &expected_size);

  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected_data, expected_size);
}

static gint
compare_strings (gconstpointer left, gconstpointer right)
{
  const gchar *left_name = *(const gchar * const *) left;
  const gchar *right_name = *(const gchar * const *) right;

  return g_strcmp0 (left_name, right_name);
}

static GPtrArray *
list_fixture_names (const gchar *fixture_dir)
{
  g_autoptr (GDir) dir = NULL;
  const gchar *name = NULL;
  g_autoptr (GError) error = NULL;
  GPtrArray *names = NULL;

  dir = g_dir_open (fixture_dir, 0, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  names = g_ptr_array_new_with_free_func (g_free);
  while ((name = g_dir_read_name (dir)) != NULL) {
    if (g_str_has_suffix (name, ".eml"))
      g_ptr_array_add (names, g_strdup (name));
  }

  g_ptr_array_sort (names, compare_strings);
  g_assert_cmpuint (names->len, >, 0);
  return names;
}

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-eml-roundtrip-catalog-XXXXXX", &error);
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

static gchar *
query_nullable_string_by_subject (duckdb_connection connection,
    const gchar *column, const gchar *subject)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_auto (TestDuckdbOwnedString) value = NULL;
  g_autofree gchar *sql = NULL;

  sql = g_strdup_printf ("SELECT %s FROM message_headers WHERE subject = ?;",
      column);
  g_assert_cmpint (duckdb_prepare (connection, sql, &statement), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 1, subject), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_execute_prepared (statement, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);

  if (duckdb_value_is_null (&result, 0, 0))
    return NULL;

  value = duckdb_value_varchar (&result, 0, 0);
  return g_strdup (value);
}

static guint64
query_uint64_by_subject (duckdb_connection connection,
    const gchar *column, const gchar *subject)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *sql = NULL;

  sql = g_strdup_printf ("SELECT %s FROM message_headers WHERE subject = ?;",
      column);
  g_assert_cmpint (duckdb_prepare (connection, sql, &statement), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_bind_varchar (statement, 1, subject), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_execute_prepared (statement, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static void
assert_materialized_state (const gchar *catalog_path, guint fixture_count)
{
  TestDuckdbFixture duckdb = { 0 };
  guint64 expected_uidnext = fixture_count + 1;
  g_autofree gchar *missing_message_id = NULL;
  g_autofree gchar *duplicate_message_id = NULL;
  g_autofree gchar *non_ascii_from = NULL;
  g_autofree gchar *non_ascii_to = NULL;
  g_autofree gchar *non_ascii_subject = NULL;

  open_duckdb_fixture (catalog_path, &duckdb);

  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(*) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, fixture_count);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT COUNT(DISTINCT uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, fixture_count);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT MIN(uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT MAX(uid) FROM mailbox_memberships WHERE "
          "account_id = 'account-1' AND mailbox_id = 'mailbox-inbox' "
          "AND is_visible = TRUE;"), ==, fixture_count);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, expected_uidnext);

  missing_message_id = query_nullable_string_by_subject (duckdb.connection,
      "rfc_message_id", "Missing Message-ID fixture");
  g_assert_null (missing_message_id);

  duplicate_message_id = query_nullable_string_by_subject (duckdb.connection,
      "rfc_message_id", "Duplicate Message-ID fixture");
  g_assert_cmpstr (duplicate_message_id, ==,
      "<duplicate-message-id@example.test>");
  g_assert_cmpuint (query_uint64_by_subject (duckdb.connection,
          "duplicate_message_id_count", "Duplicate Message-ID fixture"), ==, 1);

  non_ascii_subject = query_nullable_string_by_subject (duckdb.connection,
      "subject", "=?UTF-8?B?UsOpc3Vtw6kg4oCTIOyVhOuFle2VmOyEuOyalA==?=");
  g_assert_cmpstr (non_ascii_subject, ==,
      "=?UTF-8?B?UsOpc3Vtw6kg4oCTIOyVhOuFle2VmOyEuOyalA==?=");
  non_ascii_from = query_nullable_string_by_subject (duckdb.connection,
      "from_addr", "=?UTF-8?B?UsOpc3Vtw6kg4oCTIOyVhOuFle2VmOyEuOyalA==?=");
  g_assert_cmpstr (non_ascii_from, ==,
      "=?UTF-8?B?SmnFmcOtIMWgYWZhw61r?= <jiri@example.test>");
  non_ascii_to = query_nullable_string_by_subject (duckdb.connection,
      "to_addr", "=?UTF-8?B?UsOpc3Vtw6kg4oCTIOyVhOuFle2VmOyEuOyalA==?=");
  g_assert_cmpstr (non_ascii_to, ==,
      "=?UTF-8?Q?Zo=C3=AB_Reader?= <zoe@example.test>");

  close_duckdb_fixture (&duckdb);
}

static void
assert_all_fixtures_fetch_by_uid (const gchar *catalog_path,
    const gchar *object_root, GPtrArray *fixture_names,
    GPtrArray *fixture_bytes)
{
  for (guint i = 0; i < fixture_names->len; i++) {
    g_autoptr (GBytes) fetched = NULL;
    GBytes *expected = g_ptr_array_index (fixture_bytes, i);
    guint64 uid = i + 1;

    fetched = fetch_reopened (catalog_path, object_root, uid);
    assert_bytes_equal (fetched, expected);
  }
}

static void
test_all_fixtures_roundtrip_through_materialized_uids (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_autofree gchar *catalog_path = create_bootstrap_catalog ();
  g_autofree gchar *object_root =
      g_dir_make_tmp ("wyrebox-eml-roundtrip-objects-XXXXXX", NULL);
  g_autofree gchar *journal_root =
      g_dir_make_tmp ("wyrebox-eml-roundtrip-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) fixture_names = NULL;
  g_autoptr (GPtrArray) fixture_bytes = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  fixture_names = list_fixture_names (fixture_dir);
  fixture_bytes = g_ptr_array_new_with_free_func ((GDestroyNotify)
      g_bytes_unref);
  ingestor = create_ingestor (object_root, journal_root);

  for (guint i = 0; i < fixture_names->len; i++) {
    const gchar *name = g_ptr_array_index (fixture_names, i);
    g_autoptr (GBytes) bytes = NULL;
    g_auto (WyreboxEmlIngestResult) result = { 0 };

    bytes = load_fixture_bytes (fixture_dir, name);
    g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, bytes,
            &result, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (result.journal_sequence, ==, i + 1);
    g_ptr_array_add (fixture_bytes, g_bytes_ref (bytes));
  }

  g_clear_object (&ingestor);

  run_catchup (catalog_path, object_root, journal_root);
  assert_materialized_state (catalog_path, fixture_names->len);
  assert_all_fixtures_fetch_by_uid (catalog_path, object_root, fixture_names,
      fixture_bytes);

  run_catchup (catalog_path, object_root, journal_root);
  assert_materialized_state (catalog_path, fixture_names->len);
  assert_all_fixtures_fetch_by_uid (catalog_path, object_root, fixture_names,
      fixture_bytes);

  remove_tree (object_root);
  remove_tree (journal_root);
  remove_catalog (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/eml-roundtrip/all-fixtures",
      test_all_fixtures_roundtrip_through_materialized_uids);

  return g_test_run ();
}
