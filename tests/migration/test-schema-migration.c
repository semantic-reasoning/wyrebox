/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"
#include "wyrebox-schema-metadata-store.h"

#include <glib.h>
#include <gio/gio.h>

typedef struct
{
  guint operation_call_count;
  guint validation_call_count;
  guint expected_fail_at_call;
} TestMigrationFixtureData;

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
  if (source_version == 0 &&
      target_version ==
      wyrebox_schema_migration_get_first_supported_schema_version () &&
      ++(*fail_triggered) == expected_fail_at_call) {
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
  if (source_version == 0 &&
      target_version ==
      wyrebox_schema_migration_get_first_supported_schema_version ())
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
  if (source_version == 0 &&
      target_version ==
      wyrebox_schema_migration_get_first_supported_schema_version ())
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
  expected_version =
      wyrebox_schema_migration_get_first_supported_schema_version ();

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
test_missing_schema_metadata_initializes_to_first_supported_version (void)
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
  g_assert_cmpuint (first_version, ==, current_version);
  test_schema_migration_set_materialization_checkpoint_fields (&metadata);
  g_assert_true (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, first_version, &error));
  g_assert_no_error (error);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, first_version);
  g_assert_true (metadata.materialization_checkpoint_present);
  g_assert_cmpuint (metadata.materialization_checkpoint_journal_offset, ==,
      4096);
  g_assert_cmpuint (metadata.materialization_checkpoint_sequence, ==, 2048);
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
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 1);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 1);
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

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, 2, &error));
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
      (), ==, wyrebox_schema_migration_get_current_schema_version ());
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/migration/schema/missing-metadata-initializes-to-first",
      test_missing_schema_metadata_initializes_to_first_supported_version);
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
  g_test_add_func ("/migration/schema/explicit-forward-path",
      test_explicit_forward_path_succeeds_with_checkpoint_precondition);
  g_test_add_func
      ("/migration/schema/legacy-forward-path-without-checkpoint-fails",
      test_checkpoint_precondition_missing_blocks_legacy_migration);
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
