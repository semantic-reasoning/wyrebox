#include "wyrebox-daemon-peer-credentials.h"

#include <gio/gio.h>
#include <glib.h>

#include <sys/socket.h>
#include <unistd.h>

static GSocket *
socket_from_fd_for_test (int fd)
{
  g_autoptr (GError) error = NULL;
  GSocket *socket = NULL;

  socket = g_socket_new_from_fd (fd, &error);
  g_assert_no_error (error);
  g_assert_nonnull (socket);

  return socket;
}

static void
test_peer_credentials_reads_current_process_identity (void)
{
  int socket_fds[2] = { -1, -1 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocket) server_socket = NULL;
  g_autoptr (GSocket) client_socket = NULL;
  WyreboxDaemonPeerCredentials credentials = { 0 };

  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
          socket_fds), ==, 0);

  server_socket = socket_from_fd_for_test (socket_fds[0]);
  client_socket = socket_from_fd_for_test (socket_fds[1]);

  g_assert_true (wyrebox_daemon_peer_credentials_from_socket (server_socket,
          &credentials, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (credentials.uid, ==, getuid ());
  g_assert_cmpuint (credentials.gid, ==, getgid ());
  g_assert_cmpint (credentials.pid, ==, getpid ());

  g_assert_true (wyrebox_daemon_peer_credentials_from_socket (client_socket,
          &credentials, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (credentials.uid, ==, getuid ());
  g_assert_cmpuint (credentials.gid, ==, getgid ());
  g_assert_cmpint (credentials.pid, ==, getpid ());
}

static void
test_peer_credentials_clears_output_on_closed_socket_failure (void)
{
  int socket_fds[2] = { -1, -1 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocket) socket = NULL;
  g_autoptr (GSocket) peer_socket = NULL;
  WyreboxDaemonPeerCredentials credentials = {
    .uid = 123,
    .gid = 456,
    .pid = 789,
  };

  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
          socket_fds), ==, 0);

  socket = socket_from_fd_for_test (socket_fds[0]);
  peer_socket = socket_from_fd_for_test (socket_fds[1]);
  g_assert_true (g_socket_close (socket, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_peer_credentials_from_socket (socket,
          &credentials, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED);
  g_assert_cmpuint (credentials.uid, ==, 0);
  g_assert_cmpuint (credentials.gid, ==, 0);
  g_assert_cmpint (credentials.pid, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/peer-credentials/current-process-identity",
      test_peer_credentials_reads_current_process_identity);
  g_test_add_func ("/daemon-api/peer-credentials/closed-socket-clears-output",
      test_peer_credentials_clears_output_on_closed_socket_failure);

  return g_test_run ();
}
