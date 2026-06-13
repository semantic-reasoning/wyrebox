#pragma once

#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-stream-chunk-frame.h"
#include "wyrebox-daemon-duckdb-query-template-request.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE \
    (wyrebox_daemon_duckdb_query_template_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonDuckDBQueryTemplateService,
    wyrebox_daemon_duckdb_query_template_service,
    WYREBOX,
    DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonDuckDBQueryTemplateServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error);

WyreboxDaemonDuckDBQueryTemplateService *
wyrebox_daemon_duckdb_query_template_service_new (
    WyreboxDaemonDuckDBQueryTemplateServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

WyreboxDaemonDuckDBQueryTemplateService *
wyrebox_daemon_duckdb_query_template_service_new_duckdb (
    const gchar *catalog_path,
    GError **error);

void wyrebox_daemon_duckdb_query_template_service_set_audit_writer (
    WyreboxDaemonDuckDBQueryTemplateService *self,
    WyreboxJournalWriter *audit_writer);

gboolean wyrebox_daemon_duckdb_query_template_service_handle_identity (
    WyreboxDaemonDuckDBQueryTemplateService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error);

G_END_DECLS
/* *INDENT-ON* */
