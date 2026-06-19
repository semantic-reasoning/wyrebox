#pragma once

#include "wyrebox-backup-manifest.h"

#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_BACKUP_RESTORE_REBUILD_NOT_REQUIRED = 0,
  WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_MISSING_DUCKDB_STATE,
  WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_STALE_DUCKDB_STATE,
  WYREBOX_BACKUP_RESTORE_REBUILD_REQUIRED_INCOMPATIBLE_DUCKDB_STATE,
} WyreboxBackupRestoreRebuildReason;

typedef struct
{
  gchar *restore_id;
  gchar *state_dir;
  gchar *backup_root_dir;
  gchar *object_store_identity;
  gchar *journal_root_dir;
  gchar *schema_version;
  gchar *rule_package_version;
  gchar *view_package_version;
  gchar *expected_duckdb_snapshot_version;
  guint64 expected_journal_offset;
  guint64 expected_journal_sequence;
  gboolean duckdb_snapshot_present;
  gchar *duckdb_snapshot_version;
  guint64 duckdb_snapshot_journal_offset;
  guint64 duckdb_snapshot_journal_sequence;
} WyreboxBackupRestorePlan;

void wyrebox_backup_restore_plan_clear (WyreboxBackupRestorePlan *plan);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBackupRestorePlan,
    wyrebox_backup_restore_plan_clear)

gboolean wyrebox_backup_restore_plan_validate (const WyreboxBackupRestorePlan
    *plan, GError **error);

WyreboxBackupRestoreRebuildReason
wyrebox_backup_restore_plan_needs_rebuild (const WyreboxBackupRestorePlan
    *plan, GError **error);

const char *wyrebox_backup_restore_rebuild_reason_to_string (
    WyreboxBackupRestoreRebuildReason reason);

G_END_DECLS
/* *INDENT-ON* */
