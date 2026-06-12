#pragma once

#include "wyrebox-daemon-duckdb-query-template-request.h"
#include "wyrebox-daemon-duckdb-query-template-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_duckdb_query_template_dispatch (
    WyreboxDaemonDuckDBQueryTemplateService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
