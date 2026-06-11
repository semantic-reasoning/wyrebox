/*
 * Copyright (C) 2026
 */

#include "wyrebox-schema-migration.h"

#include <glib.h>
#include <gio/gio.h>

static void
test_missing_schema_metadata_initializes_to_current (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 current_version = 0;

  migration = wyrebox_schema_migration_new ();
  current_version = wyrebox_schema_migration_get_current_schema_version ();

  g_assert_false (metadata.schema_version_present);
  g_assert_true (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_no_error (error);
  g_assert_true (metadata.schema_version_present);
  g_assert_cmpuint (metadata.schema_version, ==, current_version);
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

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version =
      wyrebox_schema_migration_get_current_schema_version () + 1;
  future_version = metadata.schema_version;

  g_assert_false (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_cmpuint (metadata.schema_version, ==, future_version);
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
test_explicit_forward_path_is_applied (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 current_version = 0;

  migration = wyrebox_schema_migration_new ();
  current_version = wyrebox_schema_migration_get_current_schema_version ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;

  g_assert_true (wyrebox_schema_migration_evaluate_to_current (migration,
          &metadata, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (metadata.schema_version, ==, current_version);
}

static void
test_failed_migration_does_not_promote_target_version (void)
{
  g_autoptr (WyreboxSchemaMigration) migration = NULL;
  g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
  g_autoptr (GError) error = NULL;
  guint64 failed_target_version = 0;

  migration = wyrebox_schema_migration_new ();
  metadata.schema_version_present = TRUE;
  metadata.schema_version = 0;
  failed_target_version =
      wyrebox_schema_migration_get_current_schema_version () + 1;

  g_assert_false (wyrebox_schema_migration_evaluate_to_version (migration,
          &metadata, failed_target_version, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
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

  g_test_add_func ("/migration/schema/missing-metadata-initializes-to-current",
      test_missing_schema_metadata_initializes_to_current);
  g_test_add_func ("/migration/schema/current-version-is-noop",
      test_current_schema_version_is_noop);
  g_test_add_func ("/migration/schema/future-version-rejected",
      test_unknown_future_schema_version_is_rejected);
  g_test_add_func ("/migration/schema/downgrade-target-rejected",
      test_downgrade_target_is_rejected);
  g_test_add_func ("/migration/schema/explicit-forward-path",
      test_explicit_forward_path_is_applied);
  g_test_add_func ("/migration/schema/failed-migration-does-not-promote",
      test_failed_migration_does_not_promote_target_version);
  g_test_add_func ("/migration/schema/version-constants",
      test_schema_version_constants_are_testable);

  return g_test_run ();
}
