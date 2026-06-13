#pragma once

#include <glib-object.h>

typedef enum
{
  WYREBOX_DAEMON_AUDIT_OPERATION_SINGLE_FACT_MUTATION,
  WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT,
  WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE,
} WyreboxDaemonAuditOperation;

typedef enum
{
  WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS,
  WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE,
} WyreboxDaemonAuditOutcome;

typedef struct
{
  WyreboxDaemonAuditOperation operation;
  WyreboxDaemonAuditOutcome outcome;
  char *request_id;
  char *correlation_id;
  char *caller_identity;
  char *account_identity;
  char *tool_identity;
  char *scope_id;
  guint64 mutation_count;
  char *predicate_id;
  guint64 final_journal_offset;
  guint64 final_journal_sequence;
  char *query_id;
  char *template_id;
  char *error_domain;
  gint error_code;
  char *error_class;
  char *error_message;
} WyreboxDaemonAuditPayload;

/* *INDENT-OFF* */
G_BEGIN_DECLS

void wyrebox_daemon_audit_payload_clear (
    WyreboxDaemonAuditPayload *payload);

GBytes *wyrebox_daemon_audit_payload_encode (
    const WyreboxDaemonAuditPayload *payload,
    GError **error);

gboolean wyrebox_daemon_audit_payload_decode (
    GBytes *bytes,
    WyreboxDaemonAuditPayload *out_payload,
    GError **error);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDaemonAuditPayload,
    wyrebox_daemon_audit_payload_clear)

G_END_DECLS
/* *INDENT-ON* */
