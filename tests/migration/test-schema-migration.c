/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"

#include <glib.h>
#include <gio/gio.h>

typedef struct
{
  guint operation_call_count;
  guint validation_call_count;
  guint expected_fail_at_call;
} TestMigrationFixtureData;

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
test_checkpoint_precondition_missing_blocks_legacy_migration (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  TestMigrationFixtureData fixture_data = { 0 };

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_record_step_calls,
      test_schema_migration_record_validation_calls, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
  g_assert_cmpuint (fixture_data.operation_call_count, ==, 0);
  g_assert_cmpuint (fixture_data.validation_call_count, ==, 0);
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
  g_assert_true (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, first_version, &error));
  g_assert_no_error (error);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, first_version);
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

  g_assert_true (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_no_error (error);
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

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, future_version, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, original_version);
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

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, original_version);
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

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, requested_version, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (metadata.schema_version, ==, current_version);
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
  wyrebox_schema_migration_set_test_step_hooks (migration,
      test_schema_migration_force_op_failure, NULL, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
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
  wyrebox_schema_migration_set_test_step_hooks (migration,
      NULL,
      test_schema_migration_force_validation_failure, &fixture_data, NULL);

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (metadata.schema_version, ==, 0);
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
