#pragma once

#include "wyrebox-daemon-message-search-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MESSAGE_SEARCH_SERVICE \
  (wyrebox_daemon_message_search_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMessageSearchService,
    wyrebox_daemon_message_search_service, WYREBOX,
    DAEMON_MESSAGE_SEARCH_SERVICE, GObject)

typedef gboolean (*WyreboxDaemonMessageSearchServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error);

WyreboxDaemonMessageSearchService *wyrebox_daemon_message_search_service_new (
    WyreboxDaemonMessageSearchServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_message_search_service_handle_identity (
    WyreboxDaemonMessageSearchService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
