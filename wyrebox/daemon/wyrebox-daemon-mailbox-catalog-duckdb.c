#include "wyrebox-daemon-mailbox-catalog-duckdb.h"

#include <duckdb.h>
#include <gio/gio.h>

struct _WyreboxDaemonMailboxCatalogDuckDB
{
  gchar *catalog_path;
  duckdb_database database;
  duckdb_connection connection;
  GMutex mutex;
};

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

typedef struct
{
  WyreboxDaemonMailboxListEntryKind kind;
  gchar *mailbox_id;
  gchar *mailbox_name;
  guint32 uid_validity;
  guint32 uid_next;
  guint32 message_count;
} MailboxCatalogSelectRow;

static void
mailbox_catalog_select_row_clear (MailboxCatalogSelectRow *row)
{
  if (row == NULL)
    return;

  g_clear_pointer (&row->mailbox_id, g_free);
  g_clear_pointer (&row->mailbox_name, g_free);
  row->kind = WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
  row->uid_validity = 0;
  row->uid_next = 0;
  row->message_count = 0;
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (MailboxCatalogSelectRow,
    mailbox_catalog_select_row_clear)
/* *INDENT-ON* */

static gboolean
prepare_statement (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    const gchar *sql, duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (catalog->connection, sql, out_statement) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB mailbox catalog prepare failed: %s",
      *out_statement != NULL ?
      duckdb_prepare_error (*out_statement) : "unknown DuckDB error");
  return FALSE;
}

static gboolean
bind_varchar (duckdb_prepared_statement statement, idx_t index,
    const gchar *value, GError **error)
{
  if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB mailbox catalog string bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
execute_statement (duckdb_prepared_statement statement, duckdb_result *result,
    GError **error)
{
  if (duckdb_execute_prepared (statement, result) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB mailbox catalog execution failed: %s",
      duckdb_result_error (result) != NULL ?
      duckdb_result_error (result) : "unknown DuckDB error");
  return FALSE;
}

static gboolean
read_required_varchar (duckdb_result *result, idx_t column, idx_t row,
    gchar **out_value, GError **error)
{
  g_auto (DuckDBOwnedString) value = NULL;

  if (duckdb_value_is_null (result, column, row)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB mailbox catalog returned NULL text at column %"
        G_GUINT64_FORMAT, (guint64) column);
    return FALSE;
  }

  value = duckdb_value_varchar (result, column, row);
  *out_value = g_strdup (value);
  return TRUE;
}

static gboolean
read_required_uint32 (duckdb_result *result, idx_t column, idx_t row,
    guint32 *out_value, GError **error)
{
  guint64 value = 0;

  if (duckdb_value_is_null (result, column, row)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB mailbox catalog returned NULL integer at column %"
        G_GUINT64_FORMAT, (guint64) column);
    return FALSE;
  }

  value = (guint64) duckdb_value_uint64 (result, column, row);
  if (value > G_MAXUINT32) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "DuckDB mailbox catalog integer at column %" G_GUINT64_FORMAT
        " exceeds guint32", (guint64) column);
    return FALSE;
  }

  *out_value = (guint32) value;
  return TRUE;
}

