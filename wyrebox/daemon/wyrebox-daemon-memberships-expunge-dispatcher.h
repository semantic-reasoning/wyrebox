#pragma once

#include "wyrebox-daemon-memberships-expunge-request.h"
#include "wyrebox-daemon-memberships-expunge-service.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_memberships_expunge_dispatch (
    WyreboxDaemonMembershipsExpungeService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
