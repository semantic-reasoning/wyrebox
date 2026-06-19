#include "wyrebox-admin-socket-status.h"
#include "wyrebox-daemon-runtime.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static char *
create_tmp_dir (void)
{
  g_autoptr (GError) error = NULL;
  char *dir = NULL;

  dir = g_dir_make_tmp ("wyrebox-admin-socket-status-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  return dir;
}

static int
create_listening_socket (const char *path)
{
  struct sockaddr_un addr = { 0 };
  int fd = -1;

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >=, 0);

  addr.sun_family = AF_UNIX;
  g_assert_cmpuint (strlen (path), <, sizeof (addr.sun_path));
  g_strlcpy (addr.sun_path, path, sizeof (addr.sun_path));

  g_assert_cmpint (bind (fd, (struct sockaddr *) &addr, sizeof (addr)), ==, 0);
  g_assert_cmpint (listen (fd, 1), ==, 0);

  return fd;
}

static void
assert_cli (char **argv,
    int expected_status, const char *stdout_substring,
    const char *stderr_substring)
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

  if (stdout_substring != NULL)
    g_assert_nonnull (g_strstr_len (stdout_text, -1, stdout_substring));
  if (stderr_substring != NULL)
    g_assert_nonnull (g_strstr_len (stderr_text, -1, stderr_substring));
}

static void
test_default_socket_path_uses_daemon_runtime_constant (void)
{
  g_assert_cmpstr (wyrebox_admin_socket_status_default_socket_path (), ==,
      wyrebox_daemon_runtime_get_default_socket_path ());
}

static void
test_missing_path (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "missing.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_MISSING_PATH);
  g_assert_cmpstr (result.socket_path, ==, path);
  g_assert_cmpstr (result.status_name, ==, "missing-path");

  remove_tree (dir);
}

static void
test_regular_file_is_not_socket (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "regular", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (g_file_set_contents (path, "not a socket\n", -1, &error));
  g_assert_no_error (error);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_NOT_SOCKET);
  g_assert_cmpstr (result.status_name, ==, "not-socket");

  remove_tree (dir);
}

static void
test_live_socket_is_connectable (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "wyrebox.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  int fd = -1;

  fd = create_listening_socket (path);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_SUCCESS);
  g_assert_cmpstr (result.status_name, ==, "ok");
  g_assert_true (result.connectable);

  close (fd);
  remove_tree (dir);
}

static void
test_stale_socket_is_not_connectable (void)
{
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "stale.sock", NULL);
  g_auto (WyreboxAdminSocketStatusResult) result = { 0 };
  int fd = -1;

  fd = create_listening_socket (path);
  close (fd);

  g_assert_cmpint (wyrebox_admin_socket_status_probe (path, &result), ==,
      WYREBOX_ADMIN_SOCKET_STATUS_EXIT_CONNECT_FAILED);
  g_assert_cmpstr (result.status_name, ==, "connect-failed");
  g_assert_false (result.connectable);

  remove_tree (dir);
}

static void
test_cli_human_and_json_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  g_autofree char *dir = create_tmp_dir ();
  g_autofree char *path = g_build_filename (dir, "wyrebox.sock", NULL);
  int fd = -1;
  char *human_argv[] = { (char *) admin, (char *) "socket-status",
    (char *) "--socket-path", path, NULL
  };
  char *json_argv[] = { (char *) admin, (char *) "socket-status",
    (char *) "--socket-path", path, (char *) "--json", NULL
  };

  g_assert_nonnull (admin);
  fd = create_listening_socket (path);

  assert_cli (human_argv, 0, "status: ok", NULL);
  assert_cli (json_argv, 0, "\"status\":\"ok\"", NULL);

  close (fd);
  remove_tree (dir);
}

static void
test_cli_usage_error_output (void)
{
  const char *admin = g_getenv ("WYREBOX_ADMIN_EXECUTABLE");
  char *argv[] = { (char *) admin, (char *) "unknown-command", NULL };

  g_assert_nonnull (admin);
  assert_cli (argv, WYREBOX_ADMIN_SOCKET_STATUS_EXIT_USAGE_ERROR, NULL,
      "Usage:");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/admin/socket-status/default-socket-path",
      test_default_socket_path_uses_daemon_runtime_constant);
  g_test_add_func ("/admin/socket-status/missing-path", test_missing_path);
  g_test_add_func ("/admin/socket-status/not-socket",
      test_regular_file_is_not_socket);
  g_test_add_func ("/admin/socket-status/connectable",
      test_live_socket_is_connectable);
  g_test_add_func ("/admin/socket-status/stale-socket",
      test_stale_socket_is_not_connectable);
  g_test_add_func ("/admin/socket-status/cli-output",
      test_cli_human_and_json_output);
  g_test_add_func ("/admin/socket-status/cli-usage",
      test_cli_usage_error_output);

  return g_test_run ();
}
