#pragma once

#include <gio/gio.h>

#include <sys/types.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  uid_t uid;
  gid_t gid;
  pid_t pid;
} WyreboxDaemonPeerCredentials;

/*
 * Reads Linux peer credentials from @socket.
 *
 * Ownership: caller owns @socket and provides @out_credentials. No heap
 * ownership is transferred. On failure, @out_credentials is left cleared.
 */
gboolean wyrebox_daemon_peer_credentials_from_socket (
    GSocket *socket,
    WyreboxDaemonPeerCredentials *out_credentials,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
