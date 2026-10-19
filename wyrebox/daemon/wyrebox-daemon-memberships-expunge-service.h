#pragma once

#include "wyrebox-daemon-memberships-expunge-request.h"
#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_MEMBERSHIPS_EXPUNGE_SERVICE \
    (wyrebox_daemon_memberships_expunge_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonMembershipsExpungeService,
    wyrebox_daemon_memberships_expunge_service,
    WYREBOX, DAEMON_MEMBERSHIPS_EXPUNGE_SERVICE, GObject)

typedef gboolean (*WyreboxDaemonMembershipsExpungeServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data,
    GError **error);

WyreboxDaemonMembershipsExpungeService *
wyrebox_daemon_memberships_expunge_service_new (
    WyreboxDaemonMembershipsExpungeServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

gboolean wyrebox_daemon_memberships_expunge_service_handle_identity (
    WyreboxDaemonMembershipsExpungeService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
