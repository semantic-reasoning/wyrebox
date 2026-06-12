/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-build-config.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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
      "memory-store-rejects-unknown-operation",
      test_memory_store_rejects_unknown_migration_operation);
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-store/missing-metadata-load",
      test_duckdb_store_missing_metadata_load);
  g_test_add_func ("/migration/schema-metadata-store/duckdb-store/"
      "accepts-legacy-bootstrap-operation",
      test_duckdb_store_accepts_legacy_bootstrap_migration_operation);
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
