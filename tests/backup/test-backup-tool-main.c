#include "wyrebox-backup-command.h"

#include <glib.h>

static void
test_command_kind_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_backup_command_kind_to_string
      (WYREBOX_BACKUP_COMMAND_BACKUP), ==, "backup");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/backup/tool/kind-names-stable",
      test_command_kind_names_are_stable);
  return g_test_run ();
}
