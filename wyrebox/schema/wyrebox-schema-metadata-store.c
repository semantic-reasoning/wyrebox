/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-rfc5322-date.h"

#include <duckdb.h>
#include <glib.h>
#include <string.h>

typedef struct
{
  WyreboxSchemaMetadataStore parent_instance;

  gboolean has_state;
  WyreboxSchemaMigrationMetadataState persisted_state;
  GHashTable *materialization_manifests;
  GHashTable *materialization_artifact_checksums;
  gboolean fail_next_save;
} WyreboxSchemaMetadataStoreMemory;

typedef struct
{
  WyreboxSchemaMetadataStoreClass parent_class;
} WyreboxSchemaMetadataStoreMemoryClass;

void
wyrebox_materialization_manifest_clear (WyreboxMaterializationManifest
    *manifest)
{
  if (manifest == NULL)
    return;

  g_clear_pointer (&manifest->run_id, g_free);
  g_clear_pointer (&manifest->object_store_identity, g_free);
  g_clear_pointer (&manifest->rule_package_version, g_free);
  g_clear_pointer (&manifest->view_package_version, g_free);
  g_clear_pointer (&manifest->engine_version, g_free);
  g_clear_pointer (&manifest->completion_status, g_free);
  g_clear_pointer (&manifest->error_state, g_free);
  memset (manifest, 0, sizeof *manifest);
}

gboolean
wyrebox_materialization_manifest_equal (const WyreboxMaterializationManifest
    *left, const WyreboxMaterializationManifest *right)
{
  g_return_val_if_fail (left != NULL, FALSE);
  g_return_val_if_fail (right != NULL, FALSE);

  return g_strcmp0 (left->run_id, right->run_id) == 0 &&
      left->start_journal_offset == right->start_journal_offset &&
      left->start_journal_sequence == right->start_journal_sequence &&
      left->end_journal_offset == right->end_journal_offset &&
      left->end_journal_sequence == right->end_journal_sequence &&
      left->materialized_schema_version == right->materialized_schema_version &&
      g_strcmp0 (left->object_store_identity,
      right->object_store_identity) == 0 &&
      g_strcmp0 (left->rule_package_version,
      right->rule_package_version) == 0 &&
      g_strcmp0 (left->view_package_version,
      right->view_package_version) == 0 &&
      g_strcmp0 (left->engine_version, right->engine_version) == 0 &&
      left->created_at_unix_us == right->created_at_unix_us &&
      g_strcmp0 (left->completion_status,
      right->completion_status) == 0 &&
      g_strcmp0 (left->error_state, right->error_state) == 0;
}

void wyrebox_materialization_artifact_checksum_clear
    (WyreboxMaterializationArtifactChecksum * checksum)
{
  if (checksum == NULL)
    return;

  g_clear_pointer (&checksum->run_id, g_free);
  g_clear_pointer (&checksum->artifact_kind, g_free);
  g_clear_pointer (&checksum->artifact_name, g_free);
  g_clear_pointer (&checksum->checksum_algorithm, g_free);
  g_clear_pointer (&checksum->logical_checksum, g_free);
  memset (checksum, 0, sizeof *checksum);
}

static WyreboxMaterializationArtifactChecksum *
wyrebox_materialization_artifact_checksum_dup (const
    WyreboxMaterializationArtifactChecksum *checksum)
{
  WyreboxMaterializationArtifactChecksum *copy = NULL;

  g_return_val_if_fail (checksum != NULL, NULL);

  copy = g_new0 (WyreboxMaterializationArtifactChecksum, 1);
  copy->run_id = g_strdup (checksum->run_id);
  copy->artifact_kind = g_strdup (checksum->artifact_kind);
  copy->artifact_name = g_strdup (checksum->artifact_name);
  copy->row_count = checksum->row_count;
  copy->checksum_algorithm = g_strdup (checksum->checksum_algorithm);
  copy->logical_checksum = g_strdup (checksum->logical_checksum);

  return copy;
}

static void
    wyrebox_materialization_artifact_checksum_free
    (WyreboxMaterializationArtifactChecksum * checksum)
{
  if (checksum == NULL)
    return;

  wyrebox_materialization_artifact_checksum_clear (checksum);
  g_free (checksum);
}

static gchar *
wyrebox_materialization_artifact_checksum_key (const gchar *run_id,
    const gchar *artifact_kind, const gchar *artifact_name)
{
  return g_strdup_printf ("%s\037%s\037%s", run_id, artifact_kind,
      artifact_name);
}

static gboolean
wyrebox_materialization_artifact_checksum_validate (const
    WyreboxMaterializationArtifactChecksum *checksum, GError **error)
{
  g_return_val_if_fail (checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (checksum->run_id == NULL || checksum->run_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum requires a non-empty run ID");
    return FALSE;
  }

  if (checksum->artifact_kind == NULL || checksum->artifact_kind[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum requires a non-empty artifact kind");
    return FALSE;
  }

  if (g_strcmp0 (checksum->artifact_kind, "table") != 0 &&
      g_strcmp0 (checksum->artifact_kind, "view") != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum artifact kind must be table or view");
    return FALSE;
  }

  if (checksum->artifact_name == NULL || checksum->artifact_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum requires a non-empty artifact name");
    return FALSE;
  }

  if (checksum->checksum_algorithm == NULL ||
      checksum->checksum_algorithm[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum requires a non-empty checksum algorithm");
    return FALSE;
  }

  if (checksum->logical_checksum == NULL ||
      checksum->logical_checksum[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization artifact checksum requires a non-empty logical checksum");
    return FALSE;
  }

  return TRUE;
}

static WyreboxMaterializationManifest *
wyrebox_materialization_manifest_dup (const WyreboxMaterializationManifest
    *manifest)
{
  WyreboxMaterializationManifest *copy = NULL;

  g_return_val_if_fail (manifest != NULL, NULL);

  copy = g_new0 (WyreboxMaterializationManifest, 1);
  copy->run_id = g_strdup (manifest->run_id);
  copy->start_journal_offset = manifest->start_journal_offset;
  copy->start_journal_sequence = manifest->start_journal_sequence;
  copy->end_journal_offset = manifest->end_journal_offset;
  copy->end_journal_sequence = manifest->end_journal_sequence;
  copy->materialized_schema_version = manifest->materialized_schema_version;
  copy->object_store_identity = g_strdup (manifest->object_store_identity);
  copy->rule_package_version = g_strdup (manifest->rule_package_version);
  copy->view_package_version = g_strdup (manifest->view_package_version);
  copy->engine_version = g_strdup (manifest->engine_version);
  copy->created_at_unix_us = manifest->created_at_unix_us;
  copy->completion_status = g_strdup (manifest->completion_status);
  copy->error_state = g_strdup (manifest->error_state);

  return copy;
}

static void
wyrebox_materialization_manifest_free (WyreboxMaterializationManifest *manifest)
{
  if (manifest == NULL)
    return;

  wyrebox_materialization_manifest_clear (manifest);
  g_free (manifest);
}

static gboolean
wyrebox_materialization_manifest_validate (const WyreboxMaterializationManifest
    *manifest, GError **error)
{
  const gchar *status = NULL;

  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (manifest->run_id == NULL || manifest->run_id[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty run ID");
    return FALSE;
  }

  if (manifest->object_store_identity == NULL ||
      manifest->object_store_identity[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty object store identity");
    return FALSE;
  }

  if (manifest->rule_package_version == NULL ||
      manifest->rule_package_version[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty rule package version");
    return FALSE;
  }

  if (manifest->view_package_version == NULL ||
      manifest->view_package_version[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty view package version");
    return FALSE;
  }

  if (manifest->engine_version == NULL || manifest->engine_version[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty engine version");
    return FALSE;
  }

  if (manifest->completion_status == NULL ||
      manifest->completion_status[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest requires a non-empty completion status");
    return FALSE;
  }

  status = manifest->completion_status;
  if (g_strcmp0 (status, "completed") != 0 && g_strcmp0 (status, "failed") != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest completion status must be completed or failed");
    return FALSE;
  }

  if (g_strcmp0 (status, "completed") == 0 && manifest->error_state != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "completed materialization manifests must not carry an error state");
    return FALSE;
  }

  if (g_strcmp0 (status, "failed") == 0 &&
      (manifest->error_state == NULL || manifest->error_state[0] == '\0')) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "failed materialization manifests require an error state");
    return FALSE;
  }

  if (manifest->start_journal_offset > manifest->end_journal_offset ||
      (manifest->start_journal_offset == manifest->end_journal_offset &&
          manifest->start_journal_sequence > manifest->end_journal_sequence)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "materialization manifest journal range is not ordered");
    return FALSE;
  }

  return TRUE;
}

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

static void wyrebox_schema_metadata_store_memory_finalize (GObject * object);
static gboolean
    wyrebox_schema_metadata_store_memory_load_materialization_manifest
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    WyreboxMaterializationManifest * out_manifest, GError ** error);
static gboolean
    wyrebox_schema_metadata_store_memory_save_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationManifest * manifest, GError ** error);
static gboolean
    wyrebox_schema_metadata_store_duckdb_load_materialization_manifest
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    WyreboxMaterializationManifest * out_manifest, GError ** error);
static gboolean
    wyrebox_schema_metadata_store_duckdb_save_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationManifest * manifest, GError ** error);
static void duckdb_store_rollback_quietly (WyreboxSchemaMetadataStoreDuckdb *
    self);

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
    wyrebox_schema_metadata_store_default_load_latest_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  (void) self;
  (void) out_manifest;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "schema metadata store latest manifest load is not implemented");
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
  klass->load_latest_materialization_manifest =
      wyrebox_schema_metadata_store_default_load_latest_materialization_manifest;
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

typedef struct
{
  const gchar *name;
  const gchar *type;
  gboolean not_null;
} WyreboxDuckdbColumnSpec;

typedef char *WyreboxDuckdbOwnedString;

static void
wyrebox_duckdb_owned_string_clear (WyreboxDuckdbOwnedString *value)
{
  if (value != NULL && *value != NULL)
    duckdb_free (*value);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDuckdbOwnedString,
    wyrebox_duckdb_owned_string_clear)
/* *INDENT-ON* */

static gboolean
duckdb_store_validate_table_exists (WyreboxSchemaMetadataStoreDuckdb *self,
    const gchar *table_name, GError **error)
{
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *query = NULL;
  uint64_t table_count = 0;

  query = g_strdup_printf ("SELECT COUNT(*) FROM information_schema.tables "
      "WHERE table_name = '%s';", table_name);
  if (duckdb_query (self->connection, query, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB table existence check failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_column_count (&result) != 1 || duckdb_row_count (&result) != 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB table existence query returned unexpected shape");
    return FALSE;
  }

  table_count = duckdb_value_uint64 (&result, 0, 0);
  if (table_count != 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "DuckDB table %s does not exist", table_name);
    return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_validate_table_columns (WyreboxSchemaMetadataStoreDuckdb *self,
    const gchar *table_name,
    const WyreboxDuckdbColumnSpec *expected_columns,
    gsize expected_column_count, GError **error)
{
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *query = NULL;

  if (!duckdb_store_validate_table_exists (self, table_name, error))
    return FALSE;

  query = g_strdup_printf ("PRAGMA table_info('%s');", table_name);
  if (duckdb_query (self->connection, query, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB table_info query failed for %s: %s",
        table_name, detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_column_count (&result) != 6 ||
      (guint) duckdb_row_count (&result) != expected_column_count) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB schema table %s has unexpected column shape", table_name);
    return FALSE;
  }

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    const WyreboxDuckdbColumnSpec *expected = &expected_columns[row];
    g_auto (WyreboxDuckdbOwnedString) actual_name = NULL;
    g_auto (WyreboxDuckdbOwnedString) actual_type = NULL;
    gboolean actual_not_null = FALSE;

    actual_name = duckdb_value_varchar (&result, 1, row);
    actual_type = duckdb_value_varchar (&result, 2, row);
    actual_not_null = (gboolean) duckdb_value_int64 (&result, 3, row);

    if (g_strcmp0 (expected->name, actual_name) != 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB table %s has incompatible column at position %"
          G_GINT64_FORMAT " name: expected %s, got %s", table_name,
          (gint64) row, expected->name, actual_name);
      return FALSE;
    }

    if (g_strcmp0 (expected->type, actual_type) != 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB table %s has incompatible column %s type: expected %s, got %s",
          table_name, expected->name, expected->type, actual_type);
      return FALSE;
    }

    if (expected->not_null != actual_not_null) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB table %s column %s has incompatible nullability",
          table_name, expected->name);
      return FALSE;
    }

  }

  return TRUE;
}

