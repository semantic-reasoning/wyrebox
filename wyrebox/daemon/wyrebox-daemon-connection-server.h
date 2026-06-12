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

gboolean wyrebox_daemon_connection_server_stop (
    WyreboxDaemonConnectionServer *self,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
