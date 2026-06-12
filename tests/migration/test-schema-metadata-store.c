/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-build-config.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  const gchar *name;
  const gchar *type;
  gboolean not_null;
} TestDuckdbBootstrapColumn;

typedef char *TestDuckdbOwnedString;

static void
remove_directory_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  if (path == NULL)
    return;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);
    remove_directory_tree (child);
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
duckdb_owned_string_clear (char **value)
{
  if (value != NULL && *value != NULL)
    duckdb_free (*value);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TestDuckdbOwnedString,
    duckdb_owned_string_clear)
/* *INDENT-ON* */

static void
assert_bootstrap_table_schema (duckdb_connection connection,
    const gchar *table_name, const TestDuckdbBootstrapColumn *columns,
    gsize column_count)
{
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *query = NULL;
  idx_t row_count = 0;

  query = g_strdup_printf ("PRAGMA table_info('%s');", table_name);
  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);
  g_assert_cmpint (duckdb_column_count (&result), ==, 6);
  g_assert_cmpuint (duckdb_row_count (&result), ==, column_count);

  row_count = duckdb_row_count (&result);
  for (idx_t row = 0; row < row_count; row++) {
    g_auto (TestDuckdbOwnedString) actual_name =
        duckdb_value_varchar (&result, 1, row);
    g_auto (TestDuckdbOwnedString) actual_type =
        duckdb_value_varchar (&result, 2, row);
    gboolean actual_not_null = duckdb_value_int64 (&result, 3, row);

    g_assert_cmpstr (actual_name, ==, columns[row].name);
    g_assert_cmpstr (actual_type, ==, columns[row].type);
    g_assert_cmpint (actual_not_null, ==, columns[row].not_null ? 1 : 0);
  }
}

static void
assert_bootstrap_catalog_schema (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  static const TestDuckdbBootstrapColumn accounts_columns[] = {
    {"account_id", "VARCHAR", TRUE},
  };
  static const TestDuckdbBootstrapColumn objects_columns[] = {
    {"object_id", "VARCHAR", TRUE},
    {"size_bytes", "UBIGINT", TRUE},
  };
  static const TestDuckdbBootstrapColumn messages_columns[] = {
    {"message_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"object_id", "VARCHAR", TRUE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const TestDuckdbBootstrapColumn mailboxes_columns[] = {
    {"mailbox_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"imap_name", "VARCHAR", TRUE},
    {"is_selectable", "BOOLEAN", TRUE},
    {"is_visible", "BOOLEAN", TRUE},
  };
  static const TestDuckdbBootstrapColumn mailbox_memberships_columns[] = {
    {"membership_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"mailbox_id", "VARCHAR", TRUE},
    {"message_id", "VARCHAR", TRUE},
    {"uid", "UBIGINT", TRUE},
    {"internal_date_unix_us", "UBIGINT", TRUE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
    {"is_visible", "BOOLEAN", TRUE},
  };
  static const TestDuckdbBootstrapColumn derived_views_columns[] = {
    {"view_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"imap_name", "VARCHAR", TRUE},
    {"definition_ref", "VARCHAR", TRUE},
    {"is_selectable", "BOOLEAN", TRUE},
    {"is_visible", "BOOLEAN", TRUE},
  };
  static const TestDuckdbBootstrapColumn mailbox_uid_state_columns[] = {
    {"account_id", "VARCHAR", TRUE},
    {"namespace_kind", "VARCHAR", TRUE},
    {"namespace_id", "VARCHAR", TRUE},
    {"uidnext", "UBIGINT", TRUE},
    {"uidvalidity", "UBIGINT", TRUE},
  };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_table_schema (connection, "accounts",
      accounts_columns, G_N_ELEMENTS (accounts_columns));
  assert_bootstrap_table_schema (connection, "objects",
      objects_columns, G_N_ELEMENTS (objects_columns));
  assert_bootstrap_table_schema (connection, "messages",
      messages_columns, G_N_ELEMENTS (messages_columns));
  assert_bootstrap_table_schema (connection, "mailboxes",
      mailboxes_columns, G_N_ELEMENTS (mailboxes_columns));
  assert_bootstrap_table_schema (connection, "mailbox_memberships",
      mailbox_memberships_columns, G_N_ELEMENTS (mailbox_memberships_columns));
  assert_bootstrap_table_schema (connection, "derived_views",
      derived_views_columns, G_N_ELEMENTS (derived_views_columns));
  assert_bootstrap_table_schema (connection, "mailbox_uid_state",
      mailbox_uid_state_columns, G_N_ELEMENTS (mailbox_uid_state_columns));

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_bootstrap_query_succeeds (duckdb_connection connection,
    const gchar *query)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);
}

static void
assert_bootstrap_query_fails (duckdb_connection connection, const gchar *query)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, query, &result), ==, DuckDBError);
}

static gboolean
duckdb_table_exists (duckdb_connection connection, const gchar *table_name)
{
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *query = NULL;

  query = g_strdup_printf ("SELECT COUNT(*) FROM information_schema.tables "
      "WHERE table_name = '%s';", table_name);
  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);

  return duckdb_value_uint64 (&result, 0, 0) == 1;
}

