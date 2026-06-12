#define _GNU_SOURCE

#include "wyrebox-daemon-peer-credentials.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

gboolean
wyrebox_daemon_peer_credentials_from_socket (GSocket *socket,
    WyreboxDaemonPeerCredentials *out_credentials, GError **error)
{
  struct ucred credentials = { 0 };
  socklen_t credentials_size = sizeof (credentials);
  int socket_fd = -1;

  if (out_credentials != NULL)
    memset (out_credentials, 0, sizeof (*out_credentials));

  if (socket == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "cannot read peer credentials from a NULL socket");
    return FALSE;
  }

  if (out_credentials == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "cannot read peer credentials without an output struct");
    return FALSE;
  }

  socket_fd = g_socket_get_fd (socket);
  if (socket_fd < 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_CLOSED, "cannot read peer credentials from a closed socket");
    return FALSE;
  }

  if (getsockopt (socket_fd,
          SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0) {
    int saved_errno = errno;

    g_set_error (error,
        G_IO_ERROR,
        g_io_error_from_errno (saved_errno),
        "failed to read Linux peer credentials: %s", g_strerror (saved_errno));
    return FALSE;
  }

  if (credentials_size != sizeof (credentials)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Linux peer credentials response had unexpected size");
    return FALSE;
  }

  out_credentials->uid = credentials.uid;
  out_credentials->gid = credentials.gid;
  out_credentials->pid = credentials.pid;
  return TRUE;
}