static gboolean
duckdb_store_validate_primary_key (WyreboxSchemaMetadataStoreDuckdb *self,
    const gchar *table_name,
    const gchar *const *expected_columns,
    gsize expected_column_count, GError **error)
{
  g_auto (duckdb_result) result = { 0 };
  g_autofree gchar *query = NULL;
  gsize key_column_count = 0;

  query =
      g_strdup_printf
      ("SELECT column_name, ordinal_position FROM information_schema.key_column_usage "
      "WHERE table_name = '%s' AND constraint_name LIKE '%%_pkey' "
      "ORDER BY ordinal_position;", table_name);
  if (duckdb_query (self->connection, query, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB key column usage query failed for %s: %s",
        table_name, detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  key_column_count = (gsize) duckdb_row_count (&result);
  if (duckdb_column_count (&result) != 2 ||
      key_column_count != expected_column_count) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB primary key definition for table %s has unexpected shape",
        table_name);
    return FALSE;
  }

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    g_auto (WyreboxDuckdbOwnedString) column_name = NULL;
    gint64 ordinal_position = 0;

    column_name = duckdb_value_varchar (&result, 0, row);
    ordinal_position = duckdb_value_int64 (&result, 1, row);

    if (expected_columns[row] == NULL ||
        g_strcmp0 (expected_columns[row], column_name) != 0) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB table %s has incompatible primary-key definition",
          table_name);
      return FALSE;
    }

    if (ordinal_position != (gint64) (row + 1)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB table %s primary-key position mismatch for column %s",
          table_name, column_name);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean
    duckdb_store_validate_message_attribute_tables
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_flags_columns[] = {
    {"membership_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"mailbox_id", "VARCHAR", TRUE},
    {"flag_name", "VARCHAR", TRUE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const WyreboxDuckdbColumnSpec message_keywords_columns[] = {
    {"membership_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"mailbox_id", "VARCHAR", TRUE},
    {"keyword_name", "VARCHAR", TRUE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const gchar *message_flags_primary_key_columns[] = {
    "membership_id",
    "flag_name",
  };
  static const gchar *message_keywords_primary_key_columns[] = {
    "membership_id",
    "keyword_name",
  };

  return duckdb_store_validate_table_columns (self, "message_flags",
      message_flags_columns, G_N_ELEMENTS (message_flags_columns), error)
      && duckdb_store_validate_primary_key (self, "message_flags",
      message_flags_primary_key_columns,
      G_N_ELEMENTS (message_flags_primary_key_columns), error)
      && duckdb_store_validate_table_columns (self, "message_keywords",
      message_keywords_columns, G_N_ELEMENTS (message_keywords_columns), error)
      && duckdb_store_validate_primary_key (self, "message_keywords",
      message_keywords_primary_key_columns,
      G_N_ELEMENTS (message_keywords_primary_key_columns), error);
}

static gboolean
    duckdb_store_validate_message_fact_table
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_facts_columns[] = {
    {"fact_id", "VARCHAR", TRUE},
    {"account_id", "VARCHAR", TRUE},
    {"message_id", "VARCHAR", TRUE},
    {"object_id", "VARCHAR", TRUE},
    {"predicate", "VARCHAR", TRUE},
    {"args_json", "VARCHAR", TRUE},
    {"source", "VARCHAR", TRUE},
    {"confidence_ppm", "UBIGINT", TRUE},
    {"created_at_unix_us", "UBIGINT", TRUE},
    {"retracted_at_unix_us", "UBIGINT", TRUE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const gchar *message_facts_primary_key_columns[] = {
    "fact_id",
  };

  return duckdb_store_validate_table_columns (self, "message_facts",
      message_facts_columns, G_N_ELEMENTS (message_facts_columns), error)
      && duckdb_store_validate_primary_key (self, "message_facts",
      message_facts_primary_key_columns,
      G_N_ELEMENTS (message_facts_primary_key_columns), error);
}

static gboolean
    duckdb_store_validate_message_header_table
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_headers_columns[] = {
    {"message_id", "VARCHAR", TRUE},
    {"rfc_message_id", "VARCHAR", FALSE},
    {"duplicate_message_id_count", "UBIGINT", TRUE},
    {"subject", "VARCHAR", FALSE},
    {"from_addr", "VARCHAR", FALSE},
    {"sender_domain", "VARCHAR", FALSE},
    {"to_addr", "VARCHAR", FALSE},
    {"cc_addr", "VARCHAR", FALSE},
    {"bcc_addr", "VARCHAR", FALSE},
    {"date_raw", "VARCHAR", FALSE},
    {"date_unix_us", "BIGINT", FALSE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const gchar *message_headers_primary_key_columns[] = {
    "message_id",
  };

  return duckdb_store_validate_table_columns (self, "message_headers",
      message_headers_columns, G_N_ELEMENTS (message_headers_columns), error)
      && duckdb_store_validate_primary_key (self, "message_headers",
      message_headers_primary_key_columns,
      G_N_ELEMENTS (message_headers_primary_key_columns), error);
}

static gboolean
    duckdb_store_validate_message_header_table_v6
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_headers_columns[] = {
    {"message_id", "VARCHAR", TRUE},
    {"rfc_message_id", "VARCHAR", FALSE},
    {"duplicate_message_id_count", "UBIGINT", TRUE},
    {"subject", "VARCHAR", FALSE},
    {"from_addr", "VARCHAR", FALSE},
    {"sender_domain", "VARCHAR", FALSE},
    {"to_addr", "VARCHAR", FALSE},
    {"cc_addr", "VARCHAR", FALSE},
    {"bcc_addr", "VARCHAR", FALSE},
    {"date_raw", "VARCHAR", FALSE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
  };
  static const gchar *message_headers_primary_key_columns[] = {
    "message_id",
  };

  return duckdb_store_validate_table_columns (self, "message_headers",
      message_headers_columns, G_N_ELEMENTS (message_headers_columns), error)
      && duckdb_store_validate_primary_key (self, "message_headers",
      message_headers_primary_key_columns,
      G_N_ELEMENTS (message_headers_primary_key_columns), error);
}

static gboolean
    duckdb_store_validate_message_header_table_v9
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_headers_columns[] = {
    {"message_id", "VARCHAR", TRUE},
    {"rfc_message_id", "VARCHAR", FALSE},
    {"duplicate_message_id_count", "UBIGINT", TRUE},
    {"subject", "VARCHAR", FALSE},
    {"from_addr", "VARCHAR", FALSE},
    {"sender_domain", "VARCHAR", FALSE},
    {"to_addr", "VARCHAR", FALSE},
    {"cc_addr", "VARCHAR", FALSE},
    {"bcc_addr", "VARCHAR", FALSE},
    {"date_raw", "VARCHAR", FALSE},
    {"date_unix_us", "BIGINT", FALSE},
    {"journal_offset", "UBIGINT", TRUE},
    {"journal_sequence", "UBIGINT", TRUE},
    {"message_id_span_start", "UBIGINT", FALSE},
    {"message_id_span_end", "UBIGINT", FALSE},
    {"subject_span_start", "UBIGINT", FALSE},
    {"subject_span_end", "UBIGINT", FALSE},
  };
  static const gchar *message_headers_primary_key_columns[] = {
    "message_id",
  };

  return duckdb_store_validate_table_columns (self, "message_headers",
      message_headers_columns, G_N_ELEMENTS (message_headers_columns), error)
      && duckdb_store_validate_primary_key (self, "message_headers",
      message_headers_primary_key_columns,
      G_N_ELEMENTS (message_headers_primary_key_columns), error);
}

static gboolean
    duckdb_store_validate_message_header_table_v3
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  static const WyreboxDuckdbColumnSpec message_headers_columns[] = {
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
  static const gchar *message_headers_primary_key_columns[] = {
    "message_id",
  };

  return duckdb_store_validate_table_columns (self, "message_headers",
      message_headers_columns, G_N_ELEMENTS (message_headers_columns), error)
      && duckdb_store_validate_primary_key (self, "message_headers",
      message_headers_primary_key_columns,
      G_N_ELEMENTS (message_headers_primary_key_columns), error);
}

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
    wyrebox_schema_metadata_store_memory_load_materialization_manifest
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;
  WyreboxMaterializationManifest *stored_manifest = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  wyrebox_materialization_manifest_clear (out_manifest);

  stored_manifest =
      g_hash_table_lookup (memory_store->materialization_manifests, run_id);
  if (stored_manifest == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND, "materialization manifest %s not found", run_id);
    return FALSE;
  }

  {
    g_autofree WyreboxMaterializationManifest *copy =
        wyrebox_materialization_manifest_dup (stored_manifest);

    *out_manifest = *copy;
    copy->run_id = NULL;
    copy->object_store_identity = NULL;
    copy->rule_package_version = NULL;
    copy->view_package_version = NULL;
    copy->engine_version = NULL;
    copy->completion_status = NULL;
    copy->error_state = NULL;
  }
  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_memory_load_latest_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;
  GHashTableIter iter = { 0 };
  gpointer key = NULL;
  gpointer value = NULL;
  WyreboxMaterializationManifest *latest_manifest = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  wyrebox_materialization_manifest_clear (out_manifest);

  g_hash_table_iter_init (&iter, memory_store->materialization_manifests);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    WyreboxMaterializationManifest *candidate = value;

    if (latest_manifest == NULL ||
        candidate->created_at_unix_us > latest_manifest->created_at_unix_us ||
        (candidate->created_at_unix_us ==
            latest_manifest->created_at_unix_us &&
            g_strcmp0 (candidate->run_id, latest_manifest->run_id) > 0)) {
      latest_manifest = candidate;
    }
  }

  if (latest_manifest == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND, "materialization manifest catalog is empty");
    return FALSE;
  }

  {
    g_autofree WyreboxMaterializationManifest *copy =
        wyrebox_materialization_manifest_dup (latest_manifest);

    *out_manifest = *copy;
    copy->run_id = NULL;
    copy->object_store_identity = NULL;
    copy->rule_package_version = NULL;
    copy->view_package_version = NULL;
    copy->engine_version = NULL;
    copy->completion_status = NULL;
    copy->error_state = NULL;
  }

  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_memory_save_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationManifest * manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;
  WyreboxMaterializationManifest *copy = NULL;
  gchar *run_id = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_materialization_manifest_validate (manifest, error))
    return FALSE;

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  if (memory_store->fail_next_save) {
    memory_store->fail_next_save = FALSE;
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "forced metadata store save failure");
    return FALSE;
  }

  if (g_hash_table_contains (memory_store->materialization_manifests,
          manifest->run_id)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "materialization manifest %s already exists", manifest->run_id);
    return FALSE;
  }

  copy = wyrebox_materialization_manifest_dup (manifest);
  run_id = g_strdup (copy->run_id);
  g_hash_table_insert (memory_store->materialization_manifests, run_id, copy);
  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_memory_load_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    const gchar * artifact_kind, const gchar * artifact_name,
    WyreboxMaterializationArtifactChecksum * out_checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;
  WyreboxMaterializationArtifactChecksum *stored_checksum = NULL;
  g_autofree gchar *key = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (artifact_kind != NULL, FALSE);
  g_return_val_if_fail (artifact_name != NULL, FALSE);
  g_return_val_if_fail (out_checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  wyrebox_materialization_artifact_checksum_clear (out_checksum);
  key = wyrebox_materialization_artifact_checksum_key (run_id, artifact_kind,
      artifact_name);
  stored_checksum =
      g_hash_table_lookup (memory_store->materialization_artifact_checksums,
      key);
  if (stored_checksum == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "materialization artifact checksum %s/%s/%s not found", run_id,
        artifact_kind, artifact_name);
    return FALSE;
  }

  {
    g_autofree WyreboxMaterializationArtifactChecksum *copy =
        wyrebox_materialization_artifact_checksum_dup (stored_checksum);

    *out_checksum = *copy;
    copy->run_id = NULL;
    copy->artifact_kind = NULL;
    copy->artifact_name = NULL;
    copy->checksum_algorithm = NULL;
    copy->logical_checksum = NULL;
  }

  return TRUE;
}

