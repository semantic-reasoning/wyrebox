#pragma once

#include "wyrebox-daemon-mailbox-select-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MAILBOX_SELECT_SERVICE \
  (wyrebox_daemon_mailbox_select_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMailboxSelectService,
    wyrebox_daemon_mailbox_select_service,
    WYREBOX,
    DAEMON_MAILBOX_SELECT_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonMailboxSelectServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data,
    GError **error);

WyreboxDaemonMailboxSelectService *wyrebox_daemon_mailbox_select_service_new (
    WyreboxDaemonMailboxSelectServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_mailbox_select_service_handle_identity (
    WyreboxDaemonMailboxSelectService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
