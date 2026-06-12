/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-build-config.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

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
test_duckdb_factory_fails_fast_until_implemented (void)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (GError) error = NULL;

  store =
      wyrebox_schema_metadata_store_new_duckdb ("/tmp/wyrebox-schema.duckdb",
      &error);

  g_assert_null (store);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);

  g_assert_nonnull (strstr (error->message, "not implemented"));
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
  g_test_add_func
      ("/migration/schema-metadata-store/duckdb-factory-fails-fast",
      test_duckdb_factory_fails_fast_until_implemented);

  return g_test_run ();
}
