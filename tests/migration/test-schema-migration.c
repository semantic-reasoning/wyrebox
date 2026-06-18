/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

#include <duckdb.h>
#include <glib.h>
#include <gio/gio.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  guint operation_call_count;
  guint validation_call_count;
  guint expected_fail_at_call;
} TestMigrationFixtureData;

typedef char *TestDuckdbOwnedString;

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

static char *
make_duckdb_path (char **out_root)
{
  g_autofree char *root = NULL;

  g_assert_nonnull (out_root);

  root = g_dir_make_tmp ("wyrebox-schema-migration-XXXXXX", NULL);
  g_assert_nonnull (root);

  *out_root = g_steal_pointer (&root);
  return g_build_filename (*out_root, "schema.duckdb", NULL);
}

static char *
journal_segment_path (const char *root)
{
  return g_build_filename (root, "00000000000000000000.wbj", NULL);
}

static void
read_journal_segment (const char *journal_root, guint8 **out_data,
    gsize *out_size)
{
  g_autofree char *segment_path = journal_segment_path (journal_root);
  g_autofree gchar *contents = NULL;

  g_assert_true (g_file_get_contents (segment_path, &contents, out_size, NULL));
  g_assert_nonnull (contents);
  *out_data = (guint8 *) g_steal_pointer (&contents);
}

static void
write_journal_segment (const char *journal_root, const guint8 *data, gsize size)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *segment_path = journal_segment_path (journal_root);

  g_assert_true (g_file_set_contents (segment_path, (const gchar *) data,
          (gssize) size, &error));
  g_assert_no_error (error);
}

static void
corrupt_journal_checksum (const char *journal_root, guint64 record_offset)
{
  g_autofree guint8 *segment = NULL;
  gsize segment_size = 0;

  read_journal_segment (journal_root, &segment, &segment_size);
  g_assert_cmpuint (segment_size, >, record_offset + 32);
  segment[record_offset + 32] ^= 0x01;
  write_journal_segment (journal_root, segment, segment_size);
}

static void
append_single_journal_record (const char *journal_root,
    guint64 *out_offset, guint64 *out_sequence)
{
  static const guint8 payload[] = { 0x70, 0x72, 0x65, 0x66, 0x6c, 0x69, 0x67,
    0x68, 0x74
  };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GBytes) payload_bytes =
      g_bytes_new_static (payload, sizeof (payload));

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_journal_writer_append (writer,
          WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload_bytes,
          out_offset, out_sequence, &error));
  g_assert_no_error (error);
}

