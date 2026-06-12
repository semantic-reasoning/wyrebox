#include "wyrebox-daemon-runtime.h"
#include "wyrebox-daemon-socket-listener.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct
{
  GMainLoop *loop;
  guint expected_count;
  guint count;
  WyreboxDaemonPeerCredentials last_credentials;
} CaptureHandlerState;

typedef struct
{
  GMainLoop *loop;
  gboolean timed_out;
} LoopTimeout;

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
make_socket_path (char **out_root)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-daemon-socket-listener-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "run", "wyrebox.sock", NULL);
}

static GSocketConnection *
connect_with_socket_client (const char *socket_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketConnection) connection = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client,
      G_SOCKET_CONNECTABLE (address), NULL, &error);

  g_assert_no_error (error);
  g_assert_nonnull (connection);

  return g_steal_pointer (&connection);
}

static void
assert_connects_with_socket_client (const char *socket_path)
{
  g_autoptr (GSocketConnection) connection = NULL;

  connection = connect_with_socket_client (socket_path);
}

static void
assert_socket_missing (const char *socket_path)
{
  GStatBuf stat_buf = { 0 };

  g_assert_cmpint (g_lstat (socket_path, &stat_buf), ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);
}

static GStatBuf
stat_existing_path (const char *path)
{
  GStatBuf stat_buf = { 0 };

  g_assert_cmpint (g_lstat (path, &stat_buf), ==, 0);
  return stat_buf;
}

static void
create_stale_socket_path (const char *socket_path)
{
  g_autofree char *parent_dir = g_path_get_dirname (socket_path);
  struct sockaddr_un address = { 0 };
  int socket_fd = -1;

  g_assert_cmpint (g_mkdir_with_parents (parent_dir, 0750), ==, 0);
  g_assert_cmpuint (strlen (socket_path), <, sizeof (address.sun_path));

  socket_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (socket_fd, >=, 0);

  address.sun_family = AF_UNIX;
  g_strlcpy (address.sun_path, socket_path, sizeof (address.sun_path));
  g_assert_cmpint (bind (socket_fd,
          (const struct sockaddr *) &address, sizeof (address)), ==, 0);
  g_assert_cmpint (listen (socket_fd, 1), ==, 0);
  g_assert_cmpint (close (socket_fd), ==, 0);
}

static void
capture_and_close_connection (WyreboxDaemonSocketListener *listener,
    GSocketConnection *connection,
    const WyreboxDaemonPeerCredentials *credentials, gpointer user_data)
{
  CaptureHandlerState *state = user_data;
  g_autoptr (GError) error = NULL;

  g_assert_true (WYREBOX_IS_DAEMON_SOCKET_LISTENER (listener));
  g_assert_nonnull (connection);
  g_assert_nonnull (credentials);

  state->count++;
  state->last_credentials = *credentials;

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
  g_assert_no_error (error);

  if (state->count >= state->expected_count)
    g_main_loop_quit (state->loop);
}

static gboolean
quit_loop_on_timeout (gpointer user_data)
{
  LoopTimeout *timeout = user_data;

  timeout->timed_out = TRUE;
  g_main_loop_quit (timeout->loop);
  return G_SOURCE_REMOVE;
}

static void
run_loop_until_handler_count (CaptureHandlerState *state)
{
  LoopTimeout timeout = {
    .loop = state->loop,
  };
  guint timeout_id = 0;

  timeout_id = g_timeout_add (2000, quit_loop_on_timeout, &timeout);
  g_main_loop_run (state->loop);

  if (!timeout.timed_out)
    g_source_remove (timeout_id);

  g_assert_false (timeout.timed_out);
  g_assert_cmpuint (state->count, ==, state->expected_count);
}

static void
test_socket_listener_starts_and_accepts_client_connect (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_false (wyrebox_daemon_socket_listener_is_started (listener));
  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_socket_listener_is_started (listener));
  assert_connects_with_socket_client (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_handler_receives_peer_credentials (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GMainLoop) loop = NULL;
  g_autoptr (GSocketConnection) client = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;
  CaptureHandlerState state = { 0 };

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  loop = g_main_loop_new (NULL, FALSE);
  state.loop = loop;
  state.expected_count = 1;

  wyrebox_daemon_socket_listener_set_connection_handler (listener,
      capture_and_close_connection, &state, NULL);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);

  client = connect_with_socket_client (socket_path);
  run_loop_until_handler_count (&state);

  g_assert_cmpuint (state.last_credentials.uid, ==, getuid ());
  g_assert_cmpuint (state.last_credentials.gid, ==, getgid ());
  g_assert_cmpint (state.last_credentials.pid, ==, getpid ());

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_handler_receives_two_sequential_clients (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (GMainLoop) loop = NULL;
  g_autoptr (GSocketConnection) first_client = NULL;
  g_autoptr (GSocketConnection) second_client = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;
  CaptureHandlerState state = { 0 };

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  loop = g_main_loop_new (NULL, FALSE);
  state.loop = loop;
  state.expected_count = 2;

  wyrebox_daemon_socket_listener_set_connection_handler (listener,
      capture_and_close_connection, &state, NULL);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);

  first_client = connect_with_socket_client (socket_path);
  second_client = connect_with_socket_client (socket_path);
  run_loop_until_handler_count (&state);

  g_assert_cmpuint (state.count, ==, 2);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_stop_cancels_pending_accept (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  g_assert_false (wyrebox_daemon_socket_listener_is_started (listener));

  while (g_main_context_pending (NULL))
    g_main_context_iteration (NULL, FALSE);

  remove_tree (root);
}