static void
assert_mailbox_membership_constraints (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-1', 'account-1', 'mailbox-1', 'message-1', 1, "
      "1000, 2000, 1, TRUE" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-shared-journal-other-mailbox', 'account-1', 'mailbox-2', "
      "'message-2', 1, 1001, 2000, 1, TRUE" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-duplicate-journal-same-mailbox', 'account-1', "
      "'mailbox-2', 'message-3', 2, 1002, 2000, 1, TRUE" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-1', 'account-1', 'mailbox-3', 'message-4', 1, "
      "1003, 2001, 1, TRUE" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-uid-zero', 'account-1', 'mailbox-2', 'message-2', 0, "
      "1004, 2002, 1, TRUE" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'membership-duplicate-uid', 'account-1', 'mailbox-1', 'message-2', 1, "
      "1005, 2003, 1, TRUE" ");");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_attribute_tables_exist (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_true (duckdb_table_exists (connection, "message_flags"));
  g_assert_true (duckdb_table_exists (connection, "message_keywords"));

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_table_exists (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  static const TestDuckdbBootstrapColumn message_headers_columns[] = {
    {"message_id", "VARCHAR", TRUE},
    {"rfc_message_id", "VARCHAR", FALSE},
    {"duplicate_message_id_count", "UBIGINT", TRUE},
    {"subject", "VARCHAR", FALSE},
    {"from_addr", "VARCHAR", FALSE},
    {"to_addr", "VARCHAR", FALSE},
    {"cc_addr", "VARCHAR", FALSE},
    {"bcc_addr", "VARCHAR", FALSE},
    {"date_raw", "VARCHAR", FALSE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_true (duckdb_table_exists (connection, "message_headers"));
  assert_bootstrap_table_schema (connection, "message_headers",
      message_headers_columns, G_N_ELEMENTS (message_headers_columns));
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO message_headers ("
      "message_id, duplicate_message_id_count, journal_offset, journal_sequence"
      ") VALUES ('message-1', 0, 10, 1);");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO message_headers ("
      "message_id, duplicate_message_id_count, journal_offset, journal_sequence"
      ") VALUES ('message-1', 0, 10, 1);");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO message_headers ("
      "message_id, duplicate_message_id_count, journal_offset, journal_sequence"
      ") VALUES ('message-2', NULL, 11, 2);");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_table_missing (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_false (duckdb_table_exists (connection, "message_headers"));

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_attribute_tables_missing (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_false (duckdb_table_exists (connection, "message_flags"));
  g_assert_false (duckdb_table_exists (connection, "message_keywords"));

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
set_materialization_checkpoint_fields (WyreboxSchemaMigrationMetadataState
    *state)
{
  g_assert_nonnull (state);

  state->materialization_checkpoint_present = TRUE;
  state->materialization_checkpoint_journal_offset = 8192;
  state->materialization_checkpoint_sequence = 1234;
}

static void
test_missing_metadata_load (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_false (loaded.schema_version_present);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
}

static void
test_save_and_load_schema_version_roundtrip (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();

  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, original.schema_version);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
}

static void
test_save_and_load_materialization_checkpoint_roundtrip (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  set_materialization_checkpoint_fields (&original);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      original.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      original.materialization_checkpoint_sequence);
  g_assert_cmpuint (loaded.schema_version, ==, original.schema_version);
}

static void
test_transient_checkpoint_precondition_is_not_persisted (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  original.checkpoint_precondition_satisfied = TRUE;

  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
}

static void
test_save_failure_preserves_prior_state (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base_state = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) failed_state = { 0 };

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  base_state.schema_version_present = TRUE;
  base_state.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  set_materialization_checkpoint_fields (&base_state);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base_state,
          &error));
  g_assert_no_error (error);

  failed_state.schema_version_present = TRUE;
  failed_state.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  set_materialization_checkpoint_fields (&failed_state);
  failed_state.materialization_checkpoint_sequence = 5000;
  failed_state.materialization_checkpoint_journal_offset = 10000;

  wyrebox_schema_metadata_store_memory_set_next_save_failure (store, TRUE);
  g_assert_false (wyrebox_schema_metadata_store_save (store, &failed_state,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);

  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base_state.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base_state.materialization_checkpoint_sequence);
  g_assert_cmpuint (loaded.schema_version, ==, base_state.schema_version);
}

static void
test_memory_store_accepts_legacy_bootstrap_migration_operation (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
}

static void
    test_memory_store_accepts_add_message_attribute_tables_migration_operation
    (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          2, &error));
  g_assert_no_error (error);
}

static void
test_memory_store_accepts_add_message_header_table_migration_operation (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
}

