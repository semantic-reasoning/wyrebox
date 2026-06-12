/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <glib.h>

typedef struct
{
  WyreboxSchemaMetadataStore parent_instance;

  gboolean has_state;
  WyreboxSchemaMigrationMetadataState persisted_state;
  gboolean fail_next_save;
} WyreboxSchemaMetadataStoreMemory;

typedef struct
{
  WyreboxSchemaMetadataStoreClass parent_class;
} WyreboxSchemaMetadataStoreMemoryClass;

typedef struct
{
  WyreboxSchemaMetadataStore parent_instance;

  gchar *path;
  duckdb_database database;
  duckdb_connection connection;
} WyreboxSchemaMetadataStoreDuckdb;

typedef struct
{
  WyreboxSchemaMetadataStoreClass parent_class;
} WyreboxSchemaMetadataStoreDuckdbClass;

G_DEFINE_TYPE (WyreboxSchemaMetadataStore, wyrebox_schema_metadata_store,
    G_TYPE_OBJECT);

static gboolean
wyrebox_schema_metadata_store_default_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  (void) self;
  (void) out_state;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store load is not implemented");
  return FALSE;
}

static gboolean
wyrebox_schema_metadata_store_default_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  (void) self;
  (void) state;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store save is not implemented");
  return FALSE;
}

static gboolean
    wyrebox_schema_metadata_store_default_apply_migration_operation
    (WyreboxSchemaMetadataStore * self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version, guint64 target_version, GError ** error)
{
  (void) self;
  (void) operation;
  (void) source_version;
  (void) target_version;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store migration operation is not implemented");
  return FALSE;
}

static void
wyrebox_schema_metadata_store_init (WyreboxSchemaMetadataStore *self)
{
}

static void
wyrebox_schema_metadata_store_class_init (WyreboxSchemaMetadataStoreClass
    *klass)
{
  klass->load = wyrebox_schema_metadata_store_default_load;
  klass->save = wyrebox_schema_metadata_store_default_save;
  klass->apply_migration_operation =
      wyrebox_schema_metadata_store_default_apply_migration_operation;
}

G_DEFINE_TYPE (WyreboxSchemaMetadataStoreMemory,
    wyrebox_schema_metadata_store_memory, WYREBOX_TYPE_SCHEMA_METADATA_STORE);

G_DEFINE_TYPE (WyreboxSchemaMetadataStoreDuckdb,
    wyrebox_schema_metadata_store_duckdb, WYREBOX_TYPE_SCHEMA_METADATA_STORE);

static void
wyrebox_duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, wyrebox_duckdb_result_clear)
/* *INDENT-ON* */

static void
wyrebox_duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
  duckdb_destroy_prepare (statement);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    wyrebox_duckdb_prepared_statement_clear)
/* *INDENT-ON* */

static gboolean
wyrebox_schema_metadata_store_memory_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  wyrebox_schema_migration_metadata_state_clear (out_state);

  if (!memory_store->has_state)
    return TRUE;

  *out_state = memory_store->persisted_state;
  out_state->checkpoint_precondition_satisfied = FALSE;
  return TRUE;
}

static gboolean
wyrebox_schema_metadata_store_memory_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  if (memory_store->fail_next_save) {
    memory_store->fail_next_save = FALSE;
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "forced metadata store save failure");
    return FALSE;
  }

  memory_store->persisted_state = *state;
  memory_store->persisted_state.checkpoint_precondition_satisfied = FALSE;
  memory_store->has_state = TRUE;
  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_memory_apply_migration_operation
    (WyreboxSchemaMetadataStore * self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version, guint64 target_version, GError ** error)
{
  (void) self;
  (void) source_version;
  (void) target_version;

  if (operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "unsupported schema metadata store migration operation %u",
      (guint) operation);
  return FALSE;
}

static void
    wyrebox_schema_metadata_store_memory_class_init
    (WyreboxSchemaMetadataStoreMemoryClass * klass)
{
  WyreboxSchemaMetadataStoreClass *store_class =
      WYREBOX_SCHEMA_METADATA_STORE_CLASS (klass);

  store_class->load = wyrebox_schema_metadata_store_memory_load;
  store_class->save = wyrebox_schema_metadata_store_memory_save;
  store_class->apply_migration_operation =
      wyrebox_schema_metadata_store_memory_apply_migration_operation;
}

static void
wyrebox_schema_metadata_store_memory_init (WyreboxSchemaMetadataStoreMemory
    *self)
{
  self->fail_next_save = FALSE;
  self->has_state = FALSE;
  wyrebox_schema_migration_metadata_state_clear (&self->persisted_state);
}

