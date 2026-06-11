#include "wyrebox-daemon-runtime.h"

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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/runtime/default-socket-path",
      test_default_socket_path_matches_linux_runtime_contract);
  g_test_add_func ("/daemon-api/runtime/default-runtime-dir",
      test_default_socket_dir_is_run_wyrebox);

  return g_test_run ();
}
