#pragma once

#include "wyrebox-daemon-fact-mutation-request.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_FACT_BATCH_IMPORT_REQUEST_MAX_ENTRIES 1024

typedef struct
{
  GPtrArray *entries;
  char *scope_id;
} WyreboxDaemonFactBatchImportRequest;

void wyrebox_daemon_fact_batch_import_request_clear (
    WyreboxDaemonFactBatchImportRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonFactBatchImportRequest,
    wyrebox_daemon_fact_batch_import_request_clear)

gboolean wyrebox_daemon_fact_batch_import_request_init (
    WyreboxDaemonFactBatchImportRequest *request,
    const WyreboxDaemonFactMutationRequest * const *entries,
    guint n_entries,
    GError **error);

guint wyrebox_daemon_fact_batch_import_request_get_n_entries (
    const WyreboxDaemonFactBatchImportRequest *request);

const WyreboxDaemonFactMutationRequest *
wyrebox_daemon_fact_batch_import_request_get_entry (
    const WyreboxDaemonFactBatchImportRequest *request,
    guint index);

const char *wyrebox_daemon_fact_batch_import_request_get_scope_id (
    const WyreboxDaemonFactBatchImportRequest *request);

gboolean wyrebox_daemon_fact_batch_import_request_validate (
    const WyreboxDaemonFactBatchImportRequest *request,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