static gboolean
    wyrebox_schema_metadata_store_memory_save_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationArtifactChecksum * checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreMemory *memory_store = NULL;
  WyreboxMaterializationManifest manifest = { 0 };
  WyreboxMaterializationArtifactChecksum *copy = NULL;
  gchar *key = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_memory_get_type ()), FALSE);
  g_return_val_if_fail (checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!wyrebox_materialization_artifact_checksum_validate (checksum, error))
    return FALSE;

  memory_store = (WyreboxSchemaMetadataStoreMemory *) self;
  if (!wyrebox_schema_metadata_store_memory_load_materialization_manifest (self,
          checksum->run_id, &manifest, error))
    return FALSE;

  if (g_strcmp0 (manifest.completion_status, "completed") != 0) {
    wyrebox_materialization_manifest_clear (&manifest);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "materialization artifact checksum %s requires a completed manifest",
        checksum->run_id);
    return FALSE;
  }

  wyrebox_materialization_manifest_clear (&manifest);
  key = wyrebox_materialization_artifact_checksum_key (checksum->run_id,
      checksum->artifact_kind, checksum->artifact_name);
  if (g_hash_table_contains (memory_store->materialization_artifact_checksums,
          key)) {
    g_free (key);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "materialization artifact checksum %s already exists",
        checksum->run_id);
    return FALSE;
  }

  copy = wyrebox_materialization_artifact_checksum_dup (checksum);
  g_hash_table_insert (memory_store->materialization_artifact_checksums, key,
      copy);
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
      WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_OBJECT_REACHABILITY_VIEW)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_NOT_SUPPORTED,
      "unsupported schema metadata store migration operation %u",
      (guint) operation);
  return FALSE;
}

static void
wyrebox_schema_metadata_store_memory_finalize (GObject *object)
{
  WyreboxSchemaMetadataStoreMemory *self =
      (WyreboxSchemaMetadataStoreMemory *) object;

  g_clear_pointer (&self->materialization_manifests, g_hash_table_destroy);
  g_clear_pointer (&self->materialization_artifact_checksums,
      g_hash_table_destroy);

  G_OBJECT_CLASS (wyrebox_schema_metadata_store_memory_parent_class)->finalize
      (object);
}

