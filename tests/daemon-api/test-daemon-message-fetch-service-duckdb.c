#include "wyrebox-daemon-message-fetch-dispatcher.h"
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

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdb;

typedef struct
{
  gchar *catalog_path;
  gchar *object_root;
  gchar *journal_root;
  GBytes *input;
  gchar *ordinary_message_id;
  gchar *derived_message_id;
} FetchFixture;

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

static void
fetch_fixture_clear (FetchFixture *fixture)
{
  if (fixture->object_root != NULL)
    remove_tree (fixture->object_root);
  if (fixture->journal_root != NULL)
    remove_tree (fixture->journal_root);
  if (fixture->catalog_path != NULL) {
    g_autofree gchar *dir = g_path_get_dirname (fixture->catalog_path);

    remove_tree (dir);
  }

  g_clear_pointer (&fixture->catalog_path, g_free);
  g_clear_pointer (&fixture->object_root, g_free);
  g_clear_pointer (&fixture->journal_root, g_free);
  g_clear_pointer (&fixture->input, g_bytes_unref);
  g_clear_pointer (&fixture->ordinary_message_id, g_free);
  g_clear_pointer (&fixture->derived_message_id, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (FetchFixture, fetch_fixture_clear)
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

static gchar *
query_single_string (const gchar *catalog_path, const gchar *sql)
{
  g_auto (TestDuckdb) duckdb = open_test_catalog (catalog_path);
  duckdb_result result;
  gchar *copy = NULL;

  g_assert_cmpint (duckdb_query (duckdb.connection, sql, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  {
    char *value = duckdb_value_varchar (&result, 0, 0);

    copy = g_strdup (value);
    duckdb_free (value);
  }

  duckdb_destroy_result (&result);

  return copy;
}

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-daemon-fetch-catalog-XXXXXX", &error);
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

static void
seed_visible_derived_view (const gchar *catalog_path)
{
  g_auto (TestDuckdb) duckdb = open_test_catalog (catalog_path);

  exec_catalog_sql (&duckdb,
      "INSERT INTO derived_views ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") VALUES ("
      "'view-important', 'account-1', 'Important', "
      "'rule:important', TRUE, TRUE" ");");
  exec_catalog_sql (&duckdb,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES (" "'account-1', 'derived_view', 'view-important', 2, 21" ");");
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
}

static FetchFixture
create_fetch_fixture (void)
{
  const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
  g_auto (FetchFixture) fixture = { 0 };
  FetchFixture out = { 0 };
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

  g_assert_nonnull (fixture_dir);

  fixture.catalog_path = create_bootstrap_catalog ();
  fixture.object_root =
      g_dir_make_tmp ("wyrebox-daemon-fetch-objects-XXXXXX", NULL);
  fixture.journal_root =
      g_dir_make_tmp ("wyrebox-daemon-fetch-journal-XXXXXX", NULL);
  g_assert_nonnull (fixture.object_root);
  g_assert_nonnull (fixture.journal_root);

  fixture.input = load_fixture_bytes (fixture_dir, "simple-crlf.eml");
  ingestor = create_ingestor (fixture.object_root, fixture.journal_root);
  ingest_bytes (ingestor, fixture.input);
  run_catchup (fixture.catalog_path, fixture.object_root, fixture.journal_root);
  seed_visible_derived_view (fixture.catalog_path);

  fixture.ordinary_message_id = query_single_string (fixture.catalog_path,
      "SELECT message_id FROM mailbox_memberships "
      "WHERE account_id = 'account-1' "
      "AND mailbox_id = 'mailbox-inbox' AND uid = 1;");
  fixture.derived_message_id = query_single_string (fixture.catalog_path,
      "SELECT message_id FROM derived_view_memberships "
      "WHERE account_id = 'account-1' "
      "AND view_id = 'view-important' AND uid = 1;");

  out.catalog_path = g_steal_pointer (&fixture.catalog_path);
  out.object_root = g_steal_pointer (&fixture.object_root);
  out.journal_root = g_steal_pointer (&fixture.journal_root);
  out.input = g_steal_pointer (&fixture.input);
  out.ordinary_message_id = g_steal_pointer (&fixture.ordinary_message_id);
  out.derived_message_id = g_steal_pointer (&fixture.derived_message_id);

  return out;
}

static WyreboxDaemonMessageFetchService *
create_service_for_fixture (const FetchFixture *fixture)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryFetcher) fetcher = NULL;

  object_store = wyrebox_local_object_store_new (fixture->object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (object_store);

  fetcher = wyrebox_delivery_fetcher_new_duckdb (fixture->catalog_path,
      object_store, &error);
  g_assert_no_error (error);
  g_assert_nonnull (fetcher);

  return wyrebox_daemon_message_fetch_service_new_for_fetcher (fetcher);
}

static void
assert_fetch_dispatch_returns_message (WyreboxDaemonMessageFetchService
    *service, WyreboxDaemonMailboxListEntryKind namespace_kind,
    const char *namespace_id, guint64 uidvalidity, guint64 uid,
    const char *expected_message_id, GBytes *expected_bytes)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", namespace_id, namespace_kind, uidvalidity, uid, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-fetch-1");
  g_assert_cmpstr (frame.stream_chunk.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.stream_chunk.message_id, ==, expected_message_id);
  g_assert_nonnull (frame.stream_chunk.message_id);
  g_assert_cmpstr (frame.stream_chunk.query_id, ==, NULL);
  g_assert_cmpstr (frame.stream_chunk.correlation_id, ==, "imap-fetch-1");
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);
  assert_bytes_equal (frame.stream_chunk.bytes, expected_bytes);
}

static void
assert_fetch_dispatch_returns_error (WyreboxDaemonMessageFetchService *service,
    WyreboxDaemonMessageFetchRequest *request)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-fetch-1");
}

