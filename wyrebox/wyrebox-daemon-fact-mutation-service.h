#pragma once

#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-daemon-response-frame.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_FACT_MUTATION_SERVICE \
  (wyrebox_daemon_fact_mutation_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonFactMutationService,
    wyrebox_daemon_fact_mutation_service,
    WYREBOX,
    DAEMON_FACT_MUTATION_SERVICE,
    GObject)

WyreboxDaemonFactMutationService *wyrebox_daemon_fact_mutation_service_new (
    WyreboxJournalWriter *journal_writer);

gboolean wyrebox_daemon_fact_mutation_service_handle (
    WyreboxDaemonFactMutationService *self,
    const char *request_id,
    const char *correlation_id,
    const WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonResponseFrame *out_frame,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
