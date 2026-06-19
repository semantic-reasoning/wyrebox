#include "wyrebox-backup-set.h"

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

static WyreboxBackupSet
make_valid_set (void)
{
  WyreboxBackupSet set = { 0 };
  WyreboxBackupSetEntry *entry = NULL;

  set.backup_id = g_strdup ("backup-20260619-0001");
  set.entries = g_ptr_array_new_with_free_func (
      (GDestroyNotify) wyrebox_backup_set_entry_free);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("raw-objects");
  entry->relative_path = g_strdup ("raw/objects.tar");
  entry->digest =
      g_strdup
      ("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("canonical-journal");
  entry->relative_path = g_strdup ("journal/segment.wbj");
  entry->digest =
      g_strdup
      ("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("schema-metadata");
  entry->relative_path = g_strdup ("schema/metadata.db");
  entry->digest =
      g_strdup
      ("sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("rule-definitions");
  entry->relative_path = g_strdup ("rules/current.datalog");
  entry->digest =
      g_strdup
      ("sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("view-definitions");
  entry->relative_path = g_strdup ("views/current.capnp");
  entry->digest =
      g_strdup
      ("sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("fact-records");
  entry->relative_path = g_strdup ("facts/current.wbf");
  entry->digest =
      g_strdup
      ("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("configuration");
  entry->relative_path = g_strdup ("config/runtime.ini");
  entry->digest =
      g_strdup
      ("sha256:1111111111111111111111111111111111111111111111111111111111111111");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  return set;
}

static void
test_complete_set_matches_manifest (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();
  g_auto (WyreboxBackupSet) set = make_valid_set ();
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_backup_set_has_complete_durable_units (&set,
          &manifest, &error));
  g_assert_no_error (error);
}

static void
test_missing_unit_is_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();
  g_auto (WyreboxBackupSet) set = make_valid_set ();
  g_autoptr (GError) error = NULL;

  g_ptr_array_remove_index (set.entries, 2);

  g_assert_false (wyrebox_backup_set_has_complete_durable_units (&set,
          &manifest, &error));
  g_assert_nonnull (error);
}

static void
test_duplicate_unit_is_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();
  g_auto (WyreboxBackupSet) set = make_valid_set ();
  g_autoptr (GError) error = NULL;
  WyreboxBackupSetEntry *entry = NULL;

  entry = g_new0 (WyreboxBackupSetEntry, 1);
  entry->unit_name = g_strdup ("raw-objects");
  entry->relative_path = g_strdup ("raw/duplicate.tar");
  entry->digest =
      g_strdup
      ("sha256:9999999999999999999999999999999999999999999999999999999999999999");
  entry->size_bytes = 1;
  g_ptr_array_add (set.entries, entry);

  g_assert_false (wyrebox_backup_set_has_complete_durable_units (&set,
          &manifest, &error));
  g_assert_nonnull (error);
}

static void
test_backup_id_mismatch_is_rejected (void)
{
  g_auto (WyreboxBackupManifest) manifest = make_valid_manifest ();
  g_auto (WyreboxBackupSet) set = make_valid_set ();
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&set.backup_id, g_free);
  set.backup_id = g_strdup ("backup-20260619-9999");

  g_assert_false (wyrebox_backup_set_has_complete_durable_units (&set,
          &manifest, &error));
  g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/backup/set/complete", test_complete_set_matches_manifest);
  g_test_add_func ("/backup/set/missing-unit", test_missing_unit_is_rejected);
  g_test_add_func ("/backup/set/duplicate-unit",
      test_duplicate_unit_is_rejected);
  g_test_add_func ("/backup/set/backup-id-mismatch",
      test_backup_id_mismatch_is_rejected);

  return g_test_run ();
}