static void
assert_bootstrap_catalog_tables_exist (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  static const char *tables[] = { "accounts", "objects", "messages",
    "mailboxes", "mailbox_memberships", "derived_views",
    "derived_view_memberships", "mailbox_uid_state"
  };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  for (gsize i = 0; i < G_N_ELEMENTS (tables); i++) {
    duckdb_result result = { 0 };
    g_autofree gchar *query = NULL;

    query = g_strdup_printf ("PRAGMA table_info('%s');", tables[i]);
    g_assert_cmpint (duckdb_query (connection, query, &result), ==,
        DuckDBSuccess);
    duckdb_destroy_result (&result);
  }

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static gboolean
duckdb_table_exists (duckdb_connection connection, const gchar *table_name)
{
  duckdb_result result = { 0 };
  g_autofree gchar *query = NULL;
  gboolean exists = FALSE;

  query = g_strdup_printf ("SELECT COUNT(*) FROM information_schema.tables "
      "WHERE table_name = '%s';", table_name);
  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);
  exists = duckdb_value_uint64 (&result, 0, 0) == 1;
  duckdb_destroy_result (&result);

  return exists;
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

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_true (duckdb_table_exists (connection, "message_headers"));

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_sender_domain_column_exists (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM information_schema.columns "
          "WHERE table_name = 'message_headers' "
          "AND column_name = 'sender_domain';", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_sender_domain_column_missing (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM information_schema.columns "
          "WHERE table_name = 'message_headers' "
          "AND column_name = 'sender_domain';", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 0);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_sender_domain_backfilled (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id = 'message-mixed' "
          "AND sender_domain = 'example.test';", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id = 'message-malformed' "
          "AND sender_domain IS NULL;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_sender_domain_retry_rows_preserved (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 3);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id IN ("
          "'message-mixed', 'message-malformed', 'message-old-current-year'"
          ");", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 3);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM information_schema.tables "
          "WHERE table_name = 'message_headers_replacement';", &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 0);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_date_retry_rows_preserved (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 3);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id IN ("
          "'message-mixed', 'message-malformed', 'message-old-current-year'"
          ");", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 3);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM information_schema.tables "
          "WHERE table_name = 'message_headers_replacement';", &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 0);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_date_unix_us_column_exists (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM information_schema.columns "
          "WHERE table_name = 'message_headers' "
          "AND column_name = 'date_unix_us';", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_message_header_date_unix_us_backfilled (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id = 'message-mixed' "
          "AND date_unix_us = 1781258400000000;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id = 'message-malformed' "
          "AND date_unix_us IS NULL;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);
  memset (&result, 0, sizeof result);

  g_assert_cmpint (duckdb_query (connection,
          "SELECT COUNT(*) FROM message_headers "
          "WHERE message_id = 'message-old-current-year' "
          "AND date_unix_us IS NULL;", &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_value_uint64 (&result, 0, 0), ==, 1);
  duckdb_destroy_result (&result);

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
create_incompatible_message_flags_table (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  g_assert_cmpint (duckdb_query (connection,
          "DROP TABLE IF EXISTS message_flags;", &result), ==, DuckDBSuccess);
  duckdb_destroy_result (&result);
  g_assert_cmpint (duckdb_query (connection,
          "CREATE TABLE message_flags ("
          "flag_name VARCHAR NOT NULL, "
          "membership_id VARCHAR NOT NULL, "
          "account_id VARCHAR NOT NULL, "
          "mailbox_id VARCHAR NOT NULL, "
          "journal_offset UBIGINT NOT NULL, "
          "journal_sequence UBIGINT NOT NULL, "
          "PRIMARY KEY (flag_name, membership_id)"
          ");", &result), ==, DuckDBSuccess);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
create_incompatible_message_headers_table (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  g_assert_cmpint (duckdb_query (connection,
          "DROP TABLE IF EXISTS message_headers;", &result), ==, DuckDBSuccess);
  duckdb_destroy_result (&result);
  g_assert_cmpint (duckdb_query (connection,
          "CREATE TABLE message_headers ("
          "message_id VARCHAR PRIMARY KEY, "
          "subject VARCHAR NOT NULL" ");", &result), ==, DuckDBSuccess);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

typedef struct _TestSchemaMetadataStoreSpy TestSchemaMetadataStoreSpy;
typedef struct _TestSchemaMetadataStoreSpyClass TestSchemaMetadataStoreSpyClass;

struct _TestSchemaMetadataStoreSpy
{
  WyreboxSchemaMetadataStore parent_instance;

  gulong save_call_count;
  gboolean save_called;
  gboolean observed_checkpoint_precondition_satisfied;
  guint migration_operation_call_count;
  WyreboxSchemaMetadataStoreMigrationOperation observed_operations[9];
  gboolean fail_next_migration_operation;
  WyreboxSchemaMetadataStoreMigrationOperation observed_operation;
  guint64 observed_operation_source_version;
  guint64 observed_operation_target_version;

  gboolean has_state;
  gboolean fail_next_save;
  WyreboxSchemaMigrationMetadataState persisted_state;
};

struct _TestSchemaMetadataStoreSpyClass
{
  WyreboxSchemaMetadataStoreClass parent_class;
};

G_DEFINE_TYPE (TestSchemaMetadataStoreSpy, test_schema_metadata_store_spy,
    WYREBOX_TYPE_SCHEMA_METADATA_STORE);

static gboolean
test_schema_metadata_store_spy_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  g_return_val_if_fail (g_type_check_instance_is_a ((GTypeInstance *) self,
          test_schema_metadata_store_spy_get_type ()), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  TestSchemaMetadataStoreSpy *spy = (TestSchemaMetadataStoreSpy *) self;

  wyrebox_schema_migration_metadata_state_clear (out_state);
  if (!spy->has_state)
    return TRUE;

  *out_state = spy->persisted_state;
  out_state->checkpoint_precondition_satisfied = FALSE;
  return TRUE;
}

static gboolean
test_schema_metadata_store_spy_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  g_return_val_if_fail (g_type_check_instance_is_a ((GTypeInstance *) self,
          test_schema_metadata_store_spy_get_type ()), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  TestSchemaMetadataStoreSpy *spy = (TestSchemaMetadataStoreSpy *) self;

  spy->save_called = TRUE;
  spy->save_call_count += 1;
  spy->observed_checkpoint_precondition_satisfied =
      state->checkpoint_precondition_satisfied;

  if (spy->fail_next_save) {
    spy->fail_next_save = FALSE;
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "forced spy metadata store save failure");
    return FALSE;
  }

  spy->persisted_state = *state;
  spy->persisted_state.checkpoint_precondition_satisfied = FALSE;
  spy->has_state = TRUE;
  return TRUE;
}

static gboolean
    test_schema_metadata_store_spy_apply_migration_operation
    (WyreboxSchemaMetadataStore * self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version, guint64 target_version, GError ** error)
{
  TestSchemaMetadataStoreSpy *spy = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a ((GTypeInstance *) self,
          test_schema_metadata_store_spy_get_type ()), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  spy = (TestSchemaMetadataStoreSpy *) self;
  g_assert_cmpuint (spy->migration_operation_call_count, <,
      G_N_ELEMENTS (spy->observed_operations));
  spy->observed_operations[spy->migration_operation_call_count] = operation;
  spy->migration_operation_call_count += 1;
  spy->observed_operation = operation;
  spy->observed_operation_source_version = source_version;
  spy->observed_operation_target_version = target_version;

  if (spy->fail_next_migration_operation) {
    spy->fail_next_migration_operation = FALSE;
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "forced spy migration operation failure");
    return FALSE;
  }

  return operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP ||
      operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS
      || operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_OBJECT_REACHABILITY_VIEW;
}

static void
test_schema_metadata_store_spy_init (TestSchemaMetadataStoreSpy *self)
{
  self->save_called = FALSE;
  self->save_call_count = 0;
  self->observed_checkpoint_precondition_satisfied = FALSE;
  self->migration_operation_call_count = 0;
  memset (self->observed_operations, 0, sizeof self->observed_operations);
  self->fail_next_migration_operation = FALSE;
  self->observed_operation = 0;
  self->observed_operation_source_version = 0;
  self->observed_operation_target_version = 0;
  self->has_state = FALSE;
  self->fail_next_save = FALSE;
  wyrebox_schema_migration_metadata_state_clear (&self->persisted_state);
}

static void
test_schema_metadata_store_spy_class_init (TestSchemaMetadataStoreSpyClass
    *klass)
{
  WyreboxSchemaMetadataStoreClass *store_class =
      WYREBOX_SCHEMA_METADATA_STORE_CLASS (klass);

  store_class->load = test_schema_metadata_store_spy_load;
  store_class->save = test_schema_metadata_store_spy_save;
  store_class->apply_migration_operation =
      test_schema_metadata_store_spy_apply_migration_operation;
}

static TestSchemaMetadataStoreSpy *
test_schema_metadata_store_spy_new (void)
{
  return g_object_new (test_schema_metadata_store_spy_get_type (), NULL);
}

static void
    test_schema_migration_set_materialization_checkpoint_fields
    (WyreboxSchemaMigrationMetadataState * state)
{
  g_assert_nonnull (state);

  state->materialization_checkpoint_present = TRUE;
  state->materialization_checkpoint_journal_offset = 4096;
  state->materialization_checkpoint_sequence = 2048;
}

static gboolean
test_schema_migration_force_failure_for_call (guint64 source_version,
    guint64 target_version,
    guint64 expected_fail_at_call, guint *fail_triggered, GError **error)
{
  g_assert_nonnull (fail_triggered);
  *fail_triggered += 1;

  if (*fail_triggered == expected_fail_at_call) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "forced fixture failure for step %" G_GUINT64_FORMAT
        "->%" G_GUINT64_FORMAT, source_version, target_version);
    return FALSE;
  }

  return TRUE;
}

static gboolean
test_schema_migration_force_op_failure (guint64 source_version,
    guint64 target_version, gpointer user_data, GError **error)
{
  TestMigrationFixtureData *fixture_data = user_data;

  return test_schema_migration_force_failure_for_call (source_version,
      target_version,
      fixture_data->expected_fail_at_call,
      &fixture_data->operation_call_count, error);
}

static gboolean
test_schema_migration_force_validation_failure (guint64 source_version,
    guint64 target_version, gpointer user_data, GError **error)
{
  TestMigrationFixtureData *fixture_data = user_data;

  return test_schema_migration_force_failure_for_call (source_version,
      target_version,
      fixture_data->expected_fail_at_call,
      &fixture_data->validation_call_count, error);
}

static gboolean
test_schema_migration_record_step_calls (guint64 source_version,
    guint64 target_version, gpointer user_data, GError **error)
{
  TestMigrationFixtureData *fixture_data = user_data;

  g_assert_nonnull (fixture_data);

  fixture_data->operation_call_count += 1;
  if (target_version == source_version + 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "unexpected step %" G_GUINT64_FORMAT "->%" G_GUINT64_FORMAT,
      source_version, target_version);
  return FALSE;
}

static gboolean
test_schema_migration_record_validation_calls (guint64 source_version,
    guint64 target_version, gpointer user_data, GError **error)
{
  TestMigrationFixtureData *fixture_data = user_data;

  g_assert_nonnull (fixture_data);

  fixture_data->validation_call_count += 1;
  if (target_version == source_version + 1)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "unexpected step %" G_GUINT64_FORMAT "->%" G_GUINT64_FORMAT,
      source_version, target_version);
  return FALSE;
}

