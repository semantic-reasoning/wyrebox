#pragma once

#include "wyrebox-daemon-wirelog-predicate-query-request.h"
#include "wyrebox-daemon-wirelog-predicate-query-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_wirelog_predicate_query_dispatch (
    WyreboxDaemonWirelogPredicateQueryService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