static gboolean
append_list_rows (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    const gchar *account_id, WyreboxDaemonMailboxListResult *result,
    GError **error)
{
  static const gchar *sql =
      "SELECT kind, stable_id, imap_name "
      "FROM ("
      "SELECT 0 AS sort_kind, 'ordinary' AS kind, mailbox_id AS stable_id, "
      "imap_name FROM mailboxes "
      "WHERE account_id = ? AND is_visible = TRUE "
      "UNION ALL "
      "SELECT 1 AS sort_kind, 'virtual' AS kind, view_id AS stable_id, "
      "imap_name FROM derived_views "
      "WHERE account_id = ? AND is_visible = TRUE"
      ") visible_mailboxes "
      "ORDER BY imap_name ASC, sort_kind ASC, stable_id ASC;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) query_result = { 0 };

  if (!prepare_statement (catalog, sql, &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, account_id, error) ||
      !execute_statement (statement, &query_result, error))
    return FALSE;

  for (idx_t row = 0; row < duckdb_row_count (&query_result); row++) {
    g_autofree gchar *kind_text = NULL;
    g_autofree gchar *mailbox_id = NULL;
    g_autofree gchar *mailbox_name = NULL;
    WyreboxDaemonMailboxListEntryKind kind =
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;

    if (!read_required_varchar (&query_result, 0, row, &kind_text, error) ||
        !read_required_varchar (&query_result, 1, row, &mailbox_id, error) ||
        !read_required_varchar (&query_result, 2, row, &mailbox_name, error))
      return FALSE;

    if (g_strcmp0 (kind_text, "virtual") == 0)
      kind = WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL;

    if (!wyrebox_daemon_mailbox_list_result_append_entry (result,
            kind, mailbox_id, mailbox_name, "/", NULL, TRUE,
            WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
select_rows_by_selector (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    gboolean by_name, const gchar *account_id, const gchar *selector,
    GPtrArray *rows, GError **error)
{
  static const gchar *by_id_sql =
      "SELECT kind, stable_id, imap_name, uidvalidity, uidnext, "
      "message_count "
      "FROM ("
      "SELECT 'ordinary' AS kind, m.mailbox_id AS stable_id, m.imap_name, "
      "mus.uidvalidity, mus.uidnext, COUNT(mm.membership_id) AS message_count "
      "FROM mailboxes m "
      "JOIN mailbox_uid_state mus ON mus.account_id = m.account_id "
      "AND mus.namespace_kind = 'mailbox' "
      "AND mus.namespace_id = m.mailbox_id "
      "LEFT JOIN mailbox_memberships mm ON mm.account_id = m.account_id "
      "AND mm.mailbox_id = m.mailbox_id AND mm.is_visible = TRUE "
      "WHERE m.account_id = ? AND m.is_visible = TRUE "
      "AND m.mailbox_id = ? "
      "GROUP BY m.mailbox_id, m.imap_name, mus.uidvalidity, mus.uidnext "
      "UNION ALL "
      "SELECT 'virtual' AS kind, dv.view_id AS stable_id, dv.imap_name, "
      "mus.uidvalidity, mus.uidnext, COUNT(dvm.membership_id) AS "
      "message_count "
      "FROM derived_views dv "
      "JOIN mailbox_uid_state mus ON mus.account_id = dv.account_id "
      "AND mus.namespace_kind = 'derived_view' "
      "AND mus.namespace_id = dv.view_id "
      "LEFT JOIN derived_view_memberships dvm ON dvm.account_id = "
      "dv.account_id "
      "AND dvm.view_id = dv.view_id AND dvm.is_visible = TRUE "
      "WHERE dv.account_id = ? AND dv.is_visible = TRUE "
      "AND dv.view_id = ? "
      "GROUP BY dv.view_id, dv.imap_name, mus.uidvalidity, mus.uidnext"
      ") matches;";
  static const gchar *by_name_sql =
      "SELECT kind, stable_id, imap_name, uidvalidity, uidnext, "
      "message_count "
      "FROM ("
      "SELECT 'ordinary' AS kind, m.mailbox_id AS stable_id, m.imap_name, "
      "mus.uidvalidity, mus.uidnext, COUNT(mm.membership_id) AS message_count "
      "FROM mailboxes m "
      "JOIN mailbox_uid_state mus ON mus.account_id = m.account_id "
      "AND mus.namespace_kind = 'mailbox' "
      "AND mus.namespace_id = m.mailbox_id "
      "LEFT JOIN mailbox_memberships mm ON mm.account_id = m.account_id "
      "AND mm.mailbox_id = m.mailbox_id AND mm.is_visible = TRUE "
      "WHERE m.account_id = ? AND m.is_visible = TRUE "
      "AND m.imap_name = ? "
      "GROUP BY m.mailbox_id, m.imap_name, mus.uidvalidity, mus.uidnext "
      "UNION ALL "
      "SELECT 'virtual' AS kind, dv.view_id AS stable_id, dv.imap_name, "
      "mus.uidvalidity, mus.uidnext, COUNT(dvm.membership_id) AS "
      "message_count "
      "FROM derived_views dv "
      "JOIN mailbox_uid_state mus ON mus.account_id = dv.account_id "
      "AND mus.namespace_kind = 'derived_view' "
      "AND mus.namespace_id = dv.view_id "
      "LEFT JOIN derived_view_memberships dvm ON dvm.account_id = "
      "dv.account_id "
      "AND dvm.view_id = dv.view_id AND dvm.is_visible = TRUE "
      "WHERE dv.account_id = ? AND dv.is_visible = TRUE "
      "AND dv.imap_name = ? "
      "GROUP BY dv.view_id, dv.imap_name, mus.uidvalidity, mus.uidnext"
      ") matches;";
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  const gchar *sql = by_name ? by_name_sql : by_id_sql;

  if (!prepare_statement (catalog, sql, &statement, error) ||
      !bind_varchar (statement, 1, account_id, error) ||
      !bind_varchar (statement, 2, selector, error) ||
      !bind_varchar (statement, 3, account_id, error) ||
      !bind_varchar (statement, 4, selector, error) ||
      !execute_statement (statement, &result, error))
    return FALSE;

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    g_autofree gchar *kind_text = NULL;
    MailboxCatalogSelectRow *select_row = g_new0 (MailboxCatalogSelectRow, 1);

    if (!read_required_varchar (&result, 0, row, &kind_text, error) ||
        !read_required_varchar (&result, 1, row, &select_row->mailbox_id,
            error) ||
        !read_required_varchar (&result, 2, row, &select_row->mailbox_name,
            error) ||
        !read_required_uint32 (&result, 3, row, &select_row->uid_validity,
            error) ||
        !read_required_uint32 (&result, 4, row, &select_row->uid_next, error) ||
        !read_required_uint32 (&result, 5, row, &select_row->message_count,
            error)) {
      mailbox_catalog_select_row_clear (select_row);
      g_free (select_row);
      return FALSE;
    }

    select_row->kind =
        g_strcmp0 (kind_text, "virtual") == 0 ?
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL :
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
    g_ptr_array_add (rows, select_row);
  }

  return TRUE;
}

static void
select_row_pointer_free (gpointer data)
{
  MailboxCatalogSelectRow *row = data;

  mailbox_catalog_select_row_clear (row);
  g_free (row);
}

void wyrebox_daemon_mailbox_catalog_duckdb_free
    (WyreboxDaemonMailboxCatalogDuckDB * catalog)
{
  if (catalog == NULL)
    return;

  if (catalog->connection != NULL)
    duckdb_disconnect (&catalog->connection);
  if (catalog->database != NULL)
    duckdb_close (&catalog->database);
  g_mutex_clear (&catalog->mutex);
  g_clear_pointer (&catalog->catalog_path, g_free);
  g_free (catalog);
}

WyreboxDaemonMailboxCatalogDuckDB *
wyrebox_daemon_mailbox_catalog_duckdb_new (const char *catalog_path,
    GError **error)
{
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (catalog_path == NULL || *catalog_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "DuckDB mailbox catalog path is required");
    return NULL;
  }

  catalog = g_new0 (WyreboxDaemonMailboxCatalogDuckDB, 1);
  catalog->catalog_path = g_strdup (catalog_path);
  g_mutex_init (&catalog->mutex);

  if (duckdb_open (catalog_path, &catalog->database) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB mailbox catalog open failed for '%s'", catalog_path);
    return NULL;
  }

  if (duckdb_connect (catalog->database, &catalog->connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB mailbox catalog connect failed for '%s'", catalog_path);
    return NULL;
  }

  return g_steal_pointer (&catalog);
}

gboolean
wyrebox_daemon_mailbox_catalog_duckdb_list (const
    WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxListRequest *request,
    WyreboxDaemonMailboxListResult *out_result, gpointer user_data,
    GError **error)
{
  WyreboxDaemonMailboxCatalogDuckDB *catalog = user_data;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  gboolean ok = FALSE;

  (void) identity;
  g_return_val_if_fail (catalog != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  wyrebox_daemon_mailbox_list_result_init_empty (&result);

  g_mutex_lock (&catalog->mutex);
  ok = append_list_rows (catalog, request->account_identity, &result, error);
  g_mutex_unlock (&catalog->mutex);

  if (!ok)
    return FALSE;

  wyrebox_daemon_mailbox_list_result_clear (out_result);
  *out_result = result;
  result.entries = NULL;
  return TRUE;
}

gboolean
wyrebox_daemon_mailbox_catalog_duckdb_select (const
    WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result, gpointer user_data,
    GError **error)
{
  WyreboxDaemonMailboxCatalogDuckDB *catalog = user_data;
  g_autoptr (GPtrArray) rows = NULL;
  const gboolean by_name =
      request != NULL && request->mailbox_name != NULL &&
      request->mailbox_name[0] != '\0';
  const gchar *selector = by_name ? request->mailbox_name : request->mailbox_id;
  gboolean ok = FALSE;
  const MailboxCatalogSelectRow *row = NULL;

  (void) identity;
  g_return_val_if_fail (catalog != NULL, FALSE);
  g_return_val_if_fail (request != NULL, FALSE);
  g_return_val_if_fail (out_result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  rows = g_ptr_array_new_with_free_func (select_row_pointer_free);

  g_mutex_lock (&catalog->mutex);
  ok = select_rows_by_selector (catalog, by_name, request->account_identity,
      selector, rows, error);
  g_mutex_unlock (&catalog->mutex);

  if (!ok)
    return FALSE;

  if (rows->len == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "mailbox catalog entry not found for account '%s'",
        request->account_identity);
    return FALSE;
  }

  if (rows->len > 1) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_EXISTS,
        "mailbox catalog selector is ambiguous for account '%s'",
        request->account_identity);
    return FALSE;
  }

  row = g_ptr_array_index (rows, 0);
  return wyrebox_daemon_mailbox_select_result_init (out_result,
      row->kind,
      row->mailbox_id,
      row->mailbox_name,
      row->uid_validity, row->uid_next, row->message_count, error);
}

WyreboxDaemonMailboxListService *
wyrebox_daemon_mailbox_catalog_duckdb_new_list_service (const char
    *catalog_path, GError **error)
{
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (catalog_path, error);
  if (catalog == NULL)
    return NULL;

  return wyrebox_daemon_mailbox_list_service_new
      (wyrebox_daemon_mailbox_catalog_duckdb_list,
      g_steal_pointer (&catalog),
      (GDestroyNotify) wyrebox_daemon_mailbox_catalog_duckdb_free);
}

WyreboxDaemonMailboxSelectService *
wyrebox_daemon_mailbox_catalog_duckdb_new_select_service (const char
    *catalog_path, GError **error)
{
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (catalog_path, error);
  if (catalog == NULL)
    return NULL;

  return wyrebox_daemon_mailbox_select_service_new
      (wyrebox_daemon_mailbox_catalog_duckdb_select,
      g_steal_pointer (&catalog),
      (GDestroyNotify) wyrebox_daemon_mailbox_catalog_duckdb_free);
}