static void
    wyrebox_schema_metadata_store_memory_class_init
    (WyreboxSchemaMetadataStoreMemoryClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  WyreboxSchemaMetadataStoreClass *store_class =
      WYREBOX_SCHEMA_METADATA_STORE_CLASS (klass);

  object_class->finalize = wyrebox_schema_metadata_store_memory_finalize;
  store_class->load = wyrebox_schema_metadata_store_memory_load;
  store_class->save = wyrebox_schema_metadata_store_memory_save;
  store_class->load_materialization_manifest =
      wyrebox_schema_metadata_store_memory_load_materialization_manifest;
  store_class->load_latest_materialization_manifest =
      wyrebox_schema_metadata_store_memory_load_latest_materialization_manifest;
  store_class->save_materialization_manifest =
      wyrebox_schema_metadata_store_memory_save_materialization_manifest;
  store_class->load_materialization_artifact_checksum =
      wyrebox_schema_metadata_store_memory_load_materialization_artifact_checksum;
  store_class->save_materialization_artifact_checksum =
      wyrebox_schema_metadata_store_memory_save_materialization_artifact_checksum;
  store_class->apply_migration_operation =
      wyrebox_schema_metadata_store_memory_apply_migration_operation;
}

static void
wyrebox_schema_metadata_store_memory_init (WyreboxSchemaMetadataStoreMemory
    *self)
{
  self->fail_next_save = FALSE;
  self->has_state = FALSE;
  self->materialization_manifests = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free,
      (GDestroyNotify) wyrebox_materialization_manifest_free);
  self->materialization_artifact_checksums = g_hash_table_new_full (g_str_hash,
      g_str_equal, g_free,
      (GDestroyNotify) wyrebox_materialization_artifact_checksum_free);
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
duckdb_store_bind_varchar (duckdb_prepared_statement statement, idx_t index,
    const gchar *value, GError **error)
{
  if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB schema metadata string bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
duckdb_store_bind_nullable_varchar (duckdb_prepared_statement statement,
    idx_t index, const gchar *value, GError **error)
{
  if (value == NULL) {
    if (duckdb_bind_null (statement, index) == DuckDBSuccess)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB schema metadata NULL bind failed at index %" G_GUINT64_FORMAT,
        (guint64) index);
    return FALSE;
  }

  return duckdb_store_bind_varchar (statement, index, value, error);
}

