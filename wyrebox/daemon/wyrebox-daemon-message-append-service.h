#pragma once

#include "wyrebox-daemon-message-append-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-success-receipt.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MESSAGE_APPEND_SERVICE \
    (wyrebox_daemon_message_append_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMessageAppendService,
    wyrebox_daemon_message_append_service,
    WYREBOX, DAEMON_MESSAGE_APPEND_SERVICE, GObject)

typedef gboolean (*WyreboxDaemonMessageAppendServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageAppendRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data,
    GError **error);

WyreboxDaemonMessageAppendService *
wyrebox_daemon_message_append_service_new (
    WyreboxDaemonMessageAppendServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_message_append_service_handle_identity (
    WyreboxDaemonMessageAppendService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageAppendRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
