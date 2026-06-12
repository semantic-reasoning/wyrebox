#pragma once

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  /*
   * Strings and vectors are owned by the request and released by clear().
   */
  char *query_id;
  char *predicate_id;
  char *scope_id;
  gchar **bindings;
} WyreboxDaemonWirelogPredicateQueryRequest;

void wyrebox_daemon_wirelog_predicate_query_request_clear (
    WyreboxDaemonWirelogPredicateQueryRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonWirelogPredicateQueryRequest,
    wyrebox_daemon_wirelog_predicate_query_request_clear)

gboolean wyrebox_daemon_wirelog_predicate_query_request_init (
    WyreboxDaemonWirelogPredicateQueryRequest *request,
    const char *query_id,
    const char *predicate_id,
    const char *scope_id,
    const char * const *bindings,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
