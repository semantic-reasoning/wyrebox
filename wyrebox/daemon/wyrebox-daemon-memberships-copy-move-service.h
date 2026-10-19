#pragma once

#include "wyrebox-daemon-memberships-copy-move-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE \
    (wyrebox_daemon_memberships_copy_move_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMembershipsCopyMoveService,
    wyrebox_daemon_memberships_copy_move_service,
    WYREBOX, DAEMON_MEMBERSHIPS_COPY_MOVE_SERVICE, GObject)

typedef gboolean (*WyreboxDaemonMembershipsCopyMoveServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    gboolean is_move,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data,
    GError **error);

WyreboxDaemonMembershipsCopyMoveService *
wyrebox_daemon_memberships_copy_move_service_new (
    WyreboxDaemonMembershipsCopyMoveServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_memberships_copy_move_service_handle_identity (
    WyreboxDaemonMembershipsCopyMoveService *self,
    const WyreboxDaemonRequestIdentity *identity,
    gboolean is_move,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