static void
    test_schema_migration_run_store_to_current_missing_metadata_roundtrips_to_current
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  guint64 expected_version = 0;

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();
  expected_version = wyrebox_schema_migration_get_current_schema_version ();

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, expected_version);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
}

static void
test_schema_migration_run_store_to_current_preserves_current_metadata (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();

  base.schema_version_present = TRUE;
  base.schema_version = wyrebox_schema_migration_get_current_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  wyrebox_schema_metadata_store_memory_set_next_save_failure (store, TRUE);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
}

static void
    test_schema_migration_run_store_to_current_transient_precondition_not_saved
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_called = FALSE;
  spy->save_call_count = 0;
  spy->observed_checkpoint_precondition_satisfied = FALSE;

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, TRUE, &error));
  g_assert_no_error (error);
  g_assert_true (spy->save_called);
  g_assert_cmpuint (spy->save_call_count, ==, 1);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 9);
  g_assert_cmpint (spy->observed_operation, ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS);
  g_assert_cmpuint (spy->observed_operation_source_version, ==, 8);
  g_assert_cmpuint (spy->observed_operation_target_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (spy->observed_checkpoint_precondition_satisfied);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (
          (WyreboxSchemaMetadataStore *) spy, &loaded, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);

  g_object_unref (spy);
}

static void
test_schema_migration_run_store_missing_metadata_applies_full_path (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, FALSE, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 9);
  g_assert_cmpint (spy->observed_operations[0], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP);
  g_assert_cmpint (spy->observed_operations[1], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES);
  g_assert_cmpint (spy->observed_operations[2], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE);
  g_assert_cmpint (spy->observed_operations[3], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS);
  g_assert_cmpint (spy->observed_operations[4], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT);
  g_assert_cmpint (spy->observed_operations[5], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN);
  g_assert_cmpint (spy->observed_operations[6], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US);
  g_assert_cmpint (spy->observed_operations[7], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_OBJECT_REACHABILITY_VIEW);
  g_assert_cmpint (spy->observed_operations[8], ==,
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS);
  g_assert_cmpuint (spy->save_call_count, ==, 1);

  g_object_unref (spy);
}

static void
test_schema_migration_run_store_current_metadata_skips_operation (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = wyrebox_schema_migration_get_current_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_call_count = 0;
  spy->save_called = FALSE;

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, FALSE, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 0);
  g_assert_cmpuint (spy->save_call_count, ==, 0);
  g_assert_false (spy->save_called);

  g_object_unref (spy);
}

static void
test_schema_migration_run_store_operation_failure_preserves_state (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_called = FALSE;
  spy->save_call_count = 0;
  spy->migration_operation_call_count = 0;
  spy->fail_next_migration_operation = TRUE;

  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 1);
  g_assert_false (spy->save_called);
  g_assert_cmpuint (spy->save_call_count, ==, 0);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (
          (WyreboxSchemaMetadataStore *) spy, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);

  g_object_unref (spy);
}

static void
test_schema_migration_run_store_to_current_future_metadata_rejected (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();

  base.schema_version_present = TRUE;
  base.schema_version =
      wyrebox_schema_migration_get_current_schema_version () + 1;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
}

static void
    test_schema_migration_run_store_to_current_legacy_without_checkpoint_fails
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, 0);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
}

static void
    test_schema_migration_run_store_to_current_legacy_with_checkpoint_succeeds
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version,
      ==, wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
}

static void
test_schema_migration_run_store_to_current_save_failure_preserves_state (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_memory ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  wyrebox_schema_metadata_store_memory_set_next_save_failure (store, TRUE);
  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
}

