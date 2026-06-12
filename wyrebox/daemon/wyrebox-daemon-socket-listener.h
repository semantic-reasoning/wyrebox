#pragma once

#include <gio/gio.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_SOCKET_LISTENER \
    (wyrebox_daemon_socket_listener_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxDaemonSocketListener,
    wyrebox_daemon_socket_listener,
    WYREBOX,
    DAEMON_SOCKET_LISTENER,
    GObject)

/*
 * Returns: (transfer full): caller-owned daemon socket listener.
 * Free with g_object_unref().
 */
WyreboxDaemonSocketListener *wyrebox_daemon_socket_listener_new (
    const char *socket_path);

const char *wyrebox_daemon_socket_listener_get_socket_path (
    WyreboxDaemonSocketListener *self);

gboolean wyrebox_daemon_socket_listener_is_started (
    WyreboxDaemonSocketListener *self);

gboolean wyrebox_daemon_socket_listener_start (
    WyreboxDaemonSocketListener *self,
    GError **error);

gboolean wyrebox_daemon_socket_listener_stop (
    WyreboxDaemonSocketListener *self,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
