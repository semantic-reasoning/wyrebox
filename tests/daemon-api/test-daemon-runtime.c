#include "wyrebox-daemon-runtime.h"
#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

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
create_catalog_path (void)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;

  dir = g_dir_make_tmp ("wyrebox-daemon-runtime-catalog-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  migration = wyrebox_schema_migration_new ();
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_no_error (error);

  return g_steal_pointer (&path);
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
