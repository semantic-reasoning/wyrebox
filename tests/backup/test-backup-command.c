#include "wyrebox-backup-command.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_backup_command_valid_backup (void)
{
  g_auto (WyreboxBackupCommand) command = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_backup_command_init (&command,
          WYREBOX_BACKUP_COMMAND_BACKUP,
          "backup-20260619-0001",
          "/var/lib/wyrebox/state",
          "/var/lib/wyrebox/backup",
          "s3://bucket/wyrebox",
          "/var/lib/wyrebox/journal",
          "schema-7", "rules-3.2.1", "views-4.8.0", NULL, &error));
  g_assert_no_error (error);
}

static void
test_backup_command_valid_restore (void)
{
  g_auto (WyreboxBackupCommand) command = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_backup_command_init (&command,
          WYREBOX_BACKUP_COMMAND_RESTORE,
          "backup-20260619-0001",
          "/var/lib/wyrebox/state",
          "/var/lib/wyrebox/backup",
          "s3://bucket/wyrebox",
          "/var/lib/wyrebox/journal",
          "schema-7", "rules-3.2.1", "views-4.8.0", "duckdb-1", &error));
  g_assert_no_error (error);
}

static void
test_backup_command_rejects_invalid_kind (void)
{
  g_auto (WyreboxBackupCommand) command = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_backup_command_init (&command,
          (WyreboxBackupCommandKind) 99,
          "backup-20260619-0001",
          "/var/lib/wyrebox/state",
          "/var/lib/wyrebox/backup",
          "s3://bucket/wyrebox",
          "/var/lib/wyrebox/journal",
          "schema-7", "rules-3.2.1", "views-4.8.0", NULL, &error));
  g_assert_nonnull (error);
}

static void
test_backup_command_rejects_missing_fields (void)
{
  g_auto (WyreboxBackupCommand) command = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_backup_command_init (&command,
          WYREBOX_BACKUP_COMMAND_BACKUP,
          "",
          "/var/lib/wyrebox/state",
          "/var/lib/wyrebox/backup",
          "s3://bucket/wyrebox",
          "/var/lib/wyrebox/journal",
          "schema-7", "rules-3.2.1", "views-4.8.0", NULL, &error));
  g_assert_nonnull (error);
}

static void
test_backup_command_kind_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_backup_command_kind_to_string
      (WYREBOX_BACKUP_COMMAND_REBUILD), ==, "rebuild");
  g_assert_null (wyrebox_backup_command_kind_to_string (
          (WyreboxBackupCommandKind) - 1));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/backup/command/valid-backup",
      test_backup_command_valid_backup);
  g_test_add_func ("/backup/command/valid-restore",
      test_backup_command_valid_restore);
  g_test_add_func ("/backup/command/invalid-kind",
      test_backup_command_rejects_invalid_kind);
  g_test_add_func ("/backup/command/missing-fields",
      test_backup_command_rejects_missing_fields);
  g_test_add_func ("/backup/command/kind-names-stable",
      test_backup_command_kind_names_are_stable);

  return g_test_run ();
}
