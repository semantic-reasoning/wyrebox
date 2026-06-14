#include "wyrebox-daemon-duckdb-query-template-service.h"

#include "wyrebox-daemon-client-identity.h"
#include "wyrebox-daemon-duckdb-query-template-catalog.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-daemon-error.h"
#include "wyrebox-journal-writer.h"

#include <duckdb.h>
#include <errno.h>
#include <gio/gio.h>

typedef struct
{
  gchar *catalog_path;
  duckdb_database database;
  duckdb_connection connection;
} DuckDBQueryTemplateExecutor;

struct _WyreboxDaemonDuckDBQueryTemplateService
{
  GObject parent_instance;

  WyreboxDaemonDuckDBQueryTemplateServiceFunc func;
  gpointer user_data;
  GDestroyNotify user_data_destroy;
  WyreboxJournalWriter *audit_writer;
  GMutex backend_mutex;
};

G_DEFINE_TYPE (WyreboxDaemonDuckDBQueryTemplateService,
    wyrebox_daemon_duckdb_query_template_service, G_TYPE_OBJECT);

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
  duckdb_destroy_prepare (statement);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    duckdb_prepared_statement_clear)
/* *INDENT-ON* */

static void
duckdb_config_clear (duckdb_config *config)
{
  duckdb_destroy_config (config);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_config, duckdb_config_clear)
/* *INDENT-ON* */

static void
duckdb_query_template_executor_free (gpointer data)
{
  DuckDBQueryTemplateExecutor *executor = data;

  if (executor == NULL)
    return;

  if (executor->connection != NULL)
    duckdb_disconnect (&executor->connection);
  if (executor->database != NULL)
    duckdb_close (&executor->database);
  g_clear_pointer (&executor->catalog_path, g_free);
  g_free (executor);
}

/* *INDENT-OFF* */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DuckDBQueryTemplateExecutor,
    duckdb_query_template_executor_free)
/* *INDENT-ON* */

static gboolean
duckdb_query_template_prepare (DuckDBQueryTemplateExecutor *executor,
    const gchar *sql, duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (executor->connection, sql, out_statement) ==
      DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB query template prepare failed: %s",
      *out_statement != NULL ?
      duckdb_prepare_error (*out_statement) : "unknown DuckDB error");
  return FALSE;
}