static gboolean
duckdb_store_bind_uint64 (duckdb_prepared_statement statement, idx_t index,
    guint64 value, GError **error)
{
  if (duckdb_bind_uint64 (statement, index, (uint64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB schema metadata uint64 bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
duckdb_store_bind_nullable_int64 (duckdb_prepared_statement statement,
    idx_t index, gboolean has_value, gint64 value, GError **error)
{
  if (!has_value) {
    if (duckdb_bind_null (statement, index) == DuckDBSuccess)
      return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB schema metadata NULL bind failed at index %" G_GUINT64_FORMAT,
        (guint64) index);
    return FALSE;
  }

  if (duckdb_bind_int64 (statement, index, (int64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB schema metadata int64 bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static char *
extract_sender_domain_from_from_addr (const char *value)
{
  const char *at = NULL;
  const char *domain_start = NULL;
  const char *domain_end = NULL;

  if (value == NULL)
    return NULL;

  at = strrchr (value, '@');
  if (at == NULL || at[1] == '\0')
    return NULL;

  domain_start = at + 1;
  domain_end = domain_start;
  while (*domain_end != '\0' &&
      *domain_end != '>' &&
      *domain_end != ',' && !g_ascii_isspace (*domain_end)) {
    domain_end++;
  }

  if (domain_end == domain_start)
    return NULL;

  return g_ascii_strdown (domain_start, domain_end - domain_start);
}

static gchar *
duckdb_store_dup_nullable_varchar (duckdb_result *result, idx_t column,
    idx_t row)
{
  g_auto (WyreboxDuckdbOwnedString) value = NULL;

  if (duckdb_value_is_null (result, column, row))
    return NULL;

  value = duckdb_value_varchar (result, column, row);
  return g_strdup (value);
}

static gboolean
duckdb_store_require_nonnull_column (duckdb_result *result, idx_t column,
    idx_t row, const gchar *column_name, GError **error)
{
  if (!duckdb_value_is_null (result, column, row))
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA,
      "DuckDB message_headers row has NULL %s during migration", column_name);
  return FALSE;
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
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS materialization_manifests ("
      "run_id VARCHAR PRIMARY KEY CHECK (run_id <> ''),"
      "start_journal_offset UBIGINT NOT NULL,"
      "start_journal_sequence UBIGINT NOT NULL,"
      "end_journal_offset UBIGINT NOT NULL,"
      "end_journal_sequence UBIGINT NOT NULL,"
      "materialized_schema_version UBIGINT NOT NULL,"
      "object_store_identity VARCHAR NOT NULL CHECK (object_store_identity <> ''),"
      "rule_package_version VARCHAR NOT NULL CHECK (rule_package_version <> ''),"
      "view_package_version VARCHAR NOT NULL CHECK (view_package_version <> ''),"
      "engine_version VARCHAR NOT NULL CHECK (engine_version <> ''),"
      "created_at_unix_us UBIGINT NOT NULL,"
      "completion_status VARCHAR NOT NULL CHECK (completion_status IN ('completed', 'failed')),"
      "error_state VARCHAR,"
      "CHECK (start_journal_offset < end_journal_offset OR "
      "(start_journal_offset = end_journal_offset AND "
      "start_journal_sequence <= end_journal_sequence)),"
      "CHECK (error_state IS NULL OR error_state <> '')" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS materialization_artifact_checksums ("
      "run_id VARCHAR NOT NULL CHECK (run_id <> ''),"
      "artifact_kind VARCHAR NOT NULL CHECK (artifact_kind IN ('table', 'view')),"
      "artifact_name VARCHAR NOT NULL CHECK (artifact_name <> ''),"
      "row_count UBIGINT NOT NULL,"
      "checksum_algorithm VARCHAR NOT NULL CHECK (checksum_algorithm <> ''),"
      "logical_checksum VARCHAR NOT NULL CHECK (logical_checksum <> ''),"
      "PRIMARY KEY (run_id, artifact_kind, artifact_name)" ");", error);
}

static gboolean
    duckdb_store_create_message_attribute_tables
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  return duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS message_flags ("
      "membership_id VARCHAR NOT NULL,"
      "account_id VARCHAR NOT NULL,"
      "mailbox_id VARCHAR NOT NULL,"
      "flag_name VARCHAR NOT NULL,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL,"
      "PRIMARY KEY (membership_id, flag_name)" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS message_keywords ("
      "membership_id VARCHAR NOT NULL,"
      "account_id VARCHAR NOT NULL,"
      "mailbox_id VARCHAR NOT NULL,"
      "keyword_name VARCHAR NOT NULL,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL,"
      "PRIMARY KEY (membership_id, keyword_name)" ");", error)
      && duckdb_store_validate_message_attribute_tables (self, error);
}

static gboolean
    duckdb_store_create_message_header_table_v3
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  return duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS message_headers ("
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
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_validate_message_header_table_v3 (self, error);
}

static gboolean
    duckdb_store_copy_message_headers_with_sender_domain
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (duckdb_prepared_statement) statement = NULL;

  if (duckdb_query (self->connection,
          "SELECT message_id, rfc_message_id, duplicate_message_id_count, "
          "subject, from_addr, to_addr, cc_addr, bcc_addr, date_raw, "
          "journal_offset, journal_sequence FROM message_headers;",
          &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB message_headers migration select failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_column_count (&result) != 11) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB message_headers migration select returned unexpected shape");
    return FALSE;
  }

  if (!duckdb_store_prepare (self,
          "INSERT INTO message_headers_replacement ("
          "message_id, rfc_message_id, duplicate_message_id_count, subject, "
          "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
          "journal_offset, journal_sequence"
          ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);", &statement, error))
    return FALSE;

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    g_autofree gchar *message_id = NULL;
    g_autofree gchar *rfc_message_id = NULL;
    g_autofree gchar *subject = NULL;
    g_autofree gchar *from_addr = NULL;
    g_autofree gchar *sender_domain = NULL;
    g_autofree gchar *to_addr = NULL;
    g_autofree gchar *cc_addr = NULL;
    g_autofree gchar *bcc_addr = NULL;
    g_autofree gchar *date_raw = NULL;
    guint64 duplicate_message_id_count = 0;
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;

    if (!duckdb_store_require_nonnull_column (&result, 0, row,
            "message_id", error) ||
        !duckdb_store_require_nonnull_column (&result, 2, row,
            "duplicate_message_id_count", error) ||
        !duckdb_store_require_nonnull_column (&result, 9, row,
            "journal_offset", error) ||
        !duckdb_store_require_nonnull_column (&result, 10, row,
            "journal_sequence", error))
      return FALSE;

    message_id = duckdb_store_dup_nullable_varchar (&result, 0, row);
    rfc_message_id = duckdb_store_dup_nullable_varchar (&result, 1, row);
    duplicate_message_id_count =
        (guint64) duckdb_value_uint64 (&result, 2, row);
    subject = duckdb_store_dup_nullable_varchar (&result, 3, row);
    from_addr = duckdb_store_dup_nullable_varchar (&result, 4, row);
    sender_domain = extract_sender_domain_from_from_addr (from_addr);
    to_addr = duckdb_store_dup_nullable_varchar (&result, 5, row);
    cc_addr = duckdb_store_dup_nullable_varchar (&result, 6, row);
    bcc_addr = duckdb_store_dup_nullable_varchar (&result, 7, row);
    date_raw = duckdb_store_dup_nullable_varchar (&result, 8, row);
    journal_offset = (guint64) duckdb_value_uint64 (&result, 9, row);
    journal_sequence = (guint64) duckdb_value_uint64 (&result, 10, row);

    if (!duckdb_store_bind_varchar (statement, 1, message_id, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 2, rfc_message_id,
            error) ||
        !duckdb_store_bind_uint64 (statement, 3,
            duplicate_message_id_count, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 4, subject, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 5, from_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 6, sender_domain,
            error) ||
        !duckdb_store_bind_nullable_varchar (statement, 7, to_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 8, cc_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 9, bcc_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 10, date_raw, error) ||
        !duckdb_store_bind_uint64 (statement, 11, journal_offset, error) ||
        !duckdb_store_bind_uint64 (statement, 12, journal_sequence, error) ||
        !duckdb_store_execute_prepared (statement, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_add_message_header_sender_domain (WyreboxSchemaMetadataStoreDuckdb
    *self, GError **error)
{
  g_autoptr (GError) current_error = NULL;
  g_autoptr (GError) v6_error = NULL;
  g_autoptr (GError) v3_error = NULL;

  if (duckdb_store_validate_message_header_table (self, &current_error))
    return TRUE;

  if (duckdb_store_validate_message_header_table_v6 (self, &v6_error))
    return TRUE;

  if (!duckdb_store_validate_message_header_table_v3 (self, &v3_error)) {
    g_propagate_error (error, g_steal_pointer (&v3_error));
    return FALSE;
  }

  return duckdb_store_query (self,
      "CREATE TABLE message_headers_replacement ("
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
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_copy_message_headers_with_sender_domain (self, error)
      && duckdb_store_query (self, "DROP TABLE message_headers;", error)
      && duckdb_store_query (self,
      "ALTER TABLE message_headers_replacement RENAME TO message_headers;",
      error)
      && duckdb_store_validate_message_header_table_v6 (self, error);
}

static gboolean
    duckdb_store_copy_message_headers_with_date_unix_us
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (duckdb_prepared_statement) statement = NULL;

  if (duckdb_query (self->connection,
          "SELECT message_id, rfc_message_id, duplicate_message_id_count, "
          "subject, from_addr, sender_domain, to_addr, cc_addr, bcc_addr, "
          "date_raw, journal_offset, journal_sequence FROM message_headers;",
          &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB message_headers date migration select failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_column_count (&result) != 12) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB message_headers date migration select returned unexpected shape");
    return FALSE;
  }

  if (!duckdb_store_prepare (self,
          "INSERT INTO message_headers_replacement ("
          "message_id, rfc_message_id, duplicate_message_id_count, subject, "
          "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
          "date_unix_us, journal_offset, journal_sequence"
          ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
          &statement, error))
    return FALSE;

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    g_autofree gchar *message_id = NULL;
    g_autofree gchar *rfc_message_id = NULL;
    g_autofree gchar *subject = NULL;
    g_autofree gchar *from_addr = NULL;
    g_autofree gchar *sender_domain = NULL;
    g_autofree gchar *to_addr = NULL;
    g_autofree gchar *cc_addr = NULL;
    g_autofree gchar *bcc_addr = NULL;
    g_autofree gchar *date_raw = NULL;
    gboolean has_date_unix_us = FALSE;
    gint64 date_unix_us = 0;
    guint64 duplicate_message_id_count = 0;
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;

    if (!duckdb_store_require_nonnull_column (&result, 0, row,
            "message_id", error) ||
        !duckdb_store_require_nonnull_column (&result, 2, row,
            "duplicate_message_id_count", error) ||
        !duckdb_store_require_nonnull_column (&result, 10, row,
            "journal_offset", error) ||
        !duckdb_store_require_nonnull_column (&result, 11, row,
            "journal_sequence", error))
      return FALSE;

    message_id = duckdb_store_dup_nullable_varchar (&result, 0, row);
    rfc_message_id = duckdb_store_dup_nullable_varchar (&result, 1, row);
    duplicate_message_id_count =
        (guint64) duckdb_value_uint64 (&result, 2, row);
    subject = duckdb_store_dup_nullable_varchar (&result, 3, row);
    from_addr = duckdb_store_dup_nullable_varchar (&result, 4, row);
    sender_domain = duckdb_store_dup_nullable_varchar (&result, 5, row);
    to_addr = duckdb_store_dup_nullable_varchar (&result, 6, row);
    cc_addr = duckdb_store_dup_nullable_varchar (&result, 7, row);
    bcc_addr = duckdb_store_dup_nullable_varchar (&result, 8, row);
    date_raw = duckdb_store_dup_nullable_varchar (&result, 9, row);
    has_date_unix_us = wyrebox_rfc5322_date_parse_unix_us (date_raw,
        &date_unix_us);
    journal_offset = (guint64) duckdb_value_uint64 (&result, 10, row);
    journal_sequence = (guint64) duckdb_value_uint64 (&result, 11, row);

    if (!duckdb_store_bind_varchar (statement, 1, message_id, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 2, rfc_message_id,
            error) ||
        !duckdb_store_bind_uint64 (statement, 3,
            duplicate_message_id_count, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 4, subject, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 5, from_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 6, sender_domain,
            error) ||
        !duckdb_store_bind_nullable_varchar (statement, 7, to_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 8, cc_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 9, bcc_addr, error) ||
        !duckdb_store_bind_nullable_varchar (statement, 10, date_raw, error) ||
        !duckdb_store_bind_nullable_int64 (statement, 11, has_date_unix_us,
            date_unix_us, error) ||
        !duckdb_store_bind_uint64 (statement, 12, journal_offset, error) ||
        !duckdb_store_bind_uint64 (statement, 13, journal_sequence, error) ||
        !duckdb_store_execute_prepared (statement, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
duckdb_store_add_message_header_date_unix_us (WyreboxSchemaMetadataStoreDuckdb
    *self, GError **error)
{
  g_autoptr (GError) current_error = NULL;
  g_autoptr (GError) v6_error = NULL;

  if (duckdb_store_validate_message_header_table (self, &current_error))
    return TRUE;

  if (!duckdb_store_validate_message_header_table_v6 (self, &v6_error)) {
    g_propagate_error (error, g_steal_pointer (&v6_error));
    return FALSE;
  }

  return duckdb_store_query (self,
      "CREATE TABLE message_headers_replacement ("
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
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_copy_message_headers_with_date_unix_us (self, error)
      && duckdb_store_query (self, "DROP TABLE message_headers;", error)
      && duckdb_store_query (self,
      "ALTER TABLE message_headers_replacement RENAME TO message_headers;",
      error)
      && duckdb_store_validate_message_header_table (self, error);
}

static gboolean
    duckdb_store_add_message_header_provenance_spans
    (WyreboxSchemaMetadataStoreDuckdb * self, GError ** error)
{
  g_autoptr (GError) current_error = NULL;
  g_autoptr (GError) v9_ready_error = NULL;

  if (duckdb_store_validate_message_header_table_v9 (self, &current_error))
    return TRUE;

  if (!duckdb_store_validate_message_header_table (self, &v9_ready_error)) {
    g_propagate_error (error, g_steal_pointer (&v9_ready_error));
    return FALSE;
  }

  return duckdb_store_query (self,
      "ALTER TABLE message_headers ADD COLUMN message_id_span_start UBIGINT;",
      error)
      && duckdb_store_query (self,
      "ALTER TABLE message_headers ADD COLUMN message_id_span_end UBIGINT;",
      error)
      && duckdb_store_query (self,
      "ALTER TABLE message_headers ADD COLUMN subject_span_start UBIGINT;",
      error)
      && duckdb_store_query (self,
      "ALTER TABLE message_headers ADD COLUMN subject_span_end UBIGINT;", error)
      && duckdb_store_validate_message_header_table_v9 (self, error);
}

static gboolean
duckdb_store_create_derived_view_memberships (WyreboxSchemaMetadataStoreDuckdb
    *self, GError **error)
{
  return duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS derived_view_memberships ("
      "membership_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "view_id VARCHAR NOT NULL,"
      "message_id VARCHAR NOT NULL,"
      "uid UBIGINT NOT NULL CHECK(uid >= 1),"
      "is_visible BOOLEAN NOT NULL,"
      "rule_version_hash VARCHAR NOT NULL,"
      "materialized_at_unix_us UBIGINT NOT NULL,"
      "UNIQUE(account_id, view_id, uid),"
      "UNIQUE(account_id, view_id, message_id, rule_version_hash)" ");", error);
}

static gboolean
duckdb_store_scope_derived_views_by_account (WyreboxSchemaMetadataStoreDuckdb
    *self, GError **error)
{
  return duckdb_store_query (self,
      "CREATE TABLE derived_views_replacement ("
      "view_id VARCHAR NOT NULL,"
      "account_id VARCHAR NOT NULL,"
      "imap_name VARCHAR NOT NULL,"
      "definition_ref VARCHAR NOT NULL,"
      "is_selectable BOOLEAN NOT NULL,"
      "is_visible BOOLEAN NOT NULL,"
      "PRIMARY KEY(account_id, view_id),"
      "UNIQUE(account_id, imap_name)" ");", error)
      && duckdb_store_query (self,
      "INSERT INTO derived_views_replacement ("
      "view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible"
      ") SELECT view_id, account_id, imap_name, definition_ref, "
      "is_selectable, is_visible FROM derived_views;", error)
      && duckdb_store_query (self,
      "CREATE TABLE derived_view_memberships_replacement ("
      "membership_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "view_id VARCHAR NOT NULL,"
      "message_id VARCHAR NOT NULL,"
      "uid UBIGINT NOT NULL CHECK(uid >= 1),"
      "is_visible BOOLEAN NOT NULL,"
      "rule_version_hash VARCHAR NOT NULL,"
      "materialized_at_unix_us UBIGINT NOT NULL,"
      "UNIQUE(account_id, view_id, uid),"
      "UNIQUE(account_id, view_id, message_id, rule_version_hash)" ");", error)
      && duckdb_store_query (self,
      "INSERT INTO derived_view_memberships_replacement ("
      "membership_id, account_id, view_id, message_id, uid, is_visible, "
      "rule_version_hash, materialized_at_unix_us"
      ") SELECT membership_id, account_id, view_id, message_id, uid, "
      "is_visible, rule_version_hash, materialized_at_unix_us "
      "FROM derived_view_memberships;", error)
      && duckdb_store_query (self,
      "DROP TABLE derived_view_memberships;", error)
      && duckdb_store_query (self, "DROP TABLE derived_views;", error)
      && duckdb_store_query (self,
      "ALTER TABLE derived_views_replacement RENAME TO derived_views;", error)
      && duckdb_store_query (self,
      "ALTER TABLE derived_view_memberships_replacement "
      "RENAME TO derived_view_memberships;", error);
}

static gboolean
duckdb_store_create_object_reachability_view (WyreboxSchemaMetadataStoreDuckdb
    *self, GError **error)
{
  return duckdb_store_query (self,
      "DROP VIEW IF EXISTS object_reachability;"
      "CREATE VIEW object_reachability AS "
      "SELECT o.object_id, o.size_bytes, "
      "CAST(COALESCE(message_refs.message_reference_count, 0) AS UBIGINT) "
      "AS message_reference_count, "
      "CAST(COALESCE(ordinary.visible_mailbox_membership_count, 0) AS UBIGINT) "
      "AS visible_mailbox_membership_count, "
      "CAST(COALESCE(derived.visible_derived_view_membership_count, 0) AS UBIGINT) "
      "AS visible_derived_view_membership_count, "
      "COALESCE(ordinary.visible_mailbox_membership_count, 0) > 0 "
      "AS is_gc_reachable, "
      "COALESCE(ordinary.visible_mailbox_membership_count, 0) = 0 "
      "AS is_gc_candidate "
      "FROM objects o "
      "LEFT JOIN ("
      "SELECT object_id, COUNT(*) AS message_reference_count "
      "FROM messages "
      "GROUP BY object_id"
      ") message_refs ON message_refs.object_id = o.object_id "
      "LEFT JOIN ("
      "SELECT m.object_id, COUNT(*) AS message_reference_count, "
      "CAST(SUM(CASE WHEN mm.is_visible THEN 1 ELSE 0 END) AS UBIGINT) "
      "AS visible_mailbox_membership_count "
      "FROM mailbox_memberships mm "
      "JOIN messages m ON m.message_id = mm.message_id "
      "GROUP BY m.object_id"
      ") ordinary ON ordinary.object_id = o.object_id "
      "LEFT JOIN ("
      "SELECT m.object_id, "
      "CAST(SUM(CASE WHEN dvm.is_visible THEN 1 ELSE 0 END) AS UBIGINT) "
      "AS visible_derived_view_membership_count "
      "FROM derived_view_memberships dvm "
      "JOIN messages m ON m.message_id = dvm.message_id "
      "GROUP BY m.object_id"
      ") derived ON derived.object_id = o.object_id;", error);
}

static gboolean
duckdb_store_create_bootstrap_catalog (WyreboxSchemaMetadataStoreDuckdb *self,
    GError **error)
{
  return duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS accounts ("
      "account_id VARCHAR PRIMARY KEY" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS objects ("
      "object_id VARCHAR PRIMARY KEY,"
      "size_bytes UBIGINT NOT NULL" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS messages ("
      "message_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "object_id VARCHAR NOT NULL,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS message_facts ("
      "fact_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "message_id VARCHAR NOT NULL,"
      "object_id VARCHAR NOT NULL,"
      "predicate VARCHAR NOT NULL,"
      "args_json VARCHAR NOT NULL,"
      "source VARCHAR NOT NULL,"
      "confidence_ppm UBIGINT NOT NULL,"
      "created_at_unix_us UBIGINT NOT NULL,"
      "retracted_at_unix_us UBIGINT NOT NULL,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL" ");", error)
      && duckdb_store_validate_message_fact_table (self, error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS mailboxes ("
      "mailbox_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "imap_name VARCHAR NOT NULL,"
      "is_selectable BOOLEAN NOT NULL,"
      "is_visible BOOLEAN NOT NULL,"
      "UNIQUE(account_id, imap_name)" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS mailbox_memberships ("
      "membership_id VARCHAR PRIMARY KEY,"
      "account_id VARCHAR NOT NULL,"
      "mailbox_id VARCHAR NOT NULL,"
      "message_id VARCHAR NOT NULL,"
      "uid UBIGINT NOT NULL CHECK(uid >= 1),"
      "internal_date_unix_us UBIGINT NOT NULL,"
      "journal_offset UBIGINT NOT NULL,"
      "journal_sequence UBIGINT NOT NULL,"
      "is_visible BOOLEAN NOT NULL,"
      "UNIQUE(mailbox_id, uid),"
      "UNIQUE(journal_offset, journal_sequence, mailbox_id)" ");", error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS derived_views ("
      "view_id VARCHAR NOT NULL,"
      "account_id VARCHAR NOT NULL,"
      "imap_name VARCHAR NOT NULL,"
      "definition_ref VARCHAR NOT NULL,"
      "is_selectable BOOLEAN NOT NULL,"
      "is_visible BOOLEAN NOT NULL,"
      "PRIMARY KEY(account_id, view_id),"
      "UNIQUE(account_id, imap_name)" ");", error)
      && duckdb_store_create_derived_view_memberships (self, error)
      && duckdb_store_query (self,
      "CREATE TABLE IF NOT EXISTS mailbox_uid_state ("
      "account_id VARCHAR NOT NULL,"
      "namespace_kind VARCHAR NOT NULL "
      "CHECK(namespace_kind IN ('mailbox','derived_view')),"
      "namespace_id VARCHAR NOT NULL,"
      "uidnext UBIGINT NOT NULL CHECK(uidnext >= 1),"
      "uidvalidity UBIGINT NOT NULL CHECK(uidvalidity >= 1),"
      "PRIMARY KEY(account_id, namespace_kind, namespace_id)" ");", error);
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
    duckdb_store_load_materialization_manifest
    (WyreboxSchemaMetadataStoreDuckdb * self,
    const gchar * run_id, WyreboxMaterializationManifest * out_manifest,
    GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (duckdb_prepared_statement) statement = NULL;
  idx_t rows = 0;

  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);

  wyrebox_materialization_manifest_clear (out_manifest);

  if (!duckdb_store_prepare (self,
          "SELECT run_id, start_journal_offset, start_journal_sequence, "
          "end_journal_offset, end_journal_sequence, "
          "materialized_schema_version, object_store_identity, "
          "rule_package_version, view_package_version, engine_version, "
          "created_at_unix_us, completion_status, error_state "
          "FROM materialization_manifests WHERE run_id = ?;",
          &statement, error))
    return FALSE;

  if (duckdb_bind_varchar (statement, 1, run_id) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB materialization manifest run ID bind failed");
    return FALSE;
  }

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB materialization manifest load failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  rows = duckdb_row_count (&result);
  if (rows > 1 || duckdb_column_count (&result) != 13) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB materialization manifest result has invalid shape");
    return FALSE;
  }

  if (rows == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND, "materialization manifest %s not found", run_id);
    return FALSE;
  }

  {
    g_auto (WyreboxMaterializationManifest) loaded = { 0 };

    if (duckdb_value_is_null (&result, 0, 0) ||
        duckdb_value_is_null (&result, 1, 0) ||
        duckdb_value_is_null (&result, 2, 0) ||
        duckdb_value_is_null (&result, 3, 0) ||
        duckdb_value_is_null (&result, 4, 0) ||
        duckdb_value_is_null (&result, 5, 0) ||
        duckdb_value_is_null (&result, 6, 0) ||
        duckdb_value_is_null (&result, 7, 0) ||
        duckdb_value_is_null (&result, 8, 0) ||
        duckdb_value_is_null (&result, 9, 0) ||
        duckdb_value_is_null (&result, 10, 0) ||
        duckdb_value_is_null (&result, 11, 0)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB materialization manifest row has NULL required fields");
      return FALSE;
    }

    loaded.run_id = duckdb_store_dup_nullable_varchar (&result, 0, 0);
    loaded.start_journal_offset = (guint64) duckdb_value_uint64 (&result, 1, 0);
    loaded.start_journal_sequence =
        (guint64) duckdb_value_uint64 (&result, 2, 0);
    loaded.end_journal_offset = (guint64) duckdb_value_uint64 (&result, 3, 0);
    loaded.end_journal_sequence = (guint64) duckdb_value_uint64 (&result, 4, 0);
    loaded.materialized_schema_version =
        (guint64) duckdb_value_uint64 (&result, 5, 0);
    loaded.object_store_identity = duckdb_store_dup_nullable_varchar (&result,
        6, 0);
    loaded.rule_package_version = duckdb_store_dup_nullable_varchar (&result,
        7, 0);
    loaded.view_package_version = duckdb_store_dup_nullable_varchar (&result,
        8, 0);
    loaded.engine_version = duckdb_store_dup_nullable_varchar (&result, 9, 0);
    loaded.created_at_unix_us = (guint64) duckdb_value_uint64 (&result, 10, 0);
    loaded.completion_status = duckdb_store_dup_nullable_varchar (&result, 11,
        0);
    loaded.error_state = duckdb_store_dup_nullable_varchar (&result, 12, 0);

    if (!wyrebox_materialization_manifest_validate (&loaded, error))
      return FALSE;

    *out_manifest = loaded;
    loaded.run_id = NULL;
    loaded.object_store_identity = NULL;
    loaded.rule_package_version = NULL;
    loaded.view_package_version = NULL;
    loaded.engine_version = NULL;
    loaded.completion_status = NULL;
    loaded.error_state = NULL;
  }

  return TRUE;
}

static gboolean
    duckdb_store_load_latest_materialization_manifest
    (WyreboxSchemaMetadataStoreDuckdb * self,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (duckdb_prepared_statement) statement = NULL;
  idx_t rows = 0;

  g_return_val_if_fail (out_manifest != NULL, FALSE);

  wyrebox_materialization_manifest_clear (out_manifest);

  if (!duckdb_store_prepare (self,
          "SELECT run_id, start_journal_offset, start_journal_sequence, "
          "end_journal_offset, end_journal_sequence, "
          "materialized_schema_version, object_store_identity, "
          "rule_package_version, view_package_version, engine_version, "
          "created_at_unix_us, completion_status, error_state "
          "FROM materialization_manifests "
          "ORDER BY created_at_unix_us DESC, run_id DESC "
          "LIMIT 1;", &statement, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB latest materialization manifest load failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  rows = duckdb_row_count (&result);
  if (rows > 1 || duckdb_column_count (&result) != 13) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB latest materialization manifest result has invalid shape");
    return FALSE;
  }

  if (rows == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND, "materialization manifest catalog is empty");
    return FALSE;
  }

  {
    g_auto (WyreboxMaterializationManifest) loaded = { 0 };

    if (duckdb_value_is_null (&result, 0, 0) ||
        duckdb_value_is_null (&result, 1, 0) ||
        duckdb_value_is_null (&result, 2, 0) ||
        duckdb_value_is_null (&result, 3, 0) ||
        duckdb_value_is_null (&result, 4, 0) ||
        duckdb_value_is_null (&result, 5, 0) ||
        duckdb_value_is_null (&result, 6, 0) ||
        duckdb_value_is_null (&result, 7, 0) ||
        duckdb_value_is_null (&result, 8, 0) ||
        duckdb_value_is_null (&result, 9, 0) ||
        duckdb_value_is_null (&result, 10, 0) ||
        duckdb_value_is_null (&result, 11, 0)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB latest materialization manifest row has NULL required fields");
      return FALSE;
    }

    loaded.run_id = duckdb_store_dup_nullable_varchar (&result, 0, 0);
    loaded.start_journal_offset = (guint64) duckdb_value_uint64 (&result, 1, 0);
    loaded.start_journal_sequence =
        (guint64) duckdb_value_uint64 (&result, 2, 0);
    loaded.end_journal_offset = (guint64) duckdb_value_uint64 (&result, 3, 0);
    loaded.end_journal_sequence = (guint64) duckdb_value_uint64 (&result, 4, 0);
    loaded.materialized_schema_version =
        (guint64) duckdb_value_uint64 (&result, 5, 0);
    loaded.object_store_identity = duckdb_store_dup_nullable_varchar (&result,
        6, 0);
    loaded.rule_package_version = duckdb_store_dup_nullable_varchar (&result,
        7, 0);
    loaded.view_package_version = duckdb_store_dup_nullable_varchar (&result,
        8, 0);
    loaded.engine_version = duckdb_store_dup_nullable_varchar (&result, 9, 0);
    loaded.created_at_unix_us = (guint64) duckdb_value_uint64 (&result, 10, 0);
    loaded.completion_status = duckdb_store_dup_nullable_varchar (&result, 11,
        0);
    loaded.error_state = duckdb_store_dup_nullable_varchar (&result, 12, 0);

    if (!wyrebox_materialization_manifest_validate (&loaded, error))
      return FALSE;

    *out_manifest = loaded;
    loaded.run_id = NULL;
    loaded.object_store_identity = NULL;
    loaded.rule_package_version = NULL;
    loaded.view_package_version = NULL;
    loaded.engine_version = NULL;
    loaded.completion_status = NULL;
    loaded.error_state = NULL;
  }

  return TRUE;
}

static gboolean
    duckdb_store_save_materialization_manifest
    (WyreboxSchemaMetadataStoreDuckdb * self,
    const WyreboxMaterializationManifest * manifest, GError ** error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;

  g_return_val_if_fail (manifest != NULL, FALSE);

  if (!wyrebox_materialization_manifest_validate (manifest, error))
    return FALSE;

  if (!duckdb_store_query (self, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!duckdb_store_create_schema (self, error) ||
      !duckdb_store_prepare (self,
          "INSERT INTO materialization_manifests ("
          "run_id, start_journal_offset, start_journal_sequence, "
          "end_journal_offset, end_journal_sequence, "
          "materialized_schema_version, object_store_identity, "
          "rule_package_version, view_package_version, engine_version, "
          "created_at_unix_us, completion_status, error_state"
          ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
          &statement, error) ||
      !duckdb_store_bind_varchar (statement, 1, manifest->run_id, error) ||
      !duckdb_store_bind_uint64 (statement, 2,
          manifest->start_journal_offset, error) ||
      !duckdb_store_bind_uint64 (statement, 3,
          manifest->start_journal_sequence, error) ||
      !duckdb_store_bind_uint64 (statement, 4,
          manifest->end_journal_offset, error) ||
      !duckdb_store_bind_uint64 (statement, 5,
          manifest->end_journal_sequence, error) ||
      !duckdb_store_bind_uint64 (statement, 6,
          manifest->materialized_schema_version, error) ||
      !duckdb_store_bind_varchar (statement, 7,
          manifest->object_store_identity, error) ||
      !duckdb_store_bind_varchar (statement, 8,
          manifest->rule_package_version, error) ||
      !duckdb_store_bind_varchar (statement, 9,
          manifest->view_package_version, error) ||
      !duckdb_store_bind_varchar (statement, 10,
          manifest->engine_version, error) ||
      !duckdb_store_bind_uint64 (statement, 11,
          manifest->created_at_unix_us, error) ||
      !duckdb_store_bind_varchar (statement, 12,
          manifest->completion_status, error) ||
      !duckdb_store_bind_nullable_varchar (statement, 13,
          manifest->error_state, error) ||
      !duckdb_store_execute_prepared (statement, error) ||
      !duckdb_store_query (self, "COMMIT;", error)) {
    duckdb_store_rollback_quietly (self);
    return FALSE;
  }

  return TRUE;
}

static gboolean
    duckdb_store_load_materialization_artifact_checksum
    (WyreboxSchemaMetadataStoreDuckdb * self, const gchar * run_id,
    const gchar * artifact_kind, const gchar * artifact_name,
    WyreboxMaterializationArtifactChecksum * out_checksum, GError ** error)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (duckdb_prepared_statement) statement = NULL;
  idx_t rows = 0;

  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (artifact_kind != NULL, FALSE);
  g_return_val_if_fail (artifact_name != NULL, FALSE);
  g_return_val_if_fail (out_checksum != NULL, FALSE);

  wyrebox_materialization_artifact_checksum_clear (out_checksum);

  if (!duckdb_store_prepare (self,
          "SELECT run_id, artifact_kind, artifact_name, row_count, "
          "checksum_algorithm, logical_checksum "
          "FROM materialization_artifact_checksums "
          "WHERE run_id = ? AND artifact_kind = ? AND artifact_name = ?;",
          &statement, error))
    return FALSE;

  if (duckdb_bind_varchar (statement, 1, run_id) != DuckDBSuccess ||
      duckdb_bind_varchar (statement, 2, artifact_kind) != DuckDBSuccess ||
      duckdb_bind_varchar (statement, 3, artifact_name) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB materialization artifact checksum bind failed");
    return FALSE;
  }

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB materialization artifact checksum load failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  rows = duckdb_row_count (&result);
  if (rows > 1 || duckdb_column_count (&result) != 6) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB materialization artifact checksum result has invalid shape");
    return FALSE;
  }

  if (rows == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "materialization artifact checksum %s/%s/%s not found", run_id,
        artifact_kind, artifact_name);
    return FALSE;
  }

  if (duckdb_value_is_null (&result, 0, 0) ||
      duckdb_value_is_null (&result, 1, 0) ||
      duckdb_value_is_null (&result, 2, 0) ||
      duckdb_value_is_null (&result, 3, 0) ||
      duckdb_value_is_null (&result, 4, 0) ||
      duckdb_value_is_null (&result, 5, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB materialization artifact checksum row has NULL required fields");
    return FALSE;
  }

  out_checksum->run_id = duckdb_store_dup_nullable_varchar (&result, 0, 0);
  out_checksum->artifact_kind = duckdb_store_dup_nullable_varchar (&result, 1,
      0);
  out_checksum->artifact_name = duckdb_store_dup_nullable_varchar (&result, 2,
      0);
  out_checksum->row_count = (guint64) duckdb_value_uint64 (&result, 3, 0);
  out_checksum->checksum_algorithm =
      duckdb_store_dup_nullable_varchar (&result, 4, 0);
  out_checksum->logical_checksum =
      duckdb_store_dup_nullable_varchar (&result, 5, 0);

  if (!wyrebox_materialization_artifact_checksum_validate (out_checksum, error))
    return FALSE;

  return TRUE;
}

static gboolean
    duckdb_store_save_materialization_artifact_checksum
    (WyreboxSchemaMetadataStoreDuckdb * self,
    const WyreboxMaterializationArtifactChecksum * checksum, GError ** error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_auto (WyreboxMaterializationManifest) manifest = { 0 };

  g_return_val_if_fail (checksum != NULL, FALSE);

  if (!wyrebox_materialization_artifact_checksum_validate (checksum, error))
    return FALSE;

  store = WYREBOX_SCHEMA_METADATA_STORE (g_object_ref (self));
  if (!wyrebox_schema_metadata_store_load_materialization_manifest (store,
          checksum->run_id, &manifest, error))
    return FALSE;

  if (g_strcmp0 (manifest.completion_status, "completed") != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "materialization artifact checksum %s requires a completed manifest",
        checksum->run_id);
    return FALSE;
  }

  if (!duckdb_store_query (self, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!duckdb_store_prepare (self,
          "INSERT INTO materialization_artifact_checksums ("
          "run_id, artifact_kind, artifact_name, row_count, "
          "checksum_algorithm, logical_checksum"
          ") VALUES (?, ?, ?, ?, ?, ?);",
          &statement, error) ||
      !duckdb_store_bind_varchar (statement, 1, checksum->run_id, error) ||
      !duckdb_store_bind_varchar (statement, 2, checksum->artifact_kind,
          error) ||
      !duckdb_store_bind_varchar (statement, 3, checksum->artifact_name,
          error) ||
      !duckdb_store_bind_uint64 (statement, 4, checksum->row_count, error) ||
      !duckdb_store_bind_varchar (statement, 5, checksum->checksum_algorithm,
          error) ||
      !duckdb_store_bind_varchar (statement, 6, checksum->logical_checksum,
          error) ||
      !duckdb_store_execute_prepared (statement, error) ||
      !duckdb_store_query (self, "COMMIT;", error)) {
    duckdb_store_rollback_quietly (self);
    return FALSE;
  }

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

static gboolean
    wyrebox_schema_metadata_store_duckdb_load_materialization_manifest
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  return duckdb_store_load_materialization_manifest (duckdb_store, run_id,
      out_manifest, error);
}

static gboolean
    wyrebox_schema_metadata_store_duckdb_load_latest_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  return duckdb_store_load_latest_materialization_manifest (duckdb_store,
      out_manifest, error);
}

static gboolean
    wyrebox_schema_metadata_store_duckdb_save_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationManifest * manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  return duckdb_store_save_materialization_manifest (duckdb_store, manifest,
      error);
}

static gboolean
    wyrebox_schema_metadata_store_duckdb_load_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    const gchar * artifact_kind, const gchar * artifact_name,
    WyreboxMaterializationArtifactChecksum * out_checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (artifact_kind != NULL, FALSE);
  g_return_val_if_fail (artifact_name != NULL, FALSE);
  g_return_val_if_fail (out_checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  return duckdb_store_load_materialization_artifact_checksum (duckdb_store,
      run_id, artifact_kind, artifact_name, out_checksum, error);
}

static gboolean
    wyrebox_schema_metadata_store_duckdb_save_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationArtifactChecksum * checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  return duckdb_store_save_materialization_artifact_checksum (duckdb_store,
      checksum, error);
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
  WyreboxSchemaMetadataStoreDuckdb *duckdb_store = NULL;

  g_return_val_if_fail (g_type_check_instance_is_a (
          (GTypeInstance *) self,
          wyrebox_schema_metadata_store_duckdb_get_type ()), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  duckdb_store = (WyreboxSchemaMetadataStoreDuckdb *) self;
  (void) source_version;
  (void) target_version;

  switch (operation) {
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS:
    case WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_OBJECT_REACHABILITY_VIEW:
      break;
    default:
      goto unsupported;
  }

  if (!duckdb_store_query (duckdb_store, "BEGIN TRANSACTION;", error))
    return FALSE;

  if (!((operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP
              && duckdb_store_create_bootstrap_catalog (duckdb_store, error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES
              && duckdb_store_create_message_attribute_tables (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE
              && duckdb_store_create_message_header_table_v3 (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_DERIVED_VIEW_MEMBERSHIPS
              && duckdb_store_create_derived_view_memberships (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_SCOPE_DERIVED_VIEWS_BY_ACCOUNT
              && duckdb_store_scope_derived_views_by_account (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN
              && duckdb_store_add_message_header_sender_domain (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US
              && duckdb_store_add_message_header_date_unix_us (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS
              && duckdb_store_add_message_header_provenance_spans (duckdb_store,
                  error))
          || (operation ==
              WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_OBJECT_REACHABILITY_VIEW
              && duckdb_store_create_object_reachability_view (duckdb_store,
                  error))) ||
      !duckdb_store_query (duckdb_store, "COMMIT;", error)) {
    duckdb_store_rollback_quietly (duckdb_store);
    return FALSE;
  }

  return TRUE;

unsupported:
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
  store_class->load_materialization_manifest =
      wyrebox_schema_metadata_store_duckdb_load_materialization_manifest;
  store_class->load_latest_materialization_manifest =
      wyrebox_schema_metadata_store_duckdb_load_latest_materialization_manifest;
  store_class->save_materialization_manifest =
      wyrebox_schema_metadata_store_duckdb_save_materialization_manifest;
  store_class->load_materialization_artifact_checksum =
      wyrebox_schema_metadata_store_duckdb_load_materialization_artifact_checksum;
  store_class->save_materialization_artifact_checksum =
      wyrebox_schema_metadata_store_duckdb_save_materialization_artifact_checksum;
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

gboolean
    wyrebox_schema_metadata_store_load_materialization_manifest
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    WyreboxMaterializationManifest * out_manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->load_materialization_manifest == NULL)
    return FALSE;

  return klass->load_materialization_manifest (self, run_id, out_manifest,
      error);
}

gboolean
    wyrebox_schema_metadata_store_load_latest_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    WyreboxMaterializationManifest * out_manifest, GError ** error) {
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (out_manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->load_latest_materialization_manifest == NULL)
    return FALSE;

  return klass->load_latest_materialization_manifest (self, out_manifest,
      error);
}

gboolean
    wyrebox_schema_metadata_store_save_materialization_manifest
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationManifest * manifest, GError ** error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (manifest != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->save_materialization_manifest == NULL)
    return FALSE;

  return klass->save_materialization_manifest (self, manifest, error);
}

gboolean
    wyrebox_schema_metadata_store_load_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self, const gchar * run_id,
    const gchar * artifact_kind, const gchar * artifact_name,
    WyreboxMaterializationArtifactChecksum * out_checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (run_id != NULL, FALSE);
  g_return_val_if_fail (artifact_kind != NULL, FALSE);
  g_return_val_if_fail (artifact_name != NULL, FALSE);
  g_return_val_if_fail (out_checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->load_materialization_artifact_checksum == NULL)
    return FALSE;

  return klass->load_materialization_artifact_checksum (self, run_id,
      artifact_kind, artifact_name, out_checksum, error);
}

gboolean
    wyrebox_schema_metadata_store_save_materialization_artifact_checksum
    (WyreboxSchemaMetadataStore * self,
    const WyreboxMaterializationArtifactChecksum * checksum, GError ** error)
{
  WyreboxSchemaMetadataStoreClass *klass = NULL;

  g_return_val_if_fail (WYREBOX_IS_SCHEMA_METADATA_STORE (self), FALSE);
  g_return_val_if_fail (checksum != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  klass = WYREBOX_SCHEMA_METADATA_STORE_GET_CLASS (self);
  if (klass == NULL || klass->save_materialization_artifact_checksum == NULL)
    return FALSE;

  return klass->save_materialization_artifact_checksum (self, checksum, error);
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
    duckdb_close (&self->database);
    return NULL;
  }

  if (!duckdb_store_create_schema (self, error)) {
    duckdb_close (&self->database);
    return NULL;
  }

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
