#pragma once

#include <gio/gio.h>

#include "wyrebox-daemon-peer-credentials.h"

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
 * Called for each accepted Unix-domain socket connection.
 *
 * Ownership: @listener, @connection, and @credentials are borrowed and valid
 * only for the duration of the callback. The callback may keep @connection by
 * taking its own reference with g_object_ref(). Copy @credentials if they are
 * needed after the callback returns. @user_data is owned by the caller unless
 * a destroy notify is configured with
 * wyrebox_daemon_socket_listener_set_connection_handler().
 */
typedef void (*WyreboxDaemonSocketListenerConnectionHandler) (
    WyreboxDaemonSocketListener *listener,
    GSocketConnection *connection,
    const WyreboxDaemonPeerCredentials *credentials,
    gpointer user_data);

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

void wyrebox_daemon_socket_listener_set_connection_handler (
    WyreboxDaemonSocketListener *self,
    WyreboxDaemonSocketListenerConnectionHandler handler,
    gpointer user_data,
    GDestroyNotify destroy_notify);

gboolean wyrebox_daemon_socket_listener_start (
    WyreboxDaemonSocketListener *self,
    GError **error);

gboolean wyrebox_daemon_socket_listener_stop (
    WyreboxDaemonSocketListener *self,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
