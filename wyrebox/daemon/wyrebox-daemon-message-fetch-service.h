#pragma once

#include "wyrebox-daemon-message-fetch-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MESSAGE_FETCH_SERVICE \
  (wyrebox_daemon_message_fetch_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMessageFetchService,
    wyrebox_daemon_message_fetch_service,
    WYREBOX,
    DAEMON_MESSAGE_FETCH_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonMessageFetchServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error);

WyreboxDaemonMessageFetchService *wyrebox_daemon_message_fetch_service_new (
    WyreboxDaemonMessageFetchServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_message_fetch_service_handle_identity (
    WyreboxDaemonMessageFetchService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
