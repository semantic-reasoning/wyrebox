#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-journal-reader.h"
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

  dir = g_dir_make_tmp ("wyrebox-fact-mutation-service-catalog-XXXXXX", &error);
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

static char *
create_unmigrated_catalog_path (void)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  dir = g_dir_make_tmp ("wyrebox-fact-mutation-service-catalog-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
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
seed_materialization_catalog (const char *catalog_path, const char *account_id)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree char *sql = NULL;

  open_duckdb_fixture (catalog_path, &fixture);
  sql = g_strdup_printf ("INSERT INTO accounts (account_id) VALUES ('%s');",
      account_id);
  execute_sql (fixture.connection, sql);
  g_clear_pointer (&sql, g_free);
  sql =
      g_strdup_printf
      ("INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('mail-1', '%s', 'object-1', 1, 1),"
      "('mail-2', '%s', 'object-2', 2, 2);", account_id, account_id);
  execute_sql (fixture.connection, sql);
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
assert_journal_is_empty (const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
assert_journal_events (const char *root,
    const WyreboxJournalEventType *event_types, guint n_event_types,
    WyreboxDaemonAuditPayload *out_audit)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);

  for (guint i = 0; i < n_event_types; i++) {
    g_auto (WyreboxJournalRecord) record = { 0 };

    g_assert_true (wyrebox_journal_reader_read_next (reader,
            &record, &eof, &error));
    g_assert_no_error (error);
    g_assert_false (eof);
    g_assert_cmpint (record.event_type, ==, event_types[i]);
    g_assert_cmpuint (record.sequence, ==, i + 1);

    if (record.event_type == WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED &&
        out_audit != NULL) {
      g_assert_true (wyrebox_daemon_audit_payload_decode (record.payload,
              out_audit, &error));
      g_assert_no_error (error);
    }
  }

  g_auto (WyreboxJournalRecord) record = { 0 };
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
test_fact_mutation_service_handles_request (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  g_auto (WyreboxJournalRecord) audit_record = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };
  gboolean eof = FALSE;

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_INSERTED);
  g_assert_cmpuint (record.sequence, ==, 1);

  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &audit_record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (audit_record.event_type, ==,
      WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED);
  g_assert_cmpuint (audit_record.sequence, ==, 2);
  g_assert_true (wyrebox_daemon_audit_payload_decode (audit_record.payload,
          &audit, &error));
  g_assert_no_error (error);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS);
  g_assert_cmpstr (audit.request_id, ==, "request-1");
  g_assert_cmpstr (audit.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (audit.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.tool_identity, ==, "fact-importer");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpuint (audit.mutation_count, ==, 1);
  g_assert_cmpstr (audit.predicate_id, ==, "project_mention");
  g_assert_cmpuint (audit.final_journal_offset, ==,
      frame.success.journal_offset);
  g_assert_cmpuint (audit.final_journal_sequence, ==,
      frame.success.journal_sequence);

  wyrebox_journal_record_clear (&record);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);

  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_identity_without_caller (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", NULL, "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_unauthorized_identity (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "dovecot", "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
assert_fact_mutation_service_rejects_caller (const char *caller_identity)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          caller_identity, "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_legacy_skill_identity (void)
{
  assert_fact_mutation_service_rejects_caller ("skill");
}

static void
test_fact_mutation_service_rejects_admin_cli_identity (void)
{
  assert_fact_mutation_service_rejects_caller ("admin-cli");
}

static void
test_fact_mutation_service_rejects_identity_without_account_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", NULL, "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_mismatched_account_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-2", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_null_identity_request (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "*request != NULL*");
  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, NULL, &frame, &error));
  g_test_assert_expected_messages ();
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_handles_identity (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");

  remove_tree (root);
}

static void
test_fact_mutation_service_materializes_configured_wirelog_view (void)
{
  const WyreboxJournalEventType expected[] = {
    WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
    WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
    WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED,
  };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autofree char *catalog_path = create_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_nonnull (root);
  seed_materialization_catalog (catalog_path, "account-1");
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_keyword", "account-1",
          (const char *[]) { "mail-1", "view-projects", NULL }, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &mutation, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  assert_journal_events (root, expected, G_N_ELEMENTS (expected), NULL);
  g_assert_cmpuint (query_catalog_uint64 (catalog_path,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE account_id = 'account-1' "
          "AND view_id = 'view-projects' " "AND is_visible = TRUE;"), ==, 1);

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (catalog_path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (catalog);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, "Projects", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_no_error (error);
  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_name, ==, "Projects");
  g_assert_cmpuint (result.message_count, ==, 1);
  g_assert_cmpuint (result.uid_next, ==, 2);

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

static void
test_fact_mutation_service_keeps_success_when_materialization_fails (void)
{
  const WyreboxJournalEventType expected[] = {
    WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
    WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
  };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autofree char *catalog_path = create_unmigrated_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_keyword", "account-1",
          (const char *[]) { "mail-1", "view-projects", NULL }, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);
  g_assert_true
      (wyrebox_daemon_fact_mutation_service_configure_wirelog_derived_view
      (service, root, catalog_path, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &mutation, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);
  assert_journal_events (root, expected, G_N_ELEMENTS (expected), &audit);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION);
  g_assert_cmpstr (audit.request_id, ==, "request-1");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpstr (audit.predicate_id, ==, "project_keyword");
  g_assert_cmpuint (audit.final_journal_sequence, ==, 1);

  remove_tree (root);
  remove_catalog_path (catalog_path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-mutation-service/handles-request",
      test_fact_mutation_service_handles_request);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-identity-without-caller",
      test_fact_mutation_service_rejects_identity_without_caller);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-unauthorized-identity",
      test_fact_mutation_service_rejects_unauthorized_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-legacy-skill-identity",
      test_fact_mutation_service_rejects_legacy_skill_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-admin-cli-identity",
      test_fact_mutation_service_rejects_admin_cli_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-identity-without-account-scope",
      test_fact_mutation_service_rejects_identity_without_account_scope);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-mismatched-account-scope",
      test_fact_mutation_service_rejects_mismatched_account_scope);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-null-identity-request",
      test_fact_mutation_service_rejects_null_identity_request);
  g_test_add_func ("/daemon-api/fact-mutation-service/handles-identity",
      test_fact_mutation_service_handles_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "materializes-configured-wirelog-view",
      test_fact_mutation_service_materializes_configured_wirelog_view);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "keeps-success-when-materialization-fails",
      test_fact_mutation_service_keeps_success_when_materialization_fails);

  return g_test_run ();
}
