#include "wyrebox-backup-restore-plan.h"

#include <gio/gio.h>
#include <string.h>

void
wyrebox_backup_restore_plan_clear (WyreboxBackupRestorePlan *plan)
{
  if (plan == NULL)
    return;

  g_clear_pointer (&plan->restore_id, g_free);
  g_clear_pointer (&plan->state_dir, g_free);
  g_clear_pointer (&plan->backup_root_dir, g_free);
  g_clear_pointer (&plan->object_store_identity, g_free);
  g_clear_pointer (&plan->journal_root_dir, g_free);
  g_clear_pointer (&plan->schema_version, g_free);
  g_clear_pointer (&plan->rule_package_version, g_free);
  g_clear_pointer (&plan->view_package_version, g_free);
  g_clear_pointer (&plan->expected_duckdb_snapshot_version, g_free);
  g_clear_pointer (&plan->duckdb_snapshot_version, g_free);
  memset (plan, 0, sizeof *plan);
}

const char *wyrebox_backup_restore_rebuild_reason_to_string
    (WyreboxBackupRestoreRebuildReason reason)
{
  switch (reason) {
    case WYREBOX_BACKUP_RESTORE_REBUILD_NOT_REQUIRED:
      return "not-required";
    case WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_MISSING_DUCKDB_STATE:
      return "missing-duckdb-state";
    case WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_STALE_DUCKDB_STATE:
      return "stale-duckdb-state";
    case WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE:
      return "incompatible-duckdb-state";
    default:
      return NULL;
  }
}

static gboolean
validate_required_string (const gchar *value, const gchar *field,
    GError **error)
{
  if (value != NULL && value[0] != '\0')
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_DATA, "restore plan requires a non-empty %s", field);
  return FALSE;
}

gboolean
wyrebox_backup_restore_plan_validate (const WyreboxBackupRestorePlan *plan,
    GError **error)
{
  g_return_val_if_fail (plan != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!validate_required_string (plan->restore_id, "restore ID", error) ||
      !validate_required_string (plan->state_dir, "state directory", error) ||
      !validate_required_string (plan->backup_root_dir,
          "backup root directory", error) ||
      !validate_required_string (plan->object_store_identity,
          "object store identity", error) ||
      !validate_required_string (plan->journal_root_dir,
          "journal root directory", error) ||
      !validate_required_string (plan->schema_version, "schema version",
          error) ||
      !validate_required_string (plan->rule_package_version,
          "rule package version", error) ||
      !validate_required_string (plan->view_package_version,
          "view package version", error) ||
      !validate_required_string (plan->expected_duckdb_snapshot_version,
          "expected DuckDB snapshot version", error))
    return FALSE;

  if (plan->expected_journal_offset > 0 && plan->expected_journal_sequence == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "restore plan requires a journal sequence when an offset is set");
    return FALSE;
  }

  return TRUE;
}

WyreboxBackupRestoreRebuildReason
wyrebox_backup_restore_plan_needs_rebuild (const WyreboxBackupRestorePlan
    *plan, GError **error)
{
  g_return_val_if_fail (plan != NULL,
      WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE);
  g_return_val_if_fail (error == NULL
      || *error == NULL,
      WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE);

  if (!wyrebox_backup_restore_plan_validate (plan, error))
    return WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE;

  if (!plan->duckdb_snapshot_present)
    return WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_MISSING_DUCKDB_STATE;

  if (g_strcmp0 (plan->duckdb_snapshot_version,
          plan->expected_duckdb_snapshot_version) != 0)
    return WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE;

  if (plan->duckdb_snapshot_journal_offset != plan->expected_journal_offset ||
      plan->duckdb_snapshot_journal_sequence != plan->expected_journal_sequence)
    return WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_STALE_DUCKDB_STATE;

  return WYREBOX_BACKUP_RESTORE_REBUILD_NOT_REQUIRED;
}