static gboolean
duckdb_query_template_bind_varchar (duckdb_prepared_statement statement,
    idx_t index, const gchar *value, GError **error)
{
  if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB query template string bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
duckdb_query_template_bind_int64 (duckdb_prepared_statement statement,
    idx_t index, gint64 value, GError **error)
{
  if (duckdb_bind_int64 (statement, index, (int64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB query template BIGINT bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
duckdb_query_template_bind_uint64 (duckdb_prepared_statement statement,
    idx_t index, guint64 value, GError **error)
{
  if (duckdb_bind_uint64 (statement, index, (uint64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB query template UBIGINT bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
duckdb_query_template_parse_int64 (const gchar *value,
    const gchar *parameter_name, gint64 *out_value, GError **error)
{
  const gchar *digits = value;
  gchar *end = NULL;
  gint64 parsed = 0;

  if (*digits == '-')
    digits++;

  if (*digits == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be a signed integer",
        parameter_name);
    return FALSE;
  }

  for (const gchar * cursor = digits; *cursor != '\0'; cursor++) {
    if (!g_ascii_isdigit (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter '%s' must be a signed integer",
          parameter_name);
      return FALSE;
    }
  }

  errno = 0;
  parsed = g_ascii_strtoll (value, &end, 10);
  if (errno == ERANGE || end == NULL || *end != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be a signed integer",
        parameter_name);
    return FALSE;
  }

  *out_value = parsed;
  return TRUE;
}

static gboolean
duckdb_query_template_parse_uint64 (const gchar *value,
    const gchar *parameter_name, guint64 *out_value, GError **error)
{
  gchar *end = NULL;
  guint64 parsed = 0;

  if (*value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be an unsigned integer",
        parameter_name);
    return FALSE;
  }

  for (const gchar * cursor = value; *cursor != '\0'; cursor++) {
    if (!g_ascii_isdigit (*cursor)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "duckdb query template parameter '%s' must be an unsigned integer",
          parameter_name);
      return FALSE;
    }
  }

  errno = 0;
  parsed = g_ascii_strtoull (value, &end, 10);
  if (errno == ERANGE || end == NULL || *end != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template parameter '%s' must be an unsigned integer",
        parameter_name);
    return FALSE;
  }

  *out_value = parsed;
  return TRUE;
}

static gchar *
duckdb_query_template_make_contains_like_pattern (const gchar *term)
{
  g_autofree gchar *lower = NULL;
  g_autoptr (GString) pattern = NULL;

  lower = g_ascii_strdown (term, -1);
  pattern = g_string_new ("%");

  for (const gchar * cursor = lower; *cursor != '\0'; cursor++) {
    if (*cursor == '\\' || *cursor == '%' || *cursor == '_')
      g_string_append_c (pattern, '\\');
    g_string_append_c (pattern, *cursor);
  }

  g_string_append_c (pattern, '%');
  return g_string_free (g_steal_pointer (&pattern), FALSE);
}

static void
csv_append_value (GString *csv, const gchar *value)
{
  gboolean needs_quote = FALSE;

  for (const gchar * cursor = value; *cursor != '\0'; cursor++) {
    if (*cursor == '"' || *cursor == ',' || *cursor == '\n' || *cursor == '\r') {
      needs_quote = TRUE;
      break;
    }
  }

  if (!needs_quote) {
    g_string_append (csv, value);
    return;
  }

  g_string_append_c (csv, '"');
  for (const gchar * cursor = value; *cursor != '\0'; cursor++) {
    if (*cursor == '"')
      g_string_append_c (csv, '"');
    g_string_append_c (csv, *cursor);
  }
  g_string_append_c (csv, '"');
}

static void
csv_append_uint64 (GString *csv, guint64 value)
{
  g_string_append_printf (csv, "%" G_GUINT64_FORMAT, value);
}

static void
csv_append_nullable_uint64 (GString *csv, duckdb_result *result, idx_t column,
    idx_t row)
{
  if (duckdb_value_is_null (result, column, row))
    return;

  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, column, row));
}

static void
duckdb_owned_string_clear (char **value)
{
  if (value != NULL && *value != NULL)
    duckdb_free (*value);
}

typedef char *DuckDBOwnedString;

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (DuckDBOwnedString,
    duckdb_owned_string_clear)
/* *INDENT-ON* */

static void
csv_append_nullable_varchar (GString *csv, duckdb_result *result, idx_t column,
    idx_t row)
{
  g_auto (DuckDBOwnedString) value = NULL;

  if (duckdb_value_is_null (result, column, row))
    return;

  value = duckdb_value_varchar (result, column, row);
  csv_append_value (csv, value);
}

static gboolean
duckdb_query_template_append_uid_map_row (GString *csv,
    duckdb_result *result, idx_t row, GError **error)
{
  g_auto (DuckDBOwnedString) account_id = NULL;
  g_auto (DuckDBOwnedString) mailbox_id = NULL;
  g_auto (DuckDBOwnedString) message_id = NULL;
  g_auto (DuckDBOwnedString) object_id = NULL;

  for (idx_t column = 0; column < 6; column++) {
    if (duckdb_value_is_null (result, column, row)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB query template returned NULL in mailbox uid map");
      return FALSE;
    }
  }

  account_id = duckdb_value_varchar (result, 0, row);
  mailbox_id = duckdb_value_varchar (result, 1, row);
  message_id = duckdb_value_varchar (result, 4, row);
  object_id = duckdb_value_varchar (result, 5, row);

  csv_append_value (csv, account_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, mailbox_id);
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 2, row));
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 3, row));
  g_string_append_c (csv, ',');
  csv_append_value (csv, message_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, object_id);
  g_string_append_c (csv, '\n');

  return TRUE;
}