static void
test_memory_store_rejects_unknown_migration_operation (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_memory ();
  g_assert_nonnull (store);

  g_assert_false (wyrebox_schema_metadata_store_apply_migration_operation
      (store, (WyreboxSchemaMetadataStoreMigrationOperation) 999, 0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static char *
make_duckdb_path (char **out_root)
{
  g_autofree char *root = NULL;

  g_assert_nonnull (out_root);

  root = g_dir_make_tmp ("wyrebox-schema-metadata-store-XXXXXX", NULL);
  g_assert_nonnull (root);

  *out_root = g_steal_pointer (&root);
  return g_build_filename (*out_root, "schema.duckdb", NULL);
}

static void
test_duckdb_store_missing_metadata_load (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_false (loaded.schema_version_present);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_accepts_legacy_bootstrap_migration_operation (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_add_message_attribute_tables_migration_operation (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (), 2,
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (), 2,
          &error));
  g_assert_no_error (error);

  assert_message_attribute_tables_exist (path);
  assert_message_header_table_missing (path);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_add_message_header_table_migration_operation (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);

  assert_message_header_table_exists (path);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_legacy_bootstrap_creates_catalog_tables (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);

  assert_bootstrap_catalog_schema (path);
  assert_mailbox_membership_constraints (path);
  assert_message_attribute_tables_missing (path);
  assert_message_header_table_missing (path);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_legacy_bootstrap_creates_catalog_tables_twice (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0,
          wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  assert_message_attribute_tables_missing (path);
  assert_message_header_table_missing (path);

  assert_bootstrap_catalog_schema (path);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_schema_version_roundtrip (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);

  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, original.schema_version);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_materialization_checkpoint_roundtrip (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  set_materialization_checkpoint_fields (&original);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);

  g_assert_true (loaded.schema_version_present);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      original.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      original.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_transient_precondition_not_persisted (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  original.checkpoint_precondition_satisfied = TRUE;
  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);

  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, original.schema_version);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_duckdb_store_cleared_state_removes_persisted_rows (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) original = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) cleared = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_autoptr (GError) error = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  original.schema_version_present = TRUE;
  original.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  set_materialization_checkpoint_fields (&original);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &original, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &cleared, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);

  g_assert_false (loaded.schema_version_present);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/migration/schema-metadata-store/missing-metadata-load",
      test_missing_metadata_load);
  g_test_add_func ("/migration/schema-metadata-store/schema-version-roundtrip",
      test_save_and_load_schema_version_roundtrip);
  g_test_add_func
      ("/migration/schema-metadata-store/materialization-checkpoint-roundtrip",
      test_save_and_load_materialization_checkpoint_roundtrip);
  g_test_add_func
      ("/migration/schema-metadata-store/transient-precondition-not-persisted",
      test_transient_checkpoint_precondition_is_not_persisted);
  g_test_add_func
      ("/migration/schema-metadata-store/save-failure-preserves-state",
      test_save_failure_preserves_prior_state);
  g_test_add_func ("/migration/schema-metadata-store/"
      "memory-store-accepts-legacy-bootstrap-operation",
      test_memory_store_accepts_legacy_bootstrap_migration_operation);
  g_test_add_func ("/migration/schema-metadata-store/"
      "memory-store-accepts-add-message-attribute-tables-operation",
      test_memory_store_accepts_add_message_attribute_tables_migration_operation);
  g_test_add_func ("/migration/schema-metadata-store/"
      "memory-store-accepts-add-message-header-table-operation",
      test_memory_store_accepts_add_message_header_table_migration_operation);
  g_test_add_func ("/migration/schema-metadata-store/"
      "memory-store-rejects-unknown-operation",
      test_memory_store_rejects_unknown_migration_operation);
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-store/missing-metadata-load",
      test_duckdb_store_missing_metadata_load);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "accepts-legacy-bootstrap-operation",
      test_duckdb_store_accepts_legacy_bootstrap_migration_operation);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "add-message-attribute-tables-operation",
      test_duckdb_store_add_message_attribute_tables_migration_operation);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "add-message-header-table-operation",
      test_duckdb_store_add_message_header_table_migration_operation);
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-store/"
      "legacy-bootstrap-creates-catalog-tables",
      test_duckdb_store_legacy_bootstrap_creates_catalog_tables);
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-store/"
      "legacy-bootstrap-creates-catalog-tables-twice",
      test_duckdb_store_legacy_bootstrap_creates_catalog_tables_twice);
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-store/schema-version-roundtrip",
      test_duckdb_store_schema_version_roundtrip);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "materialization-checkpoint-roundtrip",
      test_duckdb_store_materialization_checkpoint_roundtrip);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "transient-precondition-not-persisted",
      test_duckdb_store_transient_precondition_not_persisted);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "cleared-state-removes-persisted-rows",
      test_duckdb_store_cleared_state_removes_persisted_rows);

  return g_test_run ();
}
