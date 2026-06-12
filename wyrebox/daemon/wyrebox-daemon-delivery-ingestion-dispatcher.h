#pragma once

#include "wyrebox-daemon-delivery-ingestion-service.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

gboolean wyrebox_daemon_delivery_ingestion_dispatch (
    WyreboxDaemonDeliveryIngestionService *service,
    const char *request_id,
    const char *caller_identity,
    const char *account_identity,
    const char *tool_identity,
    const char *correlation_id,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