static void
test_fetch_service_returns_ordinary_bytes_and_message_id (void)
{
  g_auto (FetchFixture) fixture = create_fetch_fixture ();
  g_autoptr (WyreboxDaemonMessageFetchService) service =
      create_service_for_fixture (&fixture);

  assert_fetch_dispatch_returns_message (service,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", 1, 1, fixture.ordinary_message_id, fixture.input);
}

static void
test_fetch_service_returns_virtual_bytes_and_message_id (void)
{
  g_auto (FetchFixture) fixture = create_fetch_fixture ();
  g_autoptr (WyreboxDaemonMessageFetchService) service =
      create_service_for_fixture (&fixture);

  g_assert_cmpstr (fixture.derived_message_id, ==, fixture.ordinary_message_id);
  assert_fetch_dispatch_returns_message (service,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
      "view-important", 21, 1, fixture.derived_message_id, fixture.input);
}

static void
test_fetch_service_rejects_unknown_namespace_kind (void)
{
  g_auto (FetchFixture) fixture = create_fetch_fixture ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service =
      create_service_for_fixture (&fixture);
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, 1, &error));
  g_assert_no_error (error);
  request.namespace_kind = (WyreboxDaemonMailboxListEntryKind) 99;

  assert_fetch_dispatch_returns_error (service, &request);
}

static void
test_fetch_service_rejects_uidvalidity_mismatch (void)
{
  g_auto (FetchFixture) fixture = create_fetch_fixture ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service =
      create_service_for_fixture (&fixture);
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 2, 1, &error));
  g_assert_no_error (error);

  assert_fetch_dispatch_returns_error (service, &request);
}

static void
test_fetch_service_rejects_missing_uid (void)
{
  g_auto (FetchFixture) fixture = create_fetch_fixture ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service =
      create_service_for_fixture (&fixture);
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, 99, &error));
  g_assert_no_error (error);

  assert_fetch_dispatch_returns_error (service, &request);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon/message-fetch-service-duckdb/ordinary-fetches-bytes",
      test_fetch_service_returns_ordinary_bytes_and_message_id);
  g_test_add_func
      ("/daemon/message-fetch-service-duckdb/virtual-fetches-bytes",
      test_fetch_service_returns_virtual_bytes_and_message_id);
  g_test_add_func
      ("/daemon/message-fetch-service-duckdb/rejects-unknown-kind",
      test_fetch_service_rejects_unknown_namespace_kind);
  g_test_add_func
      ("/daemon/message-fetch-service-duckdb/rejects-uidvalidity-mismatch",
      test_fetch_service_rejects_uidvalidity_mismatch);
  g_test_add_func ("/daemon/message-fetch-service-duckdb/rejects-missing-uid",
      test_fetch_service_rejects_missing_uid);

  return g_test_run ();
}
