#include "wyrebox-admin-schema-version.h"

#include "wyrebox-schema-migration.h"

#include <gio/gio.h>
#include <glib.h>
#include <sys/wait.h>

static void
assert_cli (char **argv, int expected_status, const char *stdout_substring)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *stdout_text = NULL;
  g_autofree char *stderr_text = NULL;
  int wait_status = 0;

  g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
          &stdout_text, &stderr_text, &wait_status, &error));
  g_assert_no_error (error);
  g_assert_true (WIFEXITED (wait_status));
  g_assert_cmpint (WEXITSTATUS (wait_status), ==, expected_status);
  g_assert_nonnull (g_strstr_len (stdout_text, -1, stdout_substring));
}

static void
test_schema_version_probe_reports_current_version (void)
{
  g_auto (WyreboxAdminSchemaVersionResult) result = { 0 };

  g_assert_cmpint (wyrebox_admin_schema_version_probe (&result), ==, 0);
  g_assert_cmpuint (result.schema_version, ==,
      wyrebox_schema_migration_get_current_schema_version ());
}

static void
test_schema_version_cli_human_and_json_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  char *human_argv[] = { (char *) admin, (char *) "schema-version", NULL };
  char *json_argv[] = { (char *) admin, (char *) "schema-version",
    (char *) "--json", NULL
  };

  g_assert_nonnull (admin);
  assert_cli (human_argv, 0, "schema-version version=");
  assert_cli (json_argv, 0, "\"schema_version\":");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/admin/schema-version/probe-current-version",
      test_schema_version_probe_reports_current_version);
  g_test_add_func
      ("/admin/schema-version/cli-human-and-json-output",
      test_schema_version_cli_human_and_json_output);

  return g_test_run ();
}