static gboolean
duckdb_store_query (WyreboxSchemaMetadataStoreDuckdb *self,
    const gchar *sql, GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_query (self->connection, sql, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query failed: %s", detail != NULL ? detail : sql);
    return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_prepare (WyreboxSchemaMetadataStoreDuckdb *self,
    const gchar *sql, duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (self->connection, sql, out_statement) != DuckDBSuccess) {
    const char *detail = *out_statement != NULL ?
        duckdb_prepare_error (*out_statement) : NULL;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB prepare failed: %s", detail != NULL ? detail : sql);
    return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_execute_prepared (duckdb_prepared_statement statement,
    GError **error)
{
  g_auto (duckdb_result) result = { 0 };

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB prepared statement failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_create_schema (WyreboxSchemaMetadataStoreDuckdb *self,
    GError **error)
{
  return duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS schema_metadata ("
      "schema_key VARCHAR PRIMARY KEY CHECK (schema_key = 'schema'),"
      "schema_version UBIGINT NOT NULL" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS materialization_checkpoint ("
      "checkpoint_key VARCHAR PRIMARY KEY "
      "CHECK (checkpoint_key = 'materialization'),"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");", error);
}

static gboolean
duckdb_store_load_schema_metadata (WyreboxSchemaMetadataStoreDuckdb *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  g_auto (duckdb_result) result = { 0 };
  idx_t rows = 0;

  if (duckdb_query (self->connection,
          "SELECT schema_version FROM schema_metadata "
          "WHERE schema_key = 'schema';", &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB schema metadata load failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  rows = duckdb_row_count (&result);
  if (rows > 1 || duckdb_column_count (&result) != 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB schema metadata result has invalid shape");
    return FALSE;
  }

  if (rows == 0)
    return TRUE;

  if (duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB schema metadata row has NULL schema version");
    return FALSE;
  }

  out_state->schema_version_present = TRUE;
  out_state->schema_version = (guint64) duckdb_value_uint64 (&result, 0, 0);
  return TRUE;
}

static gboolean
    duckdb_store_load_materialization_checkpoint
    (WyreboxSchemaMetadataStoreDuckdb * self,
    WyreboxSchemaMigrationMetadataState * out_state, GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  idx_t rows = 0;

  if (duckdb_query (self->connection,
          "SELECT journal_offset, journal_sequence "
          "FROM materialization_checkpoint "
          "WHERE checkpoint_key = 'materialization';",
          &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB materialization checkpoint load failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  rows = duckdb_row_count (&result);
  if (rows > 1 || duckdb_column_count (&result) != 2) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB materialization checkpoint result has invalid shape");
    return FALSE;
  }

  if (rows == 0)
    return TRUE;

  if (duckdb_value_is_null (&result, 0, 0) ||
      duckdb_value_is_null (&result, 1, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB materialization checkpoint row has NULL payload");
    return FALSE;
  }

  out_state->materialization_checkpoint_present = TRUE;
  out_state->materialization_checkpoint_journal_offset =
      (guint64) duckdb_value_uint64 (&result, 0, 0);
  out_state->materialization_checkpoint_sequence =
      (guint64) duckdb_value_uint64 (&result, 1, 0);
  return TRUE;
}

static gboolean
wyrebox_schema_metadata_store_duckdb_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  wyrebox_schema_migration_metadata_state_clear (out_state);

  return duckdb_store_load_schema_metadata (duckdb_store, out_state, error)
      && duckdb_store_load_materialization_checkpoint (duckdb_store,
      out_state, error);
}

static void
duckdb_store_rollback_quietly (WyreboxSchemaMetadataStoreDuckdb *self)
{
  g_auto (duckdb_result) result = { 0 };

  (void) duckdb_query (self->connection, "ROLLBACK;", &result);
}

static gboolean
duckdb_store_save_schema_metadata (WyreboxSchemaMetadataStoreDuckdb *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  if (!state->schema_version_present)
    return TRUE;

  if (!duckdb_store_prepare (self,
          "INSERT INTO schema_metadata (schema_key, schema_version) "
          "VALUES ('schema', ?);", &statement, error))
    return FALSE;

  if (duckdb_bind_uint64 (statement, 1, (uint64_t) state->schema_version) !=
      DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "DuckDB schema metadata bind failed");
    return FALSE;
  }

  return duckdb_store_execute_prepared (statement, error);
}

static gboolean
    duckdb_store_save_materialization_checkpoint
    (WyreboxSchemaMetadataStoreDuckdb * self,
    const WyreboxSchemaMigrationMetadataState * state, GError ** error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  if (!state->materialization_checkpoint_present)
    return TRUE;

  if (!duckdb_store_prepare (self,
          "INSERT INTO materialization_checkpoint ("
          "checkpoint_key, journal_offset, journal_sequence"
          ") VALUES ('materialization', ?, ?);", &statement, error))
    return FALSE;

  if (duckdb_bind_uint64 (statement,
          1,
          (uint64_t) state->materialization_checkpoint_journal_offset) !=
      DuckDBSuccess ||
      duckdb_bind_uint64 (statement,
          2,
          (uint64_t) state->materialization_checkpoint_sequence) !=
      DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB materialization checkpoint bind failed");
    return FALSE;
  }

  return duckdb_store_execute_prepared (statement, error);
}

static gboolean
wyrebox_schema_metadata_store_duckdb_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;

  if (!duckdb_store_query (duckdb_store, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!duckdb_store_create_schema (duckdb_store, error) ||
      !duckdb_store_query (duckdb_store,
          "DELETE FROM materialization_checkpoint "
          "WHERE checkpoint_key = 'materialization';",
          error) ||
      !duckdb_store_query (duckdb_store,
          "DELETE FROM schema_metadata WHERE schema_key = 'schema';",
          error) ||
      !duckdb_store_save_schema_metadata (duckdb_store, state, error) ||
      !duckdb_store_save_materialization_checkpoint (duckdb_store,
          state, error) ||
      !duckdb_store_query (duckdb_store, "COMMIT;", error)) {
    duckdb_store_rollback_quietly (duckdb_store);
    return FALSE;
  }

  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_duckdb_apply_migration_operation
    (WyreboxSchemaMetadataStore * self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version, guint64 target_version, GError ** error)
{
  (void) self;
  (void) source_version;
  (void) target_version;

  if (operation ==
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "unsupported DuckDB schema metadata store migration operation %u",
      (guint) operation);
  return FALSE;
}

static void
wyrebox_schema_metadata_store_duckdb_finalize (GObject *object)
{
  WyreboxSchemaMetadataStoreDuckdb *self =
      (WyreboxSchemaMetadataStoreDuckdb *) object;

  if (self->connection != NULL)
    duckdb_disconnect (&self->connection);
  if (self->database != NULL)
    duckdb_close (&self->database);
  g_clear_pointer (&self->path, g_free);

  G_OBJECT_CLASS (wyrebox_schema_metadata_store_duckdb_parent_class)->finalize
      (object);
}

static void
    wyrebox_schema_metadata_store_duckdb_class_init
    (WyreboxSchemaMetadataStoreDuckdbClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  WyreboxSchemaMetadataStoreClass *store_class =
      WYREBOX_SCHEMA_METADATA_STORE_CLASS (klass);

  object_class->finalize = wyrebox_schema_metadata_store_duckdb_finalize;
  store_class->load = wyrebox_schema_metadata_store_duckdb_load;
  store_class->save = wyrebox_schema_metadata_store_duckdb_save;
  store_class->apply_migration_operation =
      wyrebox_schema_metadata_store_duckdb_apply_migration_operation;
}

static void
wyrebox_schema_metadata_store_duckdb_init (WyreboxSchemaMetadataStoreDuckdb
    *self)
{
  self->path = NULL;
}

gboolean
wyrebox_schema_metadata_store_load (WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMigrationMetadataState *out_state, GError **error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (out_state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->load == NULL)
    return FALSE;

  return klass->load (self, out_state, error);
}

gboolean
wyrebox_schema_metadata_store_save (WyreboxSchemaMetadataStore *self,
    const WyreboxSchemaMigrationMetadataState *state, GError **error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->save == NULL)
    return FALSE;

  return klass->save (self, state, error);
}

/* *INDENT-OFF* */
gboolean
wyrebox_schema_metadata_store_apply_migration_operation (
    WyreboxSchemaMetadataStore *self,
    WyreboxSchemaMetadataStoreMigrationOperation operation,
    guint64 source_version,
    guint64 target_version,
    GError **error)
/* *INDENT-ON* */

{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->apply_migration_operation == NULL)
    return FALSE;

  return klass->apply_migration_operation (self,
      operation, source_version, target_version, error);
}

WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_memory (void)
{
  return g_object_new (wyrebox_schema_metadata_store_memory_get_type (), NULL);
}

WyreboxSchemaMetadataStore *
wyrebox_schema_metadata_store_new_duckdb (const gchar *path, GError **error)
{
  const char *effective_path = path;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  WyreboxSchemaMetadataStoreDuckdb *self = NULL;

  g_return_val_if_fail (path != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (g_strcmp0 (path, ":memory:") == 0)
    effective_path = NULL;

  store = g_object_new (wyrebox_schema_metadata_store_duckdb_get_type (), NULL);
  self = (WyreboxSchemaMetadataStoreDuckdb *) store;
  self->path = g_strdup (path);

  if (duckdb_open (effective_path, &self->database) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB schema metadata store open failed");
    return NULL;
  }

  if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB schema metadata store connect failed");
    return NULL;
  }

  if (!duckdb_store_create_schema (self, error))
    return NULL;

  return g_steal_pointer (&store);
}

void wyrebox_schema_metadata_store_memory_set_next_save_failure
    (WyreboxSchemaMetadataStore * self, gboolean fail_next_save)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;

  g_return_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self));
  g_return_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()));

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  memory_store->fail_next_save = fail_next_save;
}
