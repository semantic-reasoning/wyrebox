#pragma once

#include "wyrebox-daemon-mailbox-list-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MAILBOX_LIST_SERVICE \
  (wyrebox_daemon_mailbox_list_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMailboxListService,
    wyrebox_daemon_mailbox_list_service,
    WYREBOX,
    DAEMON_MAILBOX_LIST_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonMailboxListServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result,
    gpointer user_data,
    GError **error);

WyreboxDaemonMailboxListService *wyrebox_daemon_mailbox_list_service_new (
    WyreboxDaemonMailboxListServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_mailbox_list_service_handle_identity (
    WyreboxDaemonMailboxListService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
