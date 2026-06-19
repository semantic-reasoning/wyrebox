#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef enum
{
  WYREBOX_BACKUP_COMMAND_BACKUP,
  WYREBOX_BACKUP_COMMAND_RESTORE,
  WYREBOX_BACKUP_COMMAND_REBUILD,
} WyreboxBackupCommandKind;

typedef struct
{
  WyreboxBackupCommandKind kind;
  char *backup_id;
  char *state_dir;
  char *backup_root_dir;
  char *object_store_identity;
  char *journal_root_dir;
  char *schema_version;
  char *rule_package_version;
  char *view_package_version;
  char *duckdb_snapshot_version;
} WyreboxBackupCommand;

void wyrebox_backup_command_clear (WyreboxBackupCommand *command);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxBackupCommand,
    wyrebox_backup_command_clear)

gboolean wyrebox_backup_command_init (WyreboxBackupCommand *command,
    WyreboxBackupCommandKind kind, const char *backup_id,
    const char *state_dir, const char *backup_root_dir,
    const char *object_store_identity, const char *journal_root_dir,
    const char *schema_version, const char *rule_package_version,
    const char *view_package_version, const char *duckdb_snapshot_version,
    GError **error);

const char *wyrebox_backup_command_kind_to_string (
    WyreboxBackupCommandKind kind);

G_END_DECLS
/* *INDENT-ON* */
