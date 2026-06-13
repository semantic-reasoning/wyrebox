#pragma once

#include "wyrebox-daemon-fact-mutation-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_fact_mutation_dispatch (
    WyreboxDaemonFactMutationService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

gboolean wyrebox_daemon_fact_batch_import_dispatch (
    WyreboxDaemonFactMutationService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonFactBatchImportRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
