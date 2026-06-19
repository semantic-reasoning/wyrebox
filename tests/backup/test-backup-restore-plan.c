#include "wyrebox-backup-restore-plan.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxBackupRestorePlan
make_valid_plan (void)
{
  WyreboxBackupRestorePlan plan = { 0 };

  plan.restore_id = g_strdup ("restore-20260619-0001");
  plan.state_dir = g_strdup ("/var/lib/wyrebox/state");
  plan.backup_root_dir = g_strdup ("/var/lib/wyrebox/backup");
  plan.object_store_identity = g_strdup ("s3://bucket/wyrebox");
  plan.journal_root_dir = g_strdup ("/var/lib/wyrebox/journal");
  plan.schema_version = g_strdup ("schema-7");
  plan.rule_package_version = g_strdup ("rules-3.2.1");
  plan.view_package_version = g_strdup ("views-4.8.0");
  plan.expected_duckdb_snapshot_version = g_strdup ("duckdb-1");
  plan.expected_journal_offset = 100;
  plan.expected_journal_sequence = 7;
  plan.duckdb_snapshot_present = TRUE;
  plan.duckdb_snapshot_version = g_strdup ("duckdb-1");
  plan.duckdb_snapshot_journal_offset = 100;
  plan.duckdb_snapshot_journal_sequence = 7;

  return plan;
}

static void
assert_needs_rebuild_reason (WyreboxBackupRestorePlan *plan,
    WyreboxBackupRestoreRebuildReason expected)
{
  g_autoptr (GError) error = NULL;
  WyreboxBackupRestoreRebuildReason reason =
      wyrebox_backup_restore_plan_needs_rebuild (plan, &error);

  g_assert_no_error (error);
  g_assert_cmpint (reason, ==, expected);
}

static void
test_valid_plan_does_not_require_rebuild (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();

  assert_needs_rebuild_reason (&plan,
      WYREBOX_BACKUP_RESTORE_REBUILD_NOT_REQUIRED);
}

static void
test_missing_snapshot_requires_rebuild (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();

  plan.duckdb_snapshot_present = FALSE;
  g_assert_cmpint (wyrebox_backup_restore_plan_needs_rebuild (&plan, NULL),
      ==, WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_MISSING_DUCKDB_STATE);
}

static void
test_stale_snapshot_requires_rebuild (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();

  plan.duckdb_snapshot_journal_sequence = 6;
  g_assert_cmpint (wyrebox_backup_restore_plan_needs_rebuild (&plan, NULL),
      ==, WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_STALE_DUCKDB_STATE);
}

static void
test_incompatible_snapshot_requires_rebuild (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();

  g_clear_pointer (&plan.duckdb_snapshot_version, g_free);
  plan.duckdb_snapshot_version = g_strdup ("duckdb-2");
  g_assert_cmpint (wyrebox_backup_restore_plan_needs_rebuild (&plan, NULL),
      ==, WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE);
}

static void
test_restore_plan_validation_rejects_missing_snapshot_version (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();
  g_autoptr (GError) error = NULL;

  g_clear_pointer (&plan.expected_duckdb_snapshot_version, g_free);
  g_assert_false (wyrebox_backup_restore_plan_validate (&plan, &error));
  g_assert_nonnull (error);
}

static void
test_restore_plan_validation_rejects_offset_without_sequence (void)
{
  g_auto (WyreboxBackupRestorePlan) plan = make_valid_plan ();
  g_autoptr (GError) error = NULL;

  plan.expected_journal_sequence = 0;
  g_assert_false (wyrebox_backup_restore_plan_validate (&plan, &error));
  g_assert_nonnull (error);
}

static void
test_rebuild_reason_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_backup_restore_rebuild_reason_to_string
      (WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_MISSING_DUCKDB_STATE), ==,
      "missing-duckdb-state");
  g_assert_null (wyrebox_backup_restore_rebuild_reason_to_string (
          (WyreboxBackupRestoreRebuildReason) 999));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/backup/restore-plan/valid",
      test_valid_plan_does_not_require_rebuild);
  g_test_add_func ("/backup/restore-plan/missing-snapshot",
      test_missing_snapshot_requires_rebuild);
  g_test_add_func ("/backup/restore-plan/stale-snapshot",
      test_stale_snapshot_requires_rebuild);
  g_test_add_func ("/backup/restore-plan/incompatible-snapshot",
      test_incompatible_snapshot_requires_rebuild);
  g_test_add_func ("/backup/restore-plan/validation-missing-version",
      test_restore_plan_validation_rejects_missing_snapshot_version);
  g_test_add_func ("/backup/restore-plan/validation-offset-without-sequence",
      test_restore_plan_validation_rejects_offset_without_sequence);
  g_test_add_func ("/backup/restore-plan/rebuild-reason-names",
      test_rebuild_reason_names_are_stable);

  return g_test_run ();
}
