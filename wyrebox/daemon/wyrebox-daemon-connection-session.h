#pragma once

#include "wyrebox-daemon-peer-credentials.h"

#include <gio/gio.h>

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_CONNECTION_SESSION \
    (wyrebox_daemon_connection_session_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxDaemonConnectionSession,
    wyrebox_daemon_connection_session,
    WYREBOX,
    DAEMON_CONNECTION_SESSION,
    GObject)

typedef GBytes *(*WyreboxDaemonConnectionSessionPayloadHandler) (
    const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request,
    gpointer user_data,
    GError **error);

WyreboxDaemonConnectionSession *wyrebox_daemon_connection_session_new (
    GSocketConnection *connection,
    const WyreboxDaemonPeerCredentials *peer_credentials,
    WyreboxDaemonConnectionSessionPayloadHandler payload_handler,
    gpointer payload_handler_data,
    GDestroyNotify payload_handler_destroy_notify);

gboolean wyrebox_daemon_connection_session_process_payloads (
    WyreboxDaemonConnectionSession *self,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
