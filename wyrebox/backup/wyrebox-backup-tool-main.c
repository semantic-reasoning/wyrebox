#include "wyrebox-backup-command.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sysexits.h>

static gboolean
parse_command_kind (const char *value, WyreboxBackupCommandKind *out_kind)
{
  if (g_strcmp0 (value, "backup") == 0) {
    *out_kind = WYREBOX_BACKUP_COMMAND_BACKUP;
    return TRUE;
  }

  if (g_strcmp0 (value, "restore") == 0) {
    *out_kind = WYREBOX_BACKUP_COMMAND_RESTORE;
    return TRUE;
  }

  if (g_strcmp0 (value, "rebuild") == 0) {
    *out_kind = WYREBOX_BACKUP_COMMAND_REBUILD;
    return TRUE;
  }

  return FALSE;
}

static int
print_usage (void)
{
  g_printerr ("Usage: wyrebox-backup-tool [backup|restore|rebuild] "
      "--backup-id ID --state-dir DIR --backup-root-dir DIR "
      "--object-store-identity ID --journal-root-dir DIR "
      "--schema-version VERSION --rule-package-version VERSION "
      "--view-package-version VERSION [--duckdb-snapshot-version VERSION]\n");
  return EX_USAGE;
}

static gboolean
parse_options (int argc, char **argv, WyreboxBackupCommand *command,
    GError **error)
{
  g_auto (GStrv) mutable_argv = NULL;
  g_autoptr (GOptionContext) context = NULL;
  g_autofree char *kind_text = NULL;
  WyreboxBackupCommandKind kind = WYREBOX_BACKUP_COMMAND_BACKUP;
  GOptionEntry entries[] = {
    {"backup-id", 0, 0, G_OPTION_ARG_STRING, &command->backup_id,
        "Backup identifier", "ID"},
    {"state-dir", 0, 0, G_OPTION_ARG_STRING, &command->state_dir,
        "WyreBox state directory", "DIR"},
    {"backup-root-dir", 0, 0, G_OPTION_ARG_STRING, &command->backup_root_dir,
        "Backup root directory", "DIR"},
    {"object-store-identity", 0, 0, G_OPTION_ARG_STRING,
        &command->object_store_identity, "Object store identity", "ID"},
    {"journal-root-dir", 0, 0, G_OPTION_ARG_STRING, &command->journal_root_dir,
        "Journal root directory", "DIR"},
    {"schema-version", 0, 0, G_OPTION_ARG_STRING, &command->schema_version,
        "Schema version", "VERSION"},
    {"rule-package-version", 0, 0, G_OPTION_ARG_STRING,
        &command->rule_package_version, "Rule package version", "VERSION"},
    {"view-package-version", 0, 0, G_OPTION_ARG_STRING,
        &command->view_package_version, "View package version", "VERSION"},
    {"duckdb-snapshot-version", 0, 0, G_OPTION_ARG_STRING,
          &command->duckdb_snapshot_version, "DuckDB snapshot version",
        "VERSION"},
    {NULL}
  };

  if (argc < 2)
    return FALSE;

  if (!parse_command_kind (argv[1], &kind)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "unknown backup command: %s", argv[1]);
    return FALSE;
  }

  command->kind = kind;
  mutable_argv = g_new0 (gchar *, argc);
  for (int i = 0; i < argc; i++)
    mutable_argv[i] = g_strdup (argv[i]);

  context = g_option_context_new (NULL);
  g_option_context_set_help_enabled (context, FALSE);
  g_option_context_add_main_entries (context, entries, NULL);

  if (!g_option_context_parse_strv (context, &mutable_argv, error))
    return FALSE;

  if (mutable_argv[1] != NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "unexpected positional argument: %s", mutable_argv[1]);
    return FALSE;
  }

  kind_text = g_strdup ((const char *) argv[1]);
  if (!wyrebox_backup_command_init (command, kind, command->backup_id,
          command->state_dir, command->backup_root_dir,
          command->object_store_identity, command->journal_root_dir,
          command->schema_version, command->rule_package_version,
          command->view_package_version, command->duckdb_snapshot_version,
          error))
    return FALSE;

  g_free (kind_text);
  return TRUE;
}

int
main (int argc, char **argv)
{
  g_auto (WyreboxBackupCommand) command = { 0 };
  g_autoptr (GError) error = NULL;

  if (!parse_options (argc, argv, &command, &error)) {
    g_printerr ("%s\n", error != NULL ? error->message : "invalid arguments");
    return print_usage ();
  }

  g_print ("backup-command kind=%s backup_id=%s state_dir=%s\n",
      wyrebox_backup_command_kind_to_string (command.kind),
      command.backup_id, command.state_dir);
  return EX_OK;
}
