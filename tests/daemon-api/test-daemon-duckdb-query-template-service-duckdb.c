#include "wyrebox-daemon-duckdb-query-template-dispatcher.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static void
duckdb_connection_clear (duckdb_connection *connection)
{
  duckdb_disconnect (connection);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_connection, duckdb_connection_clear)
/* *INDENT-ON* */

static void
duckdb_database_clear (duckdb_database *database)
{
  duckdb_close (database);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_database, duckdb_database_clear)
/* *INDENT-ON* */

static void
exec_sql (duckdb_connection connection, const gchar *sql)
{
  g_assert_cmpint (duckdb_query (connection, sql, NULL), ==, DuckDBSuccess);
}

static gchar *
create_temp_catalog_path (void)
{
  gint fd = -1;
  gchar *path = NULL;

  fd = g_file_open_tmp ("wyrebox-duckdb-query-template-XXXXXX.duckdb", &path,
      NULL);
  g_assert_cmpint (fd, >=, 0);
  g_assert_nonnull (path);

  g_assert_true (g_close (fd, NULL));
  (void) g_remove (path);
  return path;
}

static void
bootstrap_catalog (const gchar *path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
}

static void
seed_catalog (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO accounts (account_id) VALUES "
      "('account-1'), ('account-2');");
  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-b', 'account-1', 'object-b', 1, 1),"
      "('message-a', 'account-1', 'object-a', 2, 2),"
      "('message-hidden', 'account-1', 'object-hidden', 3, 3),"
      "('message-other-mailbox', 'account-1', 'object-other-mailbox', 4, 4),"
      "('message-other-account', 'account-2', 'object-other-account', 5, 5);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'mailbox', 'mailbox-inbox', 4, 77),"
      "('account-1', 'mailbox', 'mailbox-archive', 2, 88),"
      "('account-2', 'mailbox', 'mailbox-inbox', 3, 99),"
      "('account-1', 'derived_view', 'mailbox-inbox', 2, 55);");
  exec_sql (connection,
      "INSERT INTO mailbox_memberships (membership_id, account_id, "
      "mailbox_id, message_id, uid, internal_date_unix_us, journal_offset, "
      "journal_sequence, is_visible) VALUES "
      "('membership-b', 'account-1', 'mailbox-inbox', 'message-b', 2, 1, 1, "
      "1, TRUE),"
      "('membership-a', 'account-1', 'mailbox-inbox', 'message-a', 1, 2, 2, "
      "2, TRUE),"
      "('membership-hidden', 'account-1', 'mailbox-inbox', "
      "'message-hidden', 3, 3, 3, 3, FALSE),"
      "('membership-other-mailbox', 'account-1', 'mailbox-archive', "
      "'message-other-mailbox', 4, 4, 4, 4, TRUE),"
      "('membership-other-account', 'account-2', 'mailbox-inbox', "
      "'message-other-account', 5, 5, 5, 5, TRUE);");
}

static void
seed_sql_looking_mailbox (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking', 'account-1', 'object-sql-looking', 6, 6);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'mailbox', 'mailbox''; DROP TABLE messages; --', 2, "
      "123);");
  exec_sql (connection,
      "INSERT INTO mailbox_memberships (membership_id, account_id, "
      "mailbox_id, message_id, uid, internal_date_unix_us, journal_offset, "
      "journal_sequence, is_visible) VALUES "
      "('membership-sql-looking', 'account-1', "
      "'mailbox''; DROP TABLE messages; --', 'message-sql-looking', 6, 6, "
      "6, 6, TRUE);");
}

static void
init_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *account_id, const gchar *mailbox_id)
{
  const gchar *parameters[] = { mailbox_id, NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "mailbox.uid_map.v1", account_id, parameters, &error));
  g_assert_no_error (error);
}

static gchar *
dispatch_csv (const gchar *path, const gchar *account_id,
    const gchar *mailbox_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_request (&request, account_id, mailbox_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static void
test_duckdb_service_returns_uid_map_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_csv (path, "account-1", "mailbox-inbox");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox-inbox,77,1,message-a,object-a\n"
      "account-1,mailbox-inbox,77,2,message-b,object-b\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_empty_result_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_csv (path, "account-1", "mailbox-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_csv (path, "account-2", "mailbox-inbox");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-2,mailbox-inbox,99,5,message-other-account,"
      "object-other-account\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_mailbox_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_mailbox (path);

  csv = dispatch_csv (path, "account-1", "mailbox'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox'; DROP TABLE messages; --,123,6,"
      "message-sql-looking,object-sql-looking\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_missing_path_fails (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_null (service);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/returns-uid-map-csv",
      test_duckdb_service_returns_uid_map_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/empty-result-header-only",
      test_duckdb_service_empty_result_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/cross-account-isolation",
      test_duckdb_service_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-mailbox-value",
      test_duckdb_service_treats_sql_looking_mailbox_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/missing-path-fails",
      test_duckdb_service_missing_path_fails);

  return g_test_run ();
}
