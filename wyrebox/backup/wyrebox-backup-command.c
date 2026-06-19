#include "wyrebox-backup-command.h"

#include <gio/gio.h>

static gboolean
validate_required_text (const char *value, const char *field_name,
    GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "backup command %s is required", field_name);
    return FALSE;
  }

  return TRUE;
}

void
wyrebox_backup_command_clear (WyreboxBackupCommand *command)
{
  if (command == NULL)
    return;

  g_clear_pointer (&command->backup_id, g_free);
  g_clear_pointer (&command->state_dir, g_free);
  g_clear_pointer (&command->backup_root_dir, g_free);
  g_clear_pointer (&command->object_store_identity, g_free);
  g_clear_pointer (&command->journal_root_dir, g_free);
  g_clear_pointer (&command->schema_version, g_free);
  g_clear_pointer (&command->rule_package_version, g_free);
  g_clear_pointer (&command->view_package_version, g_free);
  g_clear_pointer (&command->duckdb_snapshot_version, g_free);
}

const char *
wyrebox_backup_command_kind_to_string (WyreboxBackupCommandKind kind)
{
  switch (kind) {
    case WYREBOX_BACKUP_COMMAND_BACKUP:
      return "backup";
    case WYREBOX_BACKUP_COMMAND_RESTORE:
      return "restore";
    case WYREBOX_BACKUP_COMMAND_REBUILD:
      return "rebuild";
    default:
      return NULL;
  }
}

gboolean
wyrebox_backup_command_init (WyreboxBackupCommand *command,
    WyreboxBackupCommandKind kind, const char *backup_id,
    const char *state_dir, const char *backup_root_dir,
    const char *object_store_identity, const char *journal_root_dir,
    const char *schema_version, const char *rule_package_version,
    const char *view_package_version, const char *duckdb_snapshot_version,
    GError **error)
{
  g_auto (WyreboxBackupCommand) next = { 0 };

  g_return_val_if_fail (command != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (wyrebox_backup_command_kind_to_string (kind) == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "backup command kind is invalid");
    return FALSE;
  }

  if (!validate_required_text (backup_id, "backup_id", error) ||
      !validate_required_text (state_dir, "state_dir", error) ||
      !validate_required_text (backup_root_dir, "backup_root_dir", error) ||
      !validate_required_text (object_store_identity,
          "object_store_identity", error) ||
      !validate_required_text (journal_root_dir, "journal_root_dir", error) ||
      !validate_required_text (schema_version, "schema_version", error) ||
      !validate_required_text (rule_package_version,
          "rule_package_version", error) ||
      !validate_required_text (view_package_version, "view_package_version",
          error))
    return FALSE;

  if (kind == WYREBOX_BACKUP_COMMAND_BACKUP &&
      duckdb_snapshot_version != NULL && *duckdb_snapshot_version != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "backup command must not name a DuckDB snapshot version");
    return FALSE;
  }

  next.kind = kind;
  next.backup_id = g_strdup (backup_id);
  next.state_dir = g_strdup (state_dir);
  next.backup_root_dir = g_strdup (backup_root_dir);
  next.object_store_identity = g_strdup (object_store_identity);
  next.journal_root_dir = g_strdup (journal_root_dir);
  next.schema_version = g_strdup (schema_version);
  next.rule_package_version = g_strdup (rule_package_version);
  next.view_package_version = g_strdup (view_package_version);
  next.duckdb_snapshot_version = g_strdup (duckdb_snapshot_version);

  wyrebox_backup_command_clear (command);
  *command = next;
  next.backup_id = NULL;
  next.state_dir = NULL;
  next.backup_root_dir = NULL;
  next.object_store_identity = NULL;
  next.journal_root_dir = NULL;
  next.schema_version = NULL;
  next.rule_package_version = NULL;
  next.view_package_version = NULL;
  next.duckdb_snapshot_version = NULL;

  return TRUE;
}
