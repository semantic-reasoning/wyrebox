#pragma once

#include "wyrebox-daemon-memberships-copy-move-request.h"
#include "wyrebox-daemon-memberships-copy-move-service.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_memberships_copy_dispatch (
    WyreboxDaemonMembershipsCopyMoveService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

gboolean wyrebox_daemon_memberships_move_dispatch (
    WyreboxDaemonMembershipsCopyMoveService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
