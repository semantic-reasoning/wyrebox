#include "wyrebox-backup-manifest.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxBackupManifest
make_valid_manifest (void)
{
  WyreboxBackupManifest manifest = { 0 };

  manifest.backup_id = g_strdup ("backup-20260619-0001");
  manifest.object_store_identity = g_strdup ("s3://bucket/wyrebox");
  manifest.journal_root_dir = g_strdup ("/var/lib/wyrebox/journal");
  manifest.schema_version = g_strdup ("schema-7");
  manifest.rule_package_version = g_strdup ("rules-3.2.1");
  manifest.view_package_version = g_strdup ("views-4.8.0");
  manifest.created_at_unix_us = 1;
  manifest.included_units =
      WYREBOX_BACKUP_UNIT_RAW_OBJECTS |
      WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL |
      WYREBOX_BACKUP_UNIT_SCHEMA_METADATA |
      WYREBOX_BACKUP_UNIT_RULE_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_VIEW_DEFINITIONS |
      WYREBOX_BACKUP_UNIT_FACT_RECORDS | WYREBOX_BACKUP_UNIT_CONFIGURATION;
  manifest.rebuildable_units =
      WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE |
      WYREBOX_BACKUP_UNIT_SEARCH_INDEXES |
      WYREBOX_BACKUP_UNIT_DERIVED_VIEW_CACHE |
      WYREBOX_BACKUP_UNIT_EXPORT_ARTIFACTS | WYREBOX_BACKUP_UNIT_RUNTIME_CACHES;

  return manifest;
}

static void
assert_invalid (WyreboxBackupManifest *manifest)
{
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_backup_manifest_validate (manifest, &error));
  g_assert_nonnull (error);
}

static void
test_valid_manifest (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_backup_manifest_validate (&manifest, &error));
  g_assert_no_error (error);
}

static void
test_missing_durable_unit_is_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();

  manifest.included_units &= ~WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL;
  assert_invalid (&manifest);
}

static void
test_rebuildable_unit_in_durable_set_is_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();

  manifest.included_units |= WYREBOX_BACKUP_UNIT_MATERIALIZED_DUCKDB_STATE;
  assert_invalid (&manifest);
}

static void
test_unknown_unit_bits_are_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();

  manifest.included_units |= (1u << 31);
  assert_invalid (&manifest);
}

static void
test_backup_unit_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_backup_unit_to_string
      (WYREBOX_BACKUP_UNIT_CANONICAL_JOURNAL), ==, "canonical-journal");
  g_assert_null (wyrebox_backup_unit_to_string ((WyreboxBackupUnit) 0));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/backup/manifest/valid", test_valid_manifest);
  g_test_add_func ("/backup/manifest/missing-durable-unit",
      test_missing_durable_unit_is_rejected);
  g_test_add_func ("/backup/manifest/rebuildable-unit-in-durable-set",
      test_rebuildable_unit_in_durable_set_is_rejected);
  g_test_add_func ("/backup/manifest/unknown-unit-bits",
      test_unknown_unit_bits_are_rejected);
  g_test_add_func ("/backup/manifest/unit-names-stable",
      test_backup_unit_names_are_stable);

  return g_test_run ();
}
