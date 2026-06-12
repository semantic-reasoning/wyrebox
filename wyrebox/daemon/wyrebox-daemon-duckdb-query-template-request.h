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
  char *template_id;
  char *scope_id;
  gchar **parameters;
} WyreboxDaemonDuckDBQueryTemplateRequest;

void wyrebox_daemon_duckdb_query_template_request_clear (
    WyreboxDaemonDuckDBQueryTemplateRequest *request);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonDuckDBQueryTemplateRequest,
    wyrebox_daemon_duckdb_query_template_request_clear)

gboolean wyrebox_daemon_duckdb_query_template_request_init (
    WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const char *query_id,
    const char *template_id,
    const char *scope_id,
    const char * const *parameters,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