static void
test_socket_listener_creates_parent_dir_mode_0750 (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autofree char *parent_dir = g_path_get_dirname (socket_path);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;
  GStatBuf stat_buf = { 0 };
  mode_t previous_umask = 0;
  gboolean started = FALSE;

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  previous_umask = umask (0);
  started = wyrebox_daemon_socket_listener_start (listener, &error);
  umask (previous_umask);

  g_assert_true (started);
  g_assert_no_error (error);
  g_assert_cmpint (g_lstat (parent_dir, &stat_buf), ==, 0);
  g_assert_cmpuint (stat_buf.st_mode & 0777, ==, 0750);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_sets_socket_mode_0660 (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;
  GStatBuf stat_buf = { 0 };

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_lstat (socket_path, &stat_buf), ==, 0);
  g_assert_cmpuint (stat_buf.st_mode & 0777, ==, 0660);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_recovers_stale_socket_path (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  create_stale_socket_path (socket_path);

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);
  assert_connects_with_socket_client (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_rejects_duplicate_start (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);
  g_assert_false (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_rejects_second_live_listener_on_path (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) first = NULL;
  g_autoptr (WyreboxDaemonSocketListener) second = NULL;
  GStatBuf before_conflict = { 0 };
  GStatBuf after_conflict = { 0 };

  first = wyrebox_daemon_socket_listener_new (socket_path);
  second = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_start (first, &error));
  g_assert_no_error (error);
  before_conflict = stat_existing_path (socket_path);

  g_assert_false (wyrebox_daemon_socket_listener_start (second, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE);
  g_clear_error (&error);

  after_conflict = stat_existing_path (socket_path);
  g_assert_cmpuint (after_conflict.st_dev, ==, before_conflict.st_dev);
  g_assert_cmpuint (after_conflict.st_ino, ==, before_conflict.st_ino);
  assert_connects_with_socket_client (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_stop (first, &error));
  g_assert_no_error (error);
  remove_tree (root);
}

static void
test_socket_listener_stop_cleans_up_created_socket (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);

  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);
  g_assert_cmpint (g_file_test (socket_path, G_FILE_TEST_EXISTS), ==, TRUE);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  g_assert_false (wyrebox_daemon_socket_listener_is_started (listener));
  assert_socket_missing (socket_path);

  remove_tree (root);
}

static void
test_socket_listener_finalize_cleans_up_created_socket (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);

  g_clear_object (&listener);
  assert_socket_missing (socket_path);

  remove_tree (root);
}

static void
test_socket_listener_stop_does_not_unlink_replacement_path (void)
{
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  g_assert_true (wyrebox_daemon_socket_listener_start (listener, &error));
  g_assert_no_error (error);

  g_assert_cmpint (g_unlink (socket_path), ==, 0);
  g_assert_true (g_file_set_contents (socket_path, "replacement", -1, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_socket_listener_stop (listener, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (socket_path, G_FILE_TEST_IS_REGULAR));

  remove_tree (root);
}

static void
test_socket_listener_api_exposes_path_without_tcp_properties (void)
{
  const char *socket_path = wyrebox_daemon_runtime_get_default_socket_path ();
  g_autoptr (WyreboxDaemonSocketListener) listener = NULL;
  GObjectClass *object_class = NULL;

  listener = wyrebox_daemon_socket_listener_new (socket_path);
  object_class = G_OBJECT_GET_CLASS (listener);

  g_assert_cmpstr (wyrebox_daemon_socket_listener_get_socket_path (listener),
      ==, socket_path);
  g_assert_null (g_object_class_find_property (object_class, "host"));
  g_assert_null (g_object_class_find_property (object_class, "port"));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/socket-listener/start-connect",
      test_socket_listener_starts_and_accepts_client_connect);
  g_test_add_func ("/daemon-api/socket-listener/handler-peer-credentials",
      test_socket_listener_handler_receives_peer_credentials);
  g_test_add_func ("/daemon-api/socket-listener/two-sequential-clients",
      test_socket_listener_handler_receives_two_sequential_clients);
  g_test_add_func ("/daemon-api/socket-listener/stop-cancels-pending-accept",
      test_socket_listener_stop_cancels_pending_accept);
  g_test_add_func ("/daemon-api/socket-listener/parent-dir-mode-0750",
      test_socket_listener_creates_parent_dir_mode_0750);
  g_test_add_func ("/daemon-api/socket-listener/mode-0660",
      test_socket_listener_sets_socket_mode_0660);
  g_test_add_func ("/daemon-api/socket-listener/stale-socket-recovery",
      test_socket_listener_recovers_stale_socket_path);
  g_test_add_func ("/daemon-api/socket-listener/duplicate-start",
      test_socket_listener_rejects_duplicate_start);
  g_test_add_func ("/daemon-api/socket-listener/second-live-path-conflict",
      test_socket_listener_rejects_second_live_listener_on_path);
  g_test_add_func ("/daemon-api/socket-listener/stop-cleanup",
      test_socket_listener_stop_cleans_up_created_socket);
  g_test_add_func ("/daemon-api/socket-listener/finalize-cleanup",
      test_socket_listener_finalize_cleans_up_created_socket);
  g_test_add_func ("/daemon-api/socket-listener/keep-replacement-path",
      test_socket_listener_stop_does_not_unlink_replacement_path);
  g_test_add_func ("/daemon-api/socket-listener/path-api-only",
      test_socket_listener_api_exposes_path_without_tcp_properties);

  return g_test_run ();
}
