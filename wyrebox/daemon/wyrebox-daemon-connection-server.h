#pragma once

#include "wyrebox-daemon-request-adapter.h"

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_CONNECTION_SERVER \
    (wyrebox_daemon_connection_server_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxDaemonConnectionServer,
    wyrebox_daemon_connection_server,
    WYREBOX, DAEMON_CONNECTION_SERVER, GObject)

/*
 * Returns: (transfer full): caller-owned daemon connection server.
 * Free with g_object_unref().
 */
WyreboxDaemonConnectionServer *wyrebox_daemon_connection_server_new (
    const char *socket_path,
    WyreboxDaemonRequestAdapter *request_adapter);

gboolean wyrebox_daemon_connection_server_start (
    WyreboxDaemonConnectionServer *self,
    GError **error);

/*
 * Stops accepting connections, shuts down the open ones and waits up to the
 * drain timeout for their sessions to finish. When none is left running, no
 * session thread holds the request adapter afterwards, so dropping the last
 * references releases it on the calling thread. Sessions still running at the
 * timeout are logged and abandoned with their references; calling stop again
 * waits for them once more.
 *
 * Call it from the thread that runs the server's main context. Finalizing a
 * server without stopping it does not wait for sessions.
 *
 * Returns: FALSE with @error set only when the listener fails to stop.
 */
gboolean wyrebox_daemon_connection_server_stop (
    WyreboxDaemonConnectionServer *self,
    GError **error);

#define WYREBOX_DAEMON_CONNECTION_SERVER_DEFAULT_DRAIN_TIMEOUT_MS 30000

/*
 * Sets how long the next wyrebox_daemon_connection_server_stop() waits for
 * in-flight sessions. Defaults to
 * WYREBOX_DAEMON_CONNECTION_SERVER_DEFAULT_DRAIN_TIMEOUT_MS.
 */
void wyrebox_daemon_connection_server_set_drain_timeout (
    WyreboxDaemonConnectionServer *self,
    guint timeout_ms);

G_END_DECLS
/* *INDENT-ON* */
