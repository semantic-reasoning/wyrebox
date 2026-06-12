#pragma once

#include "wyrebox-daemon-flag-keyword-update-request.h"
#include "wyrebox-daemon-flag-keyword-update-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_flag_keyword_update_dispatch (
    WyreboxDaemonFlagKeywordUpdateService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
