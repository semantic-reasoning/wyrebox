#include "wyrebox-daemon-duckdb-query-template-dispatcher.h"
#include "wyrebox-daemon-duckdb-query-template-service.h"
#include "wyrebox-benchmark-report.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

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
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
exec_sql (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
}

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

typedef struct
{
  gchar *path;
} TempCatalogRoot;

static void
temp_catalog_root_clear (TempCatalogRoot *root)
{
  if (root == NULL)
    return;

  if (root->path != NULL)
    remove_tree (root->path);
  g_clear_pointer (&root->path, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TempCatalogRoot, temp_catalog_root_clear)
/* *INDENT-ON* */

static gchar *
create_temp_catalog_root (void)
{
  return g_dir_make_tmp ("wyrebox-benchmark-duckdb-query-template-XXXXXX",
      NULL);
}

static gsize
count_substring (const gchar *haystack, const gchar *needle)
{
  gsize count = 0;
  gsize needle_length = strlen (needle);
  const gchar *cursor = haystack;

  while ((cursor = g_strstr_len (cursor, -1, needle)) != NULL) {
    count++;
    cursor += needle_length;
  }

  return count;
}

static void
bootstrap_catalog (const gchar *path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, 3, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
          5, 6, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US,
          6, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation
      (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS,
          8, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
}

static void
seed_catalog (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  for (guint i = 0; i < 256; i++) {
    g_autoptr (GString) sql = NULL;

    sql = g_string_new (NULL);
    g_string_append_printf (sql,
        "INSERT INTO messages (message_id, account_id, object_id, "
        "journal_offset, journal_sequence) VALUES "
        "('message-volume-%03u', 'account-1', 'object-volume-%03u', "
        "%" G_GUINT64_FORMAT ", %" G_GUINT64_FORMAT ");",
        i, i, (guint64) 1000 + i, (guint64) 1000 + i);
    exec_sql (connection, sql->str);

    g_string_truncate (sql, 0);
    g_string_append_printf (sql,
        "INSERT INTO message_headers ("
        "message_id, rfc_message_id, duplicate_message_id_count, subject, "
        "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
        "journal_sequence) VALUES "
        "('message-volume-%03u', '<message-volume-%03u@example.test>', 0, "
        "'High Volume Subject', 'Sender %03u <sender%03u@example.test>', "
        "'Bob <bob@example.test>', NULL, NULL, "
        "'Mon, 01 Jan 2024 00:00:00 +0000', "
        "%" G_GUINT64_FORMAT ", %" G_GUINT64_FORMAT ");",
        i, i, i, i, (guint64) 2000 + i, (guint64) 2000 + i);
    exec_sql (connection, sql->str);
  }

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-cross-account', 'account-2', 'object-cross-account', "
      "3000, 3000),"
      "('message-nonmatching', 'account-1', 'object-nonmatching', "
      "3001, 3001),"
      "('message-null-subject', 'account-1', 'object-null-subject', "
      "3002, 3002);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-cross-account', '<message-cross-account@example.test>', 0, "
      "'High Volume Subject', 'Sender 900 <sender900@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 4000, 4000),"
      "('message-nonmatching', '<message-nonmatching@example.test>', 0, "
      "'High Noise Subject', 'Sender 901 <sender901@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 4001, 4001),"
      "('message-null-subject', '<message-null-subject@example.test>', 0, "
      "NULL, 'Sender 902 <sender902@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 4002, 4002);");
}

static void
init_messages_subject_contains_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *subject_term,
    const gchar *limit, const gchar *offset)
{
  const gchar *parameters[] = { subject_term, limit, offset, NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.subject_contains.v1", account_id, parameters,
          &error));
  g_assert_no_error (error);
}

static void
run_microbenchmark (void)
{
  g_autofree char *catalog_path = NULL;
  g_auto (TempCatalogRoot) temp_root = { 0 };
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  g_auto (WyreboxBenchmarkReport) report = { 0 };
  gconstpointer data = NULL;
  gsize size = 0;
  gint64 start_us = 0;
  gint64 end_us = 0;
  g_autofree gchar *csv = NULL;

  temp_root.path = create_temp_catalog_root ();
  g_assert_nonnull (temp_root.path);
  catalog_path = g_build_filename (temp_root.path, "catalog.duckdb", NULL);

  bootstrap_catalog (catalog_path);
  seed_catalog (catalog_path);

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb
      (catalog_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_subject_contains_request (&request, "account-1", "volume",
      "10", "95");

  wyrebox_benchmark_report_init (&report);
  start_us = g_get_monotonic_time ();
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", "account-1", "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  end_us = g_get_monotonic_time ();
  g_assert_no_error (error);

  report.elapsed_us = end_us - start_us;
  wyrebox_benchmark_report_capture_rusage (&report);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  g_assert_cmpuint (size, >, 0);
  csv = g_strndup (data, size);
  g_assert_nonnull (csv);
  g_assert_true (g_str_has_prefix (csv,
          "account_id,message_id,object_id,message_journal_offset,"));
  g_assert_cmpuint (count_substring (csv, "object-volume-"), ==, 10);
  g_assert_nonnull (g_strstr_len (csv, -1, "message-volume-095"));
  g_assert_nonnull (g_strstr_len (csv, -1, "message-volume-104"));
  g_assert_null (g_strstr_len (csv, -1, "message-volume-094"));
  g_assert_null (g_strstr_len (csv, -1, "message-volume-105"));
  g_assert_null (g_strstr_len (csv, -1, "message-cross-account"));
  g_assert_null (g_strstr_len (csv, -1, "message-nonmatching"));
  g_assert_null (g_strstr_len (csv, -1, "message-null-subject"));

  report.object_count = count_substring (csv, "object-volume-");
  report.disk_bytes = size;
  wyrebox_benchmark_report_print_json ("duckdb-query-template",
      "messages-subject-contains", &report);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  run_microbenchmark ();

  return 0;
}