static gboolean
duckdb_query_template_append_derived_view_uid_map_row (GString *csv,
    duckdb_result *result, idx_t row, GError **error)
{
  g_auto (DuckDBOwnedString) account_id = NULL;
  g_auto (DuckDBOwnedString) view_id = NULL;
  g_auto (DuckDBOwnedString) message_id = NULL;
  g_auto (DuckDBOwnedString) object_id = NULL;
  g_auto (DuckDBOwnedString) rule_version_hash = NULL;

  for (idx_t column = 0; column < 7; column++) {
    if (duckdb_value_is_null (result, column, row)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB query template returned NULL in derived view uid map");
      return FALSE;
    }
  }

  account_id = duckdb_value_varchar (result, 0, row);
  view_id = duckdb_value_varchar (result, 1, row);
  message_id = duckdb_value_varchar (result, 4, row);
  object_id = duckdb_value_varchar (result, 5, row);
  rule_version_hash = duckdb_value_varchar (result, 6, row);

  csv_append_value (csv, account_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, view_id);
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 2, row));
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 3, row));
  g_string_append_c (csv, ',');
  csv_append_value (csv, message_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, object_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, rule_version_hash);
  g_string_append_c (csv, '\n');

  return TRUE;
}

static gboolean
duckdb_query_template_append_message_by_id_row (GString *csv,
    duckdb_result *result, idx_t row, GError **error)
{
  g_auto (DuckDBOwnedString) account_id = NULL;
  g_auto (DuckDBOwnedString) message_id = NULL;
  g_auto (DuckDBOwnedString) object_id = NULL;

  for (idx_t column = 0; column < 5; column++) {
    if (duckdb_value_is_null (result, column, row)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB query template returned NULL in message by id row");
      return FALSE;
    }
  }

  account_id = duckdb_value_varchar (result, 0, row);
  message_id = duckdb_value_varchar (result, 1, row);
  object_id = duckdb_value_varchar (result, 2, row);

  csv_append_value (csv, account_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, message_id);
  g_string_append_c (csv, ',');
  csv_append_value (csv, object_id);
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 3, row));
  g_string_append_c (csv, ',');
  csv_append_uint64 (csv, (guint64) duckdb_value_uint64 (result, 4, row));

  for (idx_t column = 5; column <= 11; column++) {
    g_string_append_c (csv, ',');
    csv_append_nullable_varchar (csv, result, column, row);
  }

  g_string_append_c (csv, ',');
  csv_append_nullable_uint64 (csv, result, 12, row);
  g_string_append_c (csv, ',');
  csv_append_nullable_uint64 (csv, result, 13, row);
  g_string_append_c (csv, '\n');

  return TRUE;
}

