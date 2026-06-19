#pragma once

#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-duckdb-query-template-request.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *column_name;
  const char *logical_type;
  gboolean nullable;
  const char *description;
} WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor;

typedef struct
{
  const char *template_id;
  const char *name;
  const char *scope_kind;
  const char *output_format;
  gsize n_parameters;
  const char * const *parameter_names;
  gsize n_result_columns;
  const WyreboxDaemonDuckDBQueryTemplateResultColumnDescriptor *
      result_columns;
} WyreboxDaemonDuckDBQueryTemplateDescriptor;

const WyreboxDaemonDuckDBQueryTemplateDescriptor *
wyrebox_daemon_duckdb_query_template_catalog_lookup (const char *template_id);

gboolean wyrebox_daemon_duckdb_query_template_catalog_validate (
    WyreboxDaemonClientIdentityClass client_class,
    const char *caller_account_id,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const WyreboxDaemonDuckDBQueryTemplateDescriptor **out_descriptor,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
