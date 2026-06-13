#pragma once

#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-wirelog-predicate-query-request.h"

#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  const char *predicate_id;
  const char *relation_name;
  const char *scope_kind;
  const char *output_format;
  gsize n_bindings;
} WyreboxDaemonWirelogPredicateQueryDescriptor;

gboolean wyrebox_daemon_wirelog_predicate_query_catalog_validate (
    WyreboxDaemonClientIdentityClass client_class,
    const char *caller_account_id,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    const WyreboxDaemonWirelogPredicateQueryDescriptor **out_descriptor,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