static gboolean
duckdb_query_template_execute_derived_view_uid_map (DuckDBQueryTemplateExecutor
    *executor, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, GError **error)
{
  static const gchar *sql =
      "SELECT dvm.account_id, dvm.view_id, mus.uidvalidity, "
      "dvm.uid, dvm.message_id, m.object_id, dvm.rule_version_hash "
      "FROM derived_view_memberships dvm "
      "JOIN mailbox_uid_state mus ON mus.account_id = dvm.account_id "
      "AND mus.namespace_kind = 'derived_view' "
      "AND mus.namespace_id = dvm.view_id "
      "JOIN messages m ON m.account_id = dvm.account_id "
      "AND m.message_id = dvm.message_id "
      "WHERE dvm.account_id = ? "
      "AND dvm.view_id = ? "
      "AND dvm.is_visible = TRUE " "ORDER BY dvm.uid ASC, dvm.message_id ASC;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const gchar *view_id = request->parameters[0];

  if (!duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, view_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_derived_view_uid_map_row (csv, &result,
            row, error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
duckdb_query_template_execute_uid_map (DuckDBQueryTemplateExecutor *executor,
    const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, GError **error)
{
  static const gchar *sql =
      "SELECT mm.account_id, mm.mailbox_id, mus.uidvalidity, "
      "mm.uid, mm.message_id, m.object_id "
      "FROM mailbox_memberships mm "
      "JOIN mailbox_uid_state mus ON mus.account_id = mm.account_id "
      "AND mus.namespace_kind = 'mailbox' "
      "AND mus.namespace_id = mm.mailbox_id "
      "JOIN messages m ON m.account_id = mm.account_id "
      "AND m.message_id = mm.message_id "
      "WHERE mm.account_id = ? "
      "AND mm.mailbox_id = ? "
      "AND mm.is_visible = TRUE " "ORDER BY mm.uid ASC, mm.message_id ASC;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const gchar *mailbox_id = request->parameters[0];

  if (!duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, mailbox_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_uid_map_row (csv, &result, row, error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
duckdb_query_template_execute_message_by_id (DuckDBQueryTemplateExecutor
    *executor, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, GError **error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "LEFT JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND m.message_id = ? " "ORDER BY m.message_id ASC " "LIMIT 1;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const gchar *message_id = request->parameters[0];

  if (!duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, message_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
duckdb_query_template_execute_messages_by_from_addr (DuckDBQueryTemplateExecutor
    *executor, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, GError **error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND mh.from_addr = ? "
      "ORDER BY m.journal_sequence ASC, m.message_id ASC " "LIMIT ? OFFSET ?;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const gchar *from_addr = request->parameters[0];
  guint64 limit = 0;
  guint64 offset = 0;

  if (!duckdb_query_template_parse_uint64 (request->parameters[1], "limit",
          &limit, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[2], "offset",
          &offset, error) ||
      !duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, from_addr, error) ||
      !duckdb_query_template_bind_uint64 (statement, 3, limit, error) ||
      !duckdb_query_template_bind_uint64 (statement, 4, offset, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
    duckdb_query_template_execute_messages_by_sender_domain
    (DuckDBQueryTemplateExecutor * executor,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    WyreboxDaemonStreamChunkFrame * out_chunk, GError ** error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND mh.sender_domain = ? "
      "ORDER BY m.journal_sequence ASC, m.message_id ASC " "LIMIT ? OFFSET ?;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autofree gchar *sender_domain = NULL;
  guint64 limit = 0;
  guint64 offset = 0;

  sender_domain = g_ascii_strdown (request->parameters[0], -1);

  if (!duckdb_query_template_parse_uint64 (request->parameters[1], "limit",
          &limit, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[2], "offset",
          &offset, error) ||
      !duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, sender_domain,
          error) ||
      !duckdb_query_template_bind_uint64 (statement, 3, limit, error) ||
      !duckdb_query_template_bind_uint64 (statement, 4, offset, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
duckdb_query_template_execute_messages_by_subject (DuckDBQueryTemplateExecutor
    *executor, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, GError **error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND mh.subject = ? "
      "ORDER BY m.journal_sequence ASC, m.message_id ASC " "LIMIT ? OFFSET ?;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  const gchar *subject = request->parameters[0];
  guint64 limit = 0;
  guint64 offset = 0;

  if (!duckdb_query_template_parse_uint64 (request->parameters[1], "limit",
          &limit, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[2], "offset",
          &offset, error) ||
      !duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, subject, error) ||
      !duckdb_query_template_bind_uint64 (statement, 3, limit, error) ||
      !duckdb_query_template_bind_uint64 (statement, 4, offset, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
    duckdb_query_template_execute_messages_subject_contains
    (DuckDBQueryTemplateExecutor * executor,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    WyreboxDaemonStreamChunkFrame * out_chunk, GError ** error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND mh.subject IS NOT NULL "
      "AND lower(mh.subject) LIKE ? ESCAPE '\\' "
      "ORDER BY m.journal_sequence ASC, m.message_id ASC " "LIMIT ? OFFSET ?;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  g_autofree gchar *subject_pattern = NULL;
  guint64 limit = 0;
  guint64 offset = 0;

  subject_pattern =
      duckdb_query_template_make_contains_like_pattern (request->parameters[0]);

  if (!duckdb_query_template_parse_uint64 (request->parameters[1], "limit",
          &limit, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[2], "offset",
          &offset, error) ||
      !duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_varchar (statement, 2, subject_pattern,
          error) ||
      !duckdb_query_template_bind_uint64 (statement, 3, limit, error) ||
      !duckdb_query_template_bind_uint64 (statement, 4, offset, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
    duckdb_query_template_execute_messages_by_date_range
    (DuckDBQueryTemplateExecutor * executor,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    WyreboxDaemonStreamChunkFrame * out_chunk, GError ** error)
{
  static const gchar *sql =
      "SELECT m.account_id, m.message_id, m.object_id, "
      "m.journal_offset, m.journal_sequence, mh.rfc_message_id, "
      "mh.subject, mh.from_addr, mh.to_addr, mh.cc_addr, mh.bcc_addr, "
      "mh.date_raw, mh.journal_offset, mh.journal_sequence "
      "FROM messages m "
      "JOIN message_headers mh ON mh.message_id = m.message_id "
      "WHERE m.account_id = ? "
      "AND mh.date_unix_us IS NOT NULL "
      "AND mh.date_unix_us >= ? "
      "AND mh.date_unix_us < ? "
      "ORDER BY m.journal_sequence ASC, m.message_id ASC " "LIMIT ? OFFSET ?;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GString) csv = NULL;
  g_autoptr (GBytes) bytes = NULL;
  gint64 start_unix_us = 0;
  gint64 end_unix_us = 0;
  guint64 limit = 0;
  guint64 offset = 0;

  if (!duckdb_query_template_parse_int64 (request->parameters[0],
          "start_unix_us", &start_unix_us, error) ||
      !duckdb_query_template_parse_int64 (request->parameters[1],
          "end_unix_us", &end_unix_us, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[2], "limit",
          &limit, error) ||
      !duckdb_query_template_parse_uint64 (request->parameters[3], "offset",
          &offset, error) ||
      !duckdb_query_template_prepare (executor, sql, &statement, error) ||
      !duckdb_query_template_bind_varchar (statement, 1, request->scope_id,
          error) ||
      !duckdb_query_template_bind_int64 (statement, 2, start_unix_us, error) ||
      !duckdb_query_template_bind_int64 (statement, 3, end_unix_us, error) ||
      !duckdb_query_template_bind_uint64 (statement, 4, limit, error) ||
      !duckdb_query_template_bind_uint64 (statement, 5, offset, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template execution failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  csv = g_string_new
      ("account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    if (!duckdb_query_template_append_message_by_id_row (csv, &result, row,
            error))
      return FALSE;
  }

  {
    gsize csv_len = csv->len;
    gchar *csv_data = g_string_free (g_steal_pointer (&csv), FALSE);

    bytes = g_bytes_new_take (csv_data, csv_len);
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
duckdb_query_template_service_execute (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, gpointer user_data,
    GError **error)
{
  DuckDBQueryTemplateExecutor *executor = user_data;

  if (g_strcmp0 (request->template_id, "mailbox.uid_map.v1") == 0)
    return duckdb_query_template_execute_uid_map (executor, identity, request,
        out_chunk, error);

  if (g_strcmp0 (request->template_id, "derived_view.uid_map.v1") == 0)
    return duckdb_query_template_execute_derived_view_uid_map (executor,
        identity, request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "message.by_id.v1") == 0)
    return duckdb_query_template_execute_message_by_id (executor, identity,
        request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "messages.by_from_addr.v1") == 0)
    return duckdb_query_template_execute_messages_by_from_addr (executor,
        identity, request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "messages.by_sender_domain.v1") == 0)
    return duckdb_query_template_execute_messages_by_sender_domain (executor,
        identity, request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "messages.by_subject.v1") == 0)
    return duckdb_query_template_execute_messages_by_subject (executor,
        identity, request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "messages.subject_contains.v1") == 0)
    return duckdb_query_template_execute_messages_subject_contains (executor,
        identity, request, out_chunk, error);

  if (g_strcmp0 (request->template_id, "messages.by_date_range.v1") == 0)
    return duckdb_query_template_execute_messages_by_date_range (executor,
        identity, request, out_chunk, error);

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_INVALID_ARGUMENT,
      "unsupported duckdb query template '%s'", request->template_id);
  return FALSE;
}

static gboolean
validate_duckdb_query_template_chunk (const WyreboxDaemonRequestIdentity
    *identity, const WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const WyreboxDaemonStreamChunkFrame *chunk, GError **error)
{
  if (chunk->request_id == NULL || *chunk->request_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk request_id is required");
    return FALSE;
  }

  if (g_strcmp0 (chunk->request_id, identity->request_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk request_id must match request "
        "envelope");
    return FALSE;
  }

  if (chunk->message_id != NULL && *chunk->message_id != '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk must not contain message_id");
    return FALSE;
  }

  if (chunk->query_id == NULL || *chunk->query_id == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk query_id is required");
    return FALSE;
  }

  if (g_strcmp0 (chunk->query_id, request->query_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk query_id must match request");
    return FALSE;
  }

  if (chunk->correlation_id != NULL
      && g_strcmp0 (chunk->correlation_id, identity->correlation_id) != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "duckdb query template stream chunk correlation_id must match request "
        "envelope");
    return FALSE;
  }

  return TRUE;
}

static void
wyrebox_daemon_duckdb_query_template_service_finalize (GObject *object)
{
  WyreboxDaemonDuckDBQueryTemplateService *self =
      WYREBOX_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE (object);

  if (self->user_data_destroy != NULL && self->user_data != NULL)
    self->user_data_destroy (self->user_data);

  g_clear_object (&self->audit_writer);
  g_mutex_clear (&self->backend_mutex);

  G_OBJECT_CLASS
      (wyrebox_daemon_duckdb_query_template_service_parent_class)->finalize
      (object);
}

static void
    wyrebox_daemon_duckdb_query_template_service_class_init
    (WyreboxDaemonDuckDBQueryTemplateServiceClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize =
      wyrebox_daemon_duckdb_query_template_service_finalize;
}

static void
    wyrebox_daemon_duckdb_query_template_service_init
    (WyreboxDaemonDuckDBQueryTemplateService * self)
{
  g_mutex_init (&self->backend_mutex);
}

WyreboxDaemonDuckDBQueryTemplateService
    * wyrebox_daemon_duckdb_query_template_service_new
    (WyreboxDaemonDuckDBQueryTemplateServiceFunc func, gpointer user_data,
    GDestroyNotify user_data_destroy) {
  g_return_val_if_fail (func != NULL, NULL);

  WyreboxDaemonDuckDBQueryTemplateService *self =
      g_object_new (WYREBOX_TYPE_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE, NULL);

  self->func = func;
  self->user_data = user_data;
  self->user_data_destroy = user_data_destroy;

  return self;
}

void wyrebox_daemon_duckdb_query_template_service_set_audit_writer
    (WyreboxDaemonDuckDBQueryTemplateService * self,
    WyreboxJournalWriter * audit_writer)
{
  g_return_if_fail (WYREBOX_IS_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE (self));
  g_return_if_fail (audit_writer == NULL || WYREBOX_IS_JOURNAL_WRITER
      (audit_writer));

  g_set_object (&self->audit_writer, audit_writer);
}

WyreboxDaemonDuckDBQueryTemplateService
    *
wyrebox_daemon_duckdb_query_template_service_new_duckdb (const gchar
    *catalog_path, GError **error)
{
  g_auto (duckdb_config) config = NULL;
  char *open_error = NULL;
  g_autoptr (DuckDBQueryTemplateExecutor) executor = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;

  g_return_val_if_fail (catalog_path != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (duckdb_create_config (&config) != DuckDBSuccess ||
      duckdb_set_config (config, "access_mode", "READ_ONLY") != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template read-only configuration failed");
    return NULL;
  }

  executor = g_new0 (DuckDBQueryTemplateExecutor, 1);
  executor->catalog_path = g_strdup (catalog_path);

  if (duckdb_open_ext (catalog_path, &executor->database, config,
          &open_error) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB query template read-only open failed: %s",
        open_error != NULL ? open_error : "unknown DuckDB error");
    if (open_error != NULL)
      duckdb_free (open_error);
    return NULL;
  }

  if (duckdb_connect (executor->database, &executor->connection) !=
      DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "DuckDB query template connect failed");
    return NULL;
  }

  service = wyrebox_daemon_duckdb_query_template_service_new
      (duckdb_query_template_service_execute,
      g_steal_pointer (&executor), duckdb_query_template_executor_free);

  return g_steal_pointer (&service);
}

static const gchar *
audit_required_value (const gchar *value, const gchar *fallback)
{
  if (value != NULL && *value != '\0')
    return value;

  return fallback;
}

static void
append_duckdb_query_failure_audit (WyreboxDaemonDuckDBQueryTemplateService
    *self, const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest *request, const GError *cause)
{
  g_autoptr (GError) ignored_error = NULL;
  g_autoptr (GBytes) payload_bytes = NULL;
  guint64 offset = 0;
  guint64 sequence = 0;
  WyreboxDaemonErrorClass error_class = WYREBOX_DAEMON_ERROR_INTERNAL_ERROR;
  g_autofree gchar *error_domain = NULL;
  WyreboxDaemonAuditPayload payload = {
    .operation = WYREBOX_DAEMON_AUDIT_OPERATION_DUCKDB_QUERY_TEMPLATE,
    .outcome = WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE,
    .request_id = (gchar *) audit_required_value (identity->request_id,
        "unknown-request"),
    .correlation_id = identity->correlation_id,
    .caller_identity = (gchar *) audit_required_value
        (identity->caller_identity, "unknown"),
    .account_identity = (gchar *) audit_required_value
        (identity->account_identity, "unknown"),
    .tool_identity = identity->tool_identity,
    .scope_id = (gchar *) audit_required_value (request->scope_id,
        identity->account_identity != NULL ? identity->account_identity :
        "unknown"),
    .query_id = (gchar *) audit_required_value (request->query_id,
        "unknown-query"),
    .template_id = (gchar *) audit_required_value (request->template_id,
        "unknown-template"),
    .error_code = cause != NULL ? cause->code : G_IO_ERROR_FAILED,
    .error_message = (gchar *) audit_required_value
        (cause != NULL ? cause->message : NULL, "unknown duckdb query error"),
  };

  if (self->audit_writer == NULL)
    return;

  if (cause != NULL && cause->domain == G_IO_ERROR)
    error_class = wyrebox_daemon_error_class_from_g_error_code (cause->code);

  error_domain = g_strdup (cause != NULL ?
      g_quark_to_string (cause->domain) : g_quark_to_string (G_IO_ERROR));
  payload.error_domain = error_domain;
  payload.error_class = (gchar *) audit_required_value
      (wyrebox_daemon_error_class_to_string (error_class), "internalError");

  payload_bytes = wyrebox_daemon_audit_payload_encode (&payload,
      &ignored_error);
  if (payload_bytes == NULL)
    return;

  (void) wyrebox_journal_writer_append (self->audit_writer,
      WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED, payload_bytes, &offset,
      &sequence, &ignored_error);
}

gboolean
    wyrebox_daemon_duckdb_query_template_service_handle_identity
    (WyreboxDaemonDuckDBQueryTemplateService * self,
    const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonDuckDBQueryTemplateRequest * request,
    WyreboxDaemonResponseFrame * out_frame, GError ** error)
{
  g_auto (WyreboxDaemonStreamChunkFrame) chunk = { 0 };
  g_auto (WyreboxDaemonStreamChunkFrame) response_chunk = { 0 };
  g_autoptr (GError) local_error = NULL;
  const WyreboxDaemonDuckDBQueryTemplateDescriptor *descriptor = NULL;
  WyreboxDaemonClientIdentityClass client_class =
      WYREBOX_DAEMON_CLIENT_IDENTITY_UNKNOWN;

  g_return_val_if_fail (WYREBOX_IS_DAEMON_DUCKDB_QUERY_TEMPLATE_SERVICE
      (self), FALSE);
  g_return_val_if_fail (identity != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_frame != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  client_class = wyrebox_daemon_client_identity_classify_request (identity);
  if (!wyrebox_daemon_duckdb_query_template_catalog_validate (client_class,
          identity->account_identity, request, &descriptor, &local_error)) {
    append_duckdb_query_failure_audit (self, identity, request, local_error);
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }
  (void) descriptor;

  g_mutex_lock (&self->backend_mutex);
  if (!self->func (identity, request, &chunk, self->user_data, &local_error)) {
    g_mutex_unlock (&self->backend_mutex);
    if (local_error == NULL) {
      g_set_error (&local_error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "duckdb query template service failed without error detail");
    }
    append_duckdb_query_failure_audit (self, identity, request, local_error);
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }
  g_mutex_unlock (&self->backend_mutex);

  if (!validate_duckdb_query_template_chunk (identity, request, &chunk,
          &local_error)) {
    append_duckdb_query_failure_audit (self, identity, request, local_error);
    g_propagate_error (error, g_steal_pointer (&local_error));
    return FALSE;
  }

  if (!wyrebox_daemon_stream_chunk_frame_init (&response_chunk,
          identity->request_id,
          NULL, request->query_id, identity->correlation_id,
          chunk.chunk_index, chunk.bytes, chunk.end_of_stream, error))
    return FALSE;

  return wyrebox_daemon_response_frame_init_stream_chunk (out_frame,
      &response_chunk, error);
}
