#pragma once

#include "wyrebox-daemon-request-identity.h"
#include "wyrebox-daemon-response-frame.h"
#include "wyrebox-daemon-stream-chunk-frame.h"
#include "wyrebox-daemon-wirelog-predicate-query-request.h"
#include "wyrebox-journal-writer.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_TYPE_DAEMON_WIRELOG_PREDICATE_QUERY_SERVICE \
    (wyrebox_daemon_wirelog_predicate_query_service_get_type())

G_DECLARE_FINAL_TYPE (WyreboxDaemonWirelogPredicateQueryService,
    wyrebox_daemon_wirelog_predicate_query_service,
    WYREBOX,
    DAEMON_WIRELOG_PREDICATE_QUERY_SERVICE,
    GObject)

typedef gboolean (*WyreboxDaemonWirelogPredicateQueryServiceFunc) (
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data,
    GError **error);

WyreboxDaemonWirelogPredicateQueryService *
wyrebox_daemon_wirelog_predicate_query_service_new (
    WyreboxDaemonWirelogPredicateQueryServiceFunc func,
    gpointer user_data,
    GDestroyNotify user_data_destroy);

WyreboxDaemonWirelogPredicateQueryService *
wyrebox_daemon_wirelog_predicate_query_service_new_wirelog (
    const char *rules_source,
    const char *journal_root_dir,
    GError **error);

void wyrebox_daemon_wirelog_predicate_query_service_set_audit_writer (
    WyreboxDaemonWirelogPredicateQueryService *self,
    WyreboxJournalWriter *audit_writer);

gboolean wyrebox_daemon_wirelog_predicate_query_service_handle_identity (
    WyreboxDaemonWirelogPredicateQueryService *self,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonResponseFrame *out_frame, GError **error);

G_END_DECLS
/* *INDENT-ON* */