static void
    test_schema_migration_duckdb_run_store_missing_metadata_persists_current
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_bootstrap_catalog_tables_exist (path);
  assert_message_attribute_tables_exist (path);
  assert_message_header_table_exists (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v1_adds_message_attribute_tables
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_bootstrap_catalog_tables_exist (path);
  assert_message_attribute_tables_missing (path);
  assert_message_header_table_missing (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_attribute_tables_exist (path);
  assert_message_header_table_exists (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_schema_migration_duckdb_run_store_v2_adds_message_header_table (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 2;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (), 2,
          &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_bootstrap_catalog_tables_exist (path);
  assert_message_attribute_tables_exist (path);
  assert_message_header_table_missing (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_header_table_exists (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
assert_bootstrap_query_succeeds (duckdb_connection connection,
    const gchar *query)
{
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);
  duckdb_destroy_result (&result);
}

static void
assert_bootstrap_query_fails (duckdb_connection connection, const gchar *query)
{
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_query (connection, query, &result), ==, DuckDBError);
  duckdb_destroy_result (&result);
}

static void
assert_derived_view_memberships_table_shape (duckdb_connection connection)
{
  duckdb_result result = { 0 };
  static const gchar *expected_names[] = {
    "membership_id",
    "account_id",
    "view_id",
    "message_id",
    "uid",
    "is_visible",
    "rule_version_hash",
    "materialized_at_unix_us",
  };
  static const gchar *expected_types[] = {
    "VARCHAR",
    "VARCHAR",
    "VARCHAR",
    "VARCHAR",
    "UBIGINT",
    "BOOLEAN",
    "VARCHAR",
    "UBIGINT",
  };

  g_assert_cmpint (duckdb_query (connection,
          "PRAGMA table_info('derived_view_memberships');", &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_row_count (&result), ==,
      G_N_ELEMENTS (expected_names));

  for (guint row = 0; row < G_N_ELEMENTS (expected_names); row++) {
    g_auto (TestDuckdbOwnedString) name =
        duckdb_value_varchar (&result, 1, row);
    g_auto (TestDuckdbOwnedString) type =
        duckdb_value_varchar (&result, 2, row);
    const gint64 not_null = duckdb_value_int64 (&result, 3, row);

    g_assert_cmpstr (name, ==, expected_names[row]);
    g_assert_cmpstr (type, ==, expected_types[row]);
    g_assert_cmpint (not_null, ==, 1);
  }

  duckdb_destroy_result (&result);
}

static void
drop_derived_view_memberships_table (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;
  duckdb_result result = { 0 };

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_query (connection,
          "DROP TABLE derived_view_memberships;", &result), ==, DuckDBSuccess);
  duckdb_destroy_result (&result);

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_derived_view_memberships_table_exists (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);
  g_assert_true (duckdb_table_exists (connection, "derived_view_memberships"));
  assert_derived_view_memberships_table_shape (connection);

  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-1', 'account-1', 'view-1', 'message-1', 1, TRUE, "
      "'rule-hash-1', 1000" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-1', 'account-1', 'view-2', "
      "'message-2', 1, TRUE, 'rule-hash-1', 1001" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-duplicate-uid', 'account-1', 'view-1', "
      "'message-2', 1, TRUE, 'rule-hash-1', 1001" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-duplicate-message-rule', 'account-1', 'view-1', "
      "'message-1', 2, TRUE, 'rule-hash-1', 1002" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-uid-zero', 'account-1', 'view-1', 'message-3', "
      "0, TRUE, 'rule-hash-1', 1003" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-null-account', NULL, 'view-1', 'message-4', "
      "3, TRUE, 'rule-hash-1', 1004" ");");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static guint64
query_uint64 (duckdb_connection connection, const gchar *query)
{
  duckdb_result result = { 0 };
  guint64 value = 0;

  g_assert_cmpint (duckdb_query (connection, query, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);

  value = (guint64) duckdb_value_uint64 (&result, 0, 0);
  duckdb_destroy_result (&result);
  return value;
}

static void
create_old_v4_derived_view_identity_shape (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_query_succeeds (connection,
      "DROP TABLE derived_view_memberships;");
  assert_bootstrap_query_succeeds (connection, "DROP TABLE derived_views;");
  assert_bootstrap_query_succeeds (connection,
      "CREATE TABLE derived_views ("
      "view_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "imap_name VARCHAR NOT NULL,"
      "definition_ref VARCHAR NOT NULL,"
      "is_selectable BOOLEAN NOT NULL,"
      "is_visible BOOLEAN NOT NULL," "UNIQUE(account_id, imap_name)" ");");
  assert_bootstrap_query_succeeds (connection,
      "CREATE TABLE derived_view_memberships ("
      "membership_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "view_id VARCHAR NOT NULL,"
      "message_id VARCHAR NOT NULL,"
      "uid UBIGINT NOT NULL CHECK(uid >= 1),"
      "is_visible BOOLEAN NOT NULL,"
      "rule_version_hash VARCHAR NOT NULL,"
      "materialized_at_unix_us UBIGINT NOT NULL,"
      "UNIQUE(view_id, uid),"
      "UNIQUE(view_id, message_id, rule_version_hash)" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO derived_views ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") VALUES ("
      "'view-shared', 'account-1', 'Virtual/Important', "
      "'wirelog:important', TRUE, TRUE" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-preserved', 'account-1', 'view-shared', "
      "'message-shared', 7, TRUE, 'rule-hash-shared', 17000000" ");");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
assert_derived_view_identity_migrated_to_account_scope (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  g_assert_cmpuint (query_uint64 (connection,
          "SELECT COUNT(*) FROM derived_views "
          "WHERE account_id = 'account-1' AND view_id = 'view-shared' "
          "AND imap_name = 'Virtual/Important' "
          "AND definition_ref = 'wirelog:important' "
          "AND is_selectable AND is_visible;"), ==, 1);
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE membership_id = 'derived-membership-preserved' "
          "AND account_id = 'account-1' AND view_id = 'view-shared' "
          "AND message_id = 'message-shared' AND uid = 7 "
          "AND is_visible AND rule_version_hash = 'rule-hash-shared' "
          "AND materialized_at_unix_us = 17000000;"), ==, 1);

  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO derived_views ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") VALUES ("
      "'view-shared', 'account-2', 'Virtual/Important', "
      "'wirelog:important-account-2', TRUE, TRUE" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-account-2', 'account-2', 'view-shared', "
      "'message-shared', 7, TRUE, 'rule-hash-shared', 17000001" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_views ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") VALUES ("
      "'view-shared', 'account-1', 'Virtual/Other', "
      "'wirelog:other', TRUE, TRUE" ");");
  assert_bootstrap_query_fails (connection,
      "INSERT INTO derived_view_memberships ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") VALUES ("
      "'derived-membership-duplicate-uid', 'account-1', 'view-shared', "
      "'message-other', 7, TRUE, 'rule-hash-other', 17000002" ");");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
    test_schema_migration_duckdb_run_store_v4_scopes_derived_view_identity
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 4;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_old_v4_derived_view_identity_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_derived_view_identity_migrated_to_account_scope (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
create_old_v5_message_header_shape (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_query_succeeds (connection, "DROP TABLE message_headers;");
  assert_bootstrap_query_succeeds (connection,
      "CREATE TABLE message_headers ("
      "message_id VARCHAR PRIMARY KEY,"
      "rfc_message_id VARCHAR,"
      "duplicate_message_id_count UBIGINT NOT NULL,"
      "subject VARCHAR,"
      "from_addr VARCHAR,"
      "to_addr VARCHAR,"
      "cc_addr VARCHAR,"
      "bcc_addr VARCHAR,"
      "date_raw VARCHAR,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence"
      ") VALUES "
      "('message-mixed', '<message-mixed@example.test>', 0, "
      "'Mixed sender', 'Sender <SENDER@Example.TEST>', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 2026 05:00:00 -0500', 100, 1),"
      "('message-malformed', '<message-malformed@example.test>', 0, "
      "'Malformed sender', 'no-domain-address', "
      "'to@example.test', NULL, NULL, NULL, 101, 2),"
      "('message-old-current-year', '<message-old@example.test>', 0, "
      "'Old current year', 'sender@example.test', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 1899 05:00:00 -0500', 102, 3);");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
create_retry_v6_message_header_shape (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_query_succeeds (connection, "DROP TABLE message_headers;");
  assert_bootstrap_query_succeeds (connection,
      "CREATE TABLE message_headers ("
      "message_id VARCHAR PRIMARY KEY,"
      "rfc_message_id VARCHAR,"
      "duplicate_message_id_count UBIGINT NOT NULL,"
      "subject VARCHAR,"
      "from_addr VARCHAR,"
      "sender_domain VARCHAR,"
      "to_addr VARCHAR,"
      "cc_addr VARCHAR,"
      "bcc_addr VARCHAR,"
      "date_raw VARCHAR,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
      "journal_offset, journal_sequence"
      ") VALUES "
      "('message-mixed', '<message-mixed@example.test>', 0, "
      "'Mixed sender', 'Sender <SENDER@Example.TEST>', 'example.test', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 2026 05:00:00 -0500', 100, 1),"
      "('message-malformed', '<message-malformed@example.test>', 0, "
      "'Malformed sender', 'no-domain-address', NULL, "
      "'to@example.test', NULL, NULL, NULL, 101, 2),"
      "('message-old-current-year', '<message-old@example.test>', 0, "
      "'Old current year', 'sender@example.test', 'example.test', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 1899 05:00:00 -0500', 102, 3);");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
create_retry_current_message_header_shape (const gchar *path)
{
  duckdb_database database = NULL;
  duckdb_connection connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  assert_bootstrap_query_succeeds (connection, "DROP TABLE message_headers;");
  assert_bootstrap_query_succeeds (connection,
      "CREATE TABLE message_headers ("
      "message_id VARCHAR PRIMARY KEY,"
      "rfc_message_id VARCHAR,"
      "duplicate_message_id_count UBIGINT NOT NULL,"
      "subject VARCHAR,"
      "from_addr VARCHAR,"
      "sender_domain VARCHAR,"
      "to_addr VARCHAR,"
      "cc_addr VARCHAR,"
      "bcc_addr VARCHAR,"
      "date_raw VARCHAR,"
      "date_unix_us BIGINT,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");");
  assert_bootstrap_query_succeeds (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
      "date_unix_us, journal_offset, journal_sequence"
      ") VALUES "
      "('message-mixed', '<message-mixed@example.test>', 0, "
      "'Mixed sender', 'Sender <SENDER@Example.TEST>', 'example.test', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 2026 05:00:00 -0500', 1781258400000000, 100, 1),"
      "('message-malformed', '<message-malformed@example.test>', 0, "
      "'Malformed sender', 'no-domain-address', NULL, "
      "'to@example.test', NULL, NULL, NULL, NULL, 101, 2),"
      "('message-old-current-year', '<message-old@example.test>', 0, "
      "'Old current year', 'sender@example.test', 'example.test', "
      "'to@example.test', NULL, NULL, "
      "'Fri, 12 Jun 1899 05:00:00 -0500', NULL, 102, 3);");

  (void) duckdb_disconnect (&connection);
  (void) duckdb_close (&database);
}

static void
test_schema_migration_duckdb_run_store_v5_adds_sender_domain (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 5;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
          4, 5, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_old_v5_message_header_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_header_sender_domain_column_exists (path);
  assert_message_header_sender_domain_backfilled (path);
  assert_message_header_date_unix_us_column_exists (path);
  assert_message_header_date_unix_us_backfilled (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v5_without_checkpoint_preserves_state
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 5;
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
          4, 5, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_old_v5_message_header_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error (&error);
  g_clear_object (&store);

  assert_message_header_sender_domain_column_missing (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v5_sender_domain_retry_completes
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 5;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
          4, 5, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_retry_v6_message_header_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_header_sender_domain_column_exists (path);
  assert_message_header_sender_domain_backfilled (path);
  assert_message_header_sender_domain_retry_rows_preserved (path);
  assert_message_header_date_unix_us_column_exists (path);
  assert_message_header_date_unix_us_backfilled (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v5_current_header_shape_completes
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 5;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
          4, 5, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_retry_current_message_header_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_header_sender_domain_column_exists (path);
  assert_message_header_sender_domain_backfilled (path);
  assert_message_header_date_unix_us_column_exists (path);
  assert_message_header_date_unix_us_backfilled (path);
  assert_message_header_date_retry_rows_preserved (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v6_date_unix_us_retry_completes
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 6;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS,
          3, 4, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT,
          4, 5, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
          5, 6, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_retry_current_message_header_shape (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_message_header_sender_domain_column_exists (path);
  assert_message_header_sender_domain_backfilled (path);
  assert_message_header_date_unix_us_column_exists (path);
  assert_message_header_date_unix_us_backfilled (path);
  assert_message_header_date_retry_rows_preserved (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_schema_migration_duckdb_run_store_v3_adds_derived_view_memberships (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 3;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
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
  g_clear_object (&store);

  drop_derived_view_memberships_table (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  assert_derived_view_memberships_table_exists (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v2_adds_message_header_table_rejects_shape_mismatch
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 2;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
          wyrebox_schema_migration_get_first_supported_schema_version (), 2,
          &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_incompatible_message_headers_table (path);
  assert_message_attribute_tables_exist (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_v1_adds_message_attribute_tables_rejects_shape_mismatch
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, wyrebox_schema_migration_get_first_supported_schema_version (),
          &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  create_incompatible_message_flags_table (path);
  assert_bootstrap_catalog_tables_exist (path);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_first_supported_schema_version ());
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_schema_migration_duckdb_run_store_current_preserves_checkpoint (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = wyrebox_schema_migration_get_current_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_no_error (error);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_legacy_with_checkpoint_persists_current
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, TRUE, &error));
  g_assert_no_error (error);
  assert_bootstrap_catalog_tables_exist (path);
  g_clear_object (&store);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_legacy_without_checkpoint_preserves_state
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_object (&store);
  g_clear_error (&error);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
    test_schema_migration_duckdb_run_store_future_metadata_preserves_state
    (void)
{
  g_autofree char *root = NULL;
  g_autofree char *path = make_duckdb_path (&root);
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };

  migration = wyrebox_schema_migration_new ();
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  base.schema_version_present = TRUE;
  base.schema_version =
      wyrebox_schema_migration_get_current_schema_version () + 1;
  test_schema_migration_set_materialization_checkpoint_fields (&base);
  g_assert_true (wyrebox_schema_metadata_store_save (store, &base, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_object (&store);
  g_clear_error (&error);

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_load (store, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);
  g_assert_false (loaded.checkpoint_precondition_satisfied);

  g_clear_object (&store);
  remove_directory_tree (root);
}

static void
test_checkpoint_precondition_missing_blocks_legacy_migration (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 0);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 0);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_materialization_checkpoint_missing_blocks_checkpointed_migration (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  metadata.checkpoint_precondition_satisfied = TRUE;
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
  g_assert_true (metadata.checkpoint_precondition_satisfied);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 0);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 0);
  g_assert_false (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 0);
}

static void
test_materialization_checkpoint_validation_accepts_clean_journal (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;

  g_assert_nonnull (journal_root);
  append_single_journal_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = checkpoint_offset;
  metadata.materialization_checkpoint_sequence = checkpoint_sequence;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_true (wyrebox_schema_migration_validate_materialization_checkpoint
      (reader, &metadata, &error));
  g_assert_no_error (error);

  remove_directory_tree (journal_root);
}

static void
test_materialization_checkpoint_validation_rejects_checksum_corruption (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;

  g_assert_nonnull (journal_root);
  append_single_journal_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);
  corrupt_journal_checksum (journal_root, checkpoint_offset);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = checkpoint_offset;
  metadata.materialization_checkpoint_sequence = checkpoint_sequence;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_false (wyrebox_schema_migration_validate_materialization_checkpoint
      (reader, &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_directory_tree (journal_root);
}

static void
test_materialization_checkpoint_validation_rejects_sequence_mismatch (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;

  g_assert_nonnull (journal_root);
  append_single_journal_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = checkpoint_offset;
  metadata.materialization_checkpoint_sequence = checkpoint_sequence + 1;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_false (wyrebox_schema_migration_validate_materialization_checkpoint
      (reader, &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);

  remove_directory_tree (journal_root);
}

static void
test_materialization_checkpoint_validation_rejects_unsafe_suffix (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autofree char *segment_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;

  g_assert_nonnull (journal_root);
  append_single_journal_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);
  segment_path = journal_segment_path (journal_root);
  g_assert_cmpint (truncate (segment_path, 17), ==, 0);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = checkpoint_offset;
  metadata.materialization_checkpoint_sequence = checkpoint_sequence;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_false (wyrebox_schema_migration_validate_materialization_checkpoint
      (reader, &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_nonnull (strstr (error->message, "unsafe suffix"));
  g_clear_error (&error);

  remove_directory_tree (journal_root);
}

static void
test_materialization_checkpoint_validation_rejects_missing_journal (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };

  g_assert_nonnull (journal_root);

  metadata.materialization_checkpoint_present = TRUE;
  metadata.materialization_checkpoint_journal_offset = 0;
  metadata.materialization_checkpoint_sequence = 1;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  g_assert_false (wyrebox_schema_migration_validate_materialization_checkpoint
      (reader, &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_directory_tree (journal_root);
}

static void
test_run_store_to_current_with_journal_rejects_corrupt_checkpoint (void)
{
  g_autofree char *journal_root =
      g_dir_make_tmp ("wyrebox-schema-migration-journal-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  guint64 checkpoint_offset = 0;
  guint64 checkpoint_sequence = 0;

  g_assert_nonnull (journal_root);
  append_single_journal_record (journal_root, &checkpoint_offset,
      &checkpoint_sequence);
  corrupt_journal_checksum (journal_root, checkpoint_offset);

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;
  test_schema_migration_set_materialization_checkpoint_fields (&base);

  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_called = FALSE;
  spy->save_call_count = 0;
  spy->migration_operation_call_count = 0;

  g_assert_false
      (wyrebox_schema_migration_run_store_to_current_with_journal
      (migration, (WyreboxSchemaMetadataStore *) spy, reader, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error (&error);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 0);
  g_assert_cmpuint (spy->save_call_count, ==, 0);
  g_assert_false (spy->save_called);

  g_assert_true (wyrebox_schema_metadata_store_load (
          (WyreboxSchemaMetadataStore *) spy, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, base.schema_version);
  g_assert_true (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==,
      base.materialization_checkpoint_journal_offset);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==,
      base.materialization_checkpoint_sequence);

  g_object_unref (spy);
  remove_directory_tree (journal_root);
}

static void
test_destructive_forward_path_without_checkpoint_precondition_fails (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 4;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, 4);
  g_assert_false (metadata.checkpoint_precondition_satisfied);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 0);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 0);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_destructive_forward_path_without_materialization_checkpoint_fails (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 4;
  metadata.checkpoint_precondition_satisfied = TRUE;
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, 4);
  g_assert_true (metadata.checkpoint_precondition_satisfied);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 0);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 0);
  g_assert_false (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 0);
}

static void
    test_schema_migration_run_store_to_current_destructive_without_materialization_checkpoint_fails
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = 5;

  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_called = FALSE;
  spy->save_call_count = 0;
  spy->migration_operation_call_count = 0;

  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_false (spy->save_called);
  g_assert_cmpuint (spy->save_call_count, ==, 0);
  g_assert_cmpuint (spy->migration_operation_call_count, ==, 0);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (
          (WyreboxSchemaMetadataStore *) spy, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, 5);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);

  g_object_unref (spy);
}

static void
    test_schema_migration_run_store_to_current_legacy_without_materialization_checkpoint_fails
    (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  TestSchemaMetadataStoreSpy *spy = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) loaded = { 0 };
  g_auto (WyreboxSchemaMigrationMetadataState) base = { 0 };

  migration = wyrebox_schema_migration_new ();
  spy = test_schema_metadata_store_spy_new ();

  base.schema_version_present = TRUE;
  base.schema_version = 0;

  g_assert_true (wyrebox_schema_metadata_store_save ((WyreboxSchemaMetadataStore
              *) spy, &base, &error));
  g_assert_no_error (error);
  spy->save_called = FALSE;
  spy->save_call_count = 0;
  spy->observed_checkpoint_precondition_satisfied = FALSE;

  g_clear_error (&error);
  g_assert_false (wyrebox_schema_migration_run_store_to_current (migration,
          (WyreboxSchemaMetadataStore *) spy, TRUE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_false (spy->save_called);
  g_assert_cmpuint (spy->save_call_count, ==, 0);

  g_clear_error (&error);
  g_assert_true (wyrebox_schema_metadata_store_load (
          (WyreboxSchemaMetadataStore *) spy, &loaded, &error));
  g_assert_no_error (error);
  g_assert_true (loaded.schema_version_present);
  g_assert_cmpuint (loaded.schema_version, ==, 0);
  g_assert_false (loaded.checkpoint_precondition_satisfied);
  g_assert_false (loaded.materialization_checkpoint_present);
  g_assert_cmpuint (loaded.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (loaded.materialization_checkpoint_sequence, ==, 0);

  g_object_unref (spy);
}

static void
test_missing_schema_metadata_runs_legacy_bootstrap_to_first_version (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 first_version = 0;
  guint64 current_version = 0;

  migration = wyrebox_schema_migration_new ();
  first_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();
  current_version = wyrebox_schema_migration_get_current_schema_version ();

  g_assert_false (metadata.schema_version_present);
  g_assert_cmpuint (first_version, ==, 1);
  g_assert_cmpuint (current_version, ==, 9);
  g_assert_true (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, first_version, &error));
  g_assert_no_error (error);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, first_version);
  g_assert_false (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 0);
}

static void
test_current_schema_version_is_noop (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);

  g_assert_true (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_no_error (error);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
}

static void
test_unknown_future_schema_version_is_rejected (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 future_version = 0;
  guint64 original_version = 0;

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version =
      wyrebox_schema_migration_get_current_schema_version ();
  original_version = metadata.schema_version;
  future_version = metadata.schema_version + 1;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, future_version, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, original_version);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_future_schema_metadata_is_rejected (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 original_version = 0;

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  original_version = wyrebox_schema_migration_get_current_schema_version () + 1;
  metadata.schema_version = original_version;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, original_version);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_downgrade_target_is_rejected (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 current_version = 0;
  guint64 requested_version = 0;

  migration = wyrebox_schema_migration_new ();
  current_version = wyrebox_schema_migration_get_current_schema_version ();
  requested_version = current_version - 1;
  metadata.schema_version_present = TRUE;
  metadata.schema_version = current_version;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, requested_version, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (metadata.schema_version, ==, current_version);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_explicit_forward_path_succeeds_with_checkpoint_precondition (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };
  guint64 current_version = 0;

  migration = wyrebox_schema_migration_new ();
  current_version = wyrebox_schema_migration_get_current_schema_version ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  metadata.checkpoint_precondition_satisfied = TRUE;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  fixture_data.expected_fail_at_call = 0;
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_true (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 9);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 9);
  g_assert_cmpuint (metadata.schema_version, ==, current_version);
  g_assert_false (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==, 0);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 0);
}

static void
test_schema_jump_without_explicit_path_is_rejected (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  metadata.checkpoint_precondition_satisfied = TRUE;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, wyrebox_schema_migration_get_current_schema_version () + 1,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
}

static void
test_operation_hook_failure_does_not_promote_version (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  fixture_data.expected_fail_at_call = 1;
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  metadata.checkpoint_precondition_satisfied = TRUE;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_force_op_failure, NULL, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_validation_hook_failure_does_not_promote_version (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  fixture_data.expected_fail_at_call = 1;
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  metadata.checkpoint_precondition_satisfied = TRUE;
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  wyrebox_schema_migration_set_test_step_hooks (migration,
      NULL,
      test_schema_migration_force_validation_failure, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
}

static void
test_schema_version_constants_are_testable (void)
{
  g_assert_cmpuint (wyrebox_schema_migration_get_first_supported_schema_version
      (), ==, 1);
  g_assert_cmpuint (wyrebox_schema_migration_get_current_schema_version (),
      ==, 9);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/migration/schema/missing-metadata-runs-legacy-bootstrap",
      test_missing_schema_metadata_runs_legacy_bootstrap_to_first_version);
  g_test_add_func ("/migration/schema/current-version-is-noop",
      test_current_schema_version_is_noop);
  g_test_add_func ("/migration/schema/future-version-rejected",
      test_unknown_future_schema_version_is_rejected);
  g_test_add_func ("/migration/schema/future-metadata-rejected",
      test_future_schema_metadata_is_rejected);
  g_test_add_func ("/migration/schema/downgrade-target-rejected",
      test_downgrade_target_is_rejected);
  g_test_add_func ("/migration/schema/run-store-to-current-missing-metadata",
      test_schema_migration_run_store_to_current_missing_metadata_roundtrips_to_current);
  g_test_add_func
      ("/migration/schema/run-store-to-current-current-roundtrip",
      test_schema_migration_run_store_to_current_preserves_current_metadata);
  g_test_add_func
      ("/migration/schema/run-store-to-current-transient-precondition-not-saved",
      test_schema_migration_run_store_to_current_transient_precondition_not_saved);
  g_test_add_func
      ("/migration/schema/run-store-missing-metadata-applies-full-path",
      test_schema_migration_run_store_missing_metadata_applies_full_path);
  g_test_add_func
      ("/migration/schema/run-store-current-metadata-skips-operation",
      test_schema_migration_run_store_current_metadata_skips_operation);
  g_test_add_func
      ("/migration/schema/run-store-operation-failure-preserves-state",
      test_schema_migration_run_store_operation_failure_preserves_state);
  g_test_add_func
      ("/migration/schema/run-store-to-current-future-metadata-rejected",
      test_schema_migration_run_store_to_current_future_metadata_rejected);
  g_test_add_func
      ("/migration/schema/run-store-to-current-legacy-without-checkpoint",
      test_schema_migration_run_store_to_current_legacy_without_checkpoint_fails);
  g_test_add_func
      ("/migration/schema/run-store-to-current-legacy-with-checkpoint",
      test_schema_migration_run_store_to_current_legacy_with_checkpoint_succeeds);
  g_test_add_func
      ("/migration/schema/run-store-to-current-save-failure",
      test_schema_migration_run_store_to_current_save_failure_preserves_state);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-missing-metadata",
      test_schema_migration_duckdb_run_store_missing_metadata_persists_current);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-current-preserves-checkpoint",
      test_schema_migration_duckdb_run_store_current_preserves_checkpoint);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-legacy-with-checkpoint",
      test_schema_migration_duckdb_run_store_legacy_with_checkpoint_persists_current);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v1-adds-message-attribute-tables",
      test_schema_migration_duckdb_run_store_v1_adds_message_attribute_tables);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v2-adds-message-header-table",
      test_schema_migration_duckdb_run_store_v2_adds_message_header_table);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v3-adds-derived-view-memberships",
      test_schema_migration_duckdb_run_store_v3_adds_derived_view_memberships);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v4-scopes-derived-view-identity",
      test_schema_migration_duckdb_run_store_v4_scopes_derived_view_identity);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v5-adds-sender-domain",
      test_schema_migration_duckdb_run_store_v5_adds_sender_domain);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v5-without-checkpoint-preserves-state",
      test_schema_migration_duckdb_run_store_v5_without_checkpoint_preserves_state);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v5-sender-domain-retry-completes",
      test_schema_migration_duckdb_run_store_v5_sender_domain_retry_completes);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v5-current-header-shape-completes",
      test_schema_migration_duckdb_run_store_v5_current_header_shape_completes);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v6-date-unix-us-retry-completes",
      test_schema_migration_duckdb_run_store_v6_date_unix_us_retry_completes);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v2-adds-message-header-table-rejects-shape-mismatch",
      test_schema_migration_duckdb_run_store_v2_adds_message_header_table_rejects_shape_mismatch);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-v1-adds-message-attribute-tables-rejects-shape-mismatch",
      test_schema_migration_duckdb_run_store_v1_adds_message_attribute_tables_rejects_shape_mismatch);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-legacy-without-checkpoint",
      test_schema_migration_duckdb_run_store_legacy_without_checkpoint_preserves_state);
  g_test_add_func
      ("/migration/schema/duckdb-run-store-future-metadata",
      test_schema_migration_duckdb_run_store_future_metadata_preserves_state);
  g_test_add_func ("/migration/schema/explicit-forward-path",
      test_explicit_forward_path_succeeds_with_checkpoint_precondition);
  g_test_add_func
      ("/migration/schema/legacy-forward-path-without-checkpoint-fails",
      test_checkpoint_precondition_missing_blocks_legacy_migration);
  g_test_add_func
      ("/migration/schema/legacy-forward-path-without-materialization-checkpoint-fails",
      test_materialization_checkpoint_missing_blocks_checkpointed_migration);
  g_test_add_func
      ("/migration/schema/materialization-checkpoint-validation/accepts-clean-journal",
      test_materialization_checkpoint_validation_accepts_clean_journal);
  g_test_add_func
      ("/migration/schema/materialization-checkpoint-validation/rejects-checksum-corruption",
      test_materialization_checkpoint_validation_rejects_checksum_corruption);
  g_test_add_func
      ("/migration/schema/materialization-checkpoint-validation/rejects-sequence-mismatch",
      test_materialization_checkpoint_validation_rejects_sequence_mismatch);
  g_test_add_func
      ("/migration/schema/materialization-checkpoint-validation/rejects-unsafe-suffix",
      test_materialization_checkpoint_validation_rejects_unsafe_suffix);
  g_test_add_func
      ("/migration/schema/materialization-checkpoint-validation/rejects-missing-journal",
      test_materialization_checkpoint_validation_rejects_missing_journal);
  g_test_add_func
      ("/migration/schema/run-store-to-current-with-journal/rejects-corrupt-checkpoint",
      test_run_store_to_current_with_journal_rejects_corrupt_checkpoint);
  g_test_add_func
      ("/migration/schema/destructive-forward-path-without-checkpoint-precondition-fails",
      test_destructive_forward_path_without_checkpoint_precondition_fails);
  g_test_add_func
      ("/migration/schema/destructive-forward-path-without-materialization-checkpoint-fails",
      test_destructive_forward_path_without_materialization_checkpoint_fails);
  g_test_add_func
      ("/migration/schema/run-store-to-current-destructive-without-materialization-checkpoint",
      test_schema_migration_run_store_to_current_destructive_without_materialization_checkpoint_fails);
  g_test_add_func
      ("/migration/schema/run-store-to-current-legacy-without-materialization-checkpoint",
      test_schema_migration_run_store_to_current_legacy_without_materialization_checkpoint_fails);
  g_test_add_func ("/migration/schema/jump-without-explicit-path",
      test_schema_jump_without_explicit_path_is_rejected);
  g_test_add_func ("/migration/schema/operation-hook-failure-no-promotion",
      test_operation_hook_failure_does_not_promote_version);
  g_test_add_func ("/migration/schema/validation-hook-failure-no-promotion",
      test_validation_hook_failure_does_not_promote_version);
  g_test_add_func ("/migration/schema/version-constants",
      test_schema_version_constants_are_testable);

  return g_test_run ();
}
