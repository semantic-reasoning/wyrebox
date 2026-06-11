#include "wyrebox-daemon-runtime.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_default_socket_path_matches_linux_runtime_contract (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_socket_path (),
      ==, "/run/wyrebox/wyrebox.sock");
}

static void
test_default_socket_dir_is_run_wyrebox (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_runtime_dir (),
      ==, "/run/wyrebox");
}

static void
test_default_fact_dump_dir_is_under_run_wyrebox (void)
{
  g_assert_cmpstr (wyrebox_daemon_runtime_get_default_fact_dump_dir (),
      ==, "/run/wyrebox/facts");
}

static void
test_default_fact_dump_file_is_owned_gfile (void)
{
  g_autoptr (GFile) first = NULL;
  g_autoptr (GFile) second = NULL;
  g_autofree char *path = NULL;

  first = wyrebox_daemon_runtime_get_default_fact_dump_file ();
  second = wyrebox_daemon_runtime_get_default_fact_dump_file ();

  g_assert_nonnull (first);
  g_assert_nonnull (second);
  g_assert_true (first != second);

  path = g_file_get_path (first);
  g_assert_cmpstr (path, ==, "/run/wyrebox/facts");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/runtime/default-socket-path",
      test_default_socket_path_matches_linux_runtime_contract);
  g_test_add_func ("/daemon-api/runtime/default-runtime-dir",
      test_default_socket_dir_is_run_wyrebox);
  g_test_add_func ("/daemon-api/runtime/default-fact-dump-dir",
      test_default_fact_dump_dir_is_under_run_wyrebox);
  g_test_add_func ("/daemon-api/runtime/default-fact-dump-file-owned",
      test_default_fact_dump_file_is_owned_gfile);

  return g_test_run ();
}
