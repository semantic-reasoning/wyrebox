#include "wyrebox-delivery-fetcher.h"

#include <duckdb.h>
#include <gio/gio.h>

struct _WyreboxDeliveryFetcher
{
  GObject parent_instance;

  gchar *catalog_path;
  WyreboxLocalObjectStore *object_store;
  duckdb_database database;
  duckdb_connection connection;
};

G_DEFINE_TYPE (WyreboxDeliveryFetcher, wyrebox_delivery_fetcher, G_TYPE_OBJECT);

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

static gboolean
fetcher_prepare (WyreboxDeliveryFetcher *self, const gchar *sql,
    duckdb_prepared_statement *out_statement, GError **error)
{
  if (duckdb_prepare (self->connection, sql, out_statement) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB delivery fetcher prepare failed: %s",
      *out_statement != NULL ?
      duckdb_prepare_error (*out_statement) : "unknown DuckDB error");
  return FALSE;
}

static gboolean
fetcher_bind_varchar (duckdb_prepared_statement statement, idx_t index,
    const gchar *value, GError **error)
{
  if (value == NULL || *value == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "DuckDB delivery fetcher string parameter %" G_GUINT64_FORMAT
        " is required", (guint64) index);
    return FALSE;
  }

  if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB delivery fetcher string bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
fetcher_bind_uint64 (duckdb_prepared_statement statement, idx_t index,
    guint64 value, GError **error)
{
  if (duckdb_bind_uint64 (statement, index, (uint64_t) value) == DuckDBSuccess)
    return TRUE;

  g_set_error (error,
      G_IO_ERROR,
      G_IO_ERROR_FAILED,
      "DuckDB delivery fetcher uint64 bind failed at index %" G_GUINT64_FORMAT,
      (guint64) index);
  return FALSE;
}

static gboolean
fetcher_namespace_kind_to_string (WyreboxDeliveryFetcherNamespaceKind kind,
    const gchar **out_kind, GError **error)
{
  switch (kind) {
    case WYREBOX_DELIVERY_FETCHER_NAMESPACE_MAILBOX:
      *out_kind = "mailbox";
      return TRUE;
    case WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW:
      *out_kind = "derived_view";
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "delivery fetcher namespace kind is unknown");
      return FALSE;
  }
}

static gboolean
fetcher_select_uidvalidity (WyreboxDeliveryFetcher *self,
    const gchar *account_id, const gchar *namespace_kind,
    const gchar *namespace_id, guint64 requested_uidvalidity, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  guint64 stored_uidvalidity = 0;

  if (!fetcher_prepare (self,
          "SELECT uidvalidity FROM mailbox_uid_state "
          "WHERE account_id = ? AND namespace_kind = ? "
          "AND namespace_id = ?;",
          &statement, error) ||
      !fetcher_bind_varchar (statement, 1, account_id, error) ||
      !fetcher_bind_varchar (statement, 2, namespace_kind, error) ||
      !fetcher_bind_varchar (statement, 3, namespace_id, error))
    return FALSE;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery fetcher UIDVALIDITY query failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
  }

  if (duckdb_row_count (&result) == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "namespace UID state is missing for %s/%s/%s",
        account_id, namespace_kind, namespace_id);
    return FALSE;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 1 ||
      duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "namespace UID state is malformed for %s/%s/%s",
        account_id, namespace_kind, namespace_id);
    return FALSE;
  }

  stored_uidvalidity = (guint64) duckdb_value_uint64 (&result, 0, 0);
  if (stored_uidvalidity != requested_uidvalidity) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "namespace UIDVALIDITY mismatch for %s/%s/%s",
        account_id, namespace_kind, namespace_id);
    return FALSE;
  }

  return TRUE;
}

static gchar *
fetcher_select_mailbox_object_id (WyreboxDeliveryFetcher *self,
    const gchar *account_id, const gchar *mailbox_id, guint64 uid,
    GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  const char *query =
      "SELECT m.object_id "
      "FROM mailbox_memberships mm "
      "JOIN mailboxes mb ON mb.account_id = mm.account_id "
      "AND mb.mailbox_id = mm.mailbox_id "
      "JOIN messages m ON m.account_id = mm.account_id "
      "AND m.message_id = mm.message_id "
      "JOIN objects o ON o.object_id = m.object_id "
      "WHERE mm.account_id = ? AND mm.mailbox_id = ? AND mm.uid = ? "
      "AND mm.is_visible = TRUE AND mb.is_visible = TRUE "
      "AND mb.is_selectable = TRUE;";

  if (!fetcher_prepare (self, query, &statement, error) ||
      !fetcher_bind_varchar (statement, 1, account_id, error) ||
      !fetcher_bind_varchar (statement, 2, mailbox_id, error) ||
      !fetcher_bind_uint64 (statement, 3, uid, error))
    return NULL;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery fetcher object query failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return NULL;
  }

  if (duckdb_row_count (&result) == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "visible mailbox UID was not found for %s/%s/%" G_GUINT64_FORMAT,
        account_id, mailbox_id, uid);
    return NULL;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 1 ||
      duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "visible mailbox UID resolved to malformed object state");
    return NULL;
  }

  {
    char *value = duckdb_value_varchar (&result, 0, 0);
    gchar *copy = g_strdup (value);

    duckdb_free (value);
    return copy;
  }
}

static gchar *
fetcher_select_derived_view_object_id (WyreboxDeliveryFetcher *self,
    const gchar *account_id, const gchar *view_id, guint64 uid, GError **error)
{
  g_auto (duckdb_prepared_statement) statement = NULL;
  g_auto (duckdb_result) result = { 0 };
  const char *query =
      "SELECT m.object_id "
      "FROM derived_view_memberships dvm "
      "JOIN derived_views dv ON dv.account_id = dvm.account_id "
      "AND dv.view_id = dvm.view_id "
      "JOIN messages m ON m.account_id = dvm.account_id "
      "AND m.message_id = dvm.message_id "
      "JOIN objects o ON o.object_id = m.object_id "
      "WHERE dvm.account_id = ? AND dvm.view_id = ? AND dvm.uid = ? "
      "AND dvm.is_visible = TRUE AND dv.is_visible = TRUE "
      "AND dv.is_selectable = TRUE;";

  if (!fetcher_prepare (self, query, &statement, error) ||
      !fetcher_bind_varchar (statement, 1, account_id, error) ||
      !fetcher_bind_varchar (statement, 2, view_id, error) ||
      !fetcher_bind_uint64 (statement, 3, uid, error))
    return NULL;

  if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery fetcher derived view object query failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return NULL;
  }

  if (duckdb_row_count (&result) == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "visible derived view UID was not found for %s/%s/%" G_GUINT64_FORMAT,
        account_id, view_id, uid);
    return NULL;
  }

  if (duckdb_row_count (&result) != 1 || duckdb_column_count (&result) != 1 ||
      duckdb_value_is_null (&result, 0, 0)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "visible derived view UID resolved to malformed object state");
    return NULL;
  }

  {
    char *value = duckdb_value_varchar (&result, 0, 0);
    gchar *copy = g_strdup (value);

    duckdb_free (value);
    return copy;
  }
}

static gchar *
fetcher_select_object_id (WyreboxDeliveryFetcher *self,
    WyreboxDeliveryFetcherNamespaceKind namespace_kind,
    const gchar *account_id, const gchar *namespace_id, guint64 uid,
    GError **error)
{
  switch (namespace_kind) {
    case WYREBOX_DELIVERY_FETCHER_NAMESPACE_MAILBOX:
      return fetcher_select_mailbox_object_id (self, account_id, namespace_id,
          uid, error);
    case WYREBOX_DELIVERY_FETCHER_NAMESPACE_DERIVED_VIEW:
      return fetcher_select_derived_view_object_id (self, account_id,
          namespace_id, uid, error);
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "delivery fetcher namespace kind is unknown");
      return NULL;
  }
}

static void
wyrebox_delivery_fetcher_finalize (GObject *object)
{
  WyreboxDeliveryFetcher *self = WYREBOX_DELIVERY_FETCHER (object);

  if (self->connection != NULL)
    duckdb_disconnect (&self->connection);
  if (self->database != NULL)
    duckdb_close (&self->database);
  g_clear_object (&self->object_store);
  g_clear_pointer (&self->catalog_path, g_free);

  G_OBJECT_CLASS (wyrebox_delivery_fetcher_parent_class)->finalize (object);
}

static void
wyrebox_delivery_fetcher_class_init (WyreboxDeliveryFetcherClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_delivery_fetcher_finalize;
}

static void
wyrebox_delivery_fetcher_init (WyreboxDeliveryFetcher *self)
{
}

WyreboxDeliveryFetcher *
wyrebox_delivery_fetcher_new_duckdb (const gchar *catalog_path,
    WyreboxLocalObjectStore *object_store, GError **error)
{
  const gchar *effective_path = catalog_path;
  g_auto (duckdb_config) config = NULL;
  char *open_error = NULL;
  g_autoptr (WyreboxDeliveryFetcher) self = NULL;

  g_return_val_if_fail (catalog_path != NULL, NULL);
  g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (g_strcmp0 (catalog_path, ":memory:") == 0)
    effective_path = NULL;

  if (duckdb_create_config (&config) != DuckDBSuccess ||
      duckdb_set_config (config, "access_mode", "READ_ONLY") != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery fetcher read-only configuration failed");
    return NULL;
  }

  self = g_object_new (WYREBOX_TYPE_DELIVERY_FETCHER, NULL);
  self->catalog_path = g_strdup (catalog_path);
  self->object_store = g_object_ref (object_store);

  if (duckdb_open_ext (effective_path, &self->database, config,
          &open_error) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "DuckDB delivery fetcher read-only open failed: %s",
        open_error != NULL ? open_error : "unknown DuckDB error");
    if (open_error != NULL)
      duckdb_free (open_error);
    return NULL;
  }

  if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "DuckDB delivery fetcher connect failed");
    return NULL;
  }

  return g_steal_pointer (&self);
}

GBytes *
wyrebox_delivery_fetcher_fetch_bytes (WyreboxDeliveryFetcher *self,
    const gchar *account_id, const gchar *mailbox_id, guint64 uidvalidity,
    guint64 uid, GError **error)
{
  return wyrebox_delivery_fetcher_fetch_namespace_bytes (self,
      account_id, WYREBOX_DELIVERY_FETCHER_NAMESPACE_MAILBOX, mailbox_id,
      uidvalidity, uid, error);
}

GBytes *
wyrebox_delivery_fetcher_fetch_namespace_bytes (WyreboxDeliveryFetcher *self,
    const gchar *account_id, WyreboxDeliveryFetcherNamespaceKind namespace_kind,
    const gchar *namespace_id, guint64 uidvalidity, guint64 uid, GError **error)
{
  g_autofree gchar *object_id = NULL;
  const gchar *namespace_kind_string = NULL;

  g_return_val_if_fail (WYREBOX_IS_DELIVERY_FETCHER (self), NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (uidvalidity == 0 || uid == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "delivery fetcher requires nonzero UIDVALIDITY and UID");
    return NULL;
  }

  if (!fetcher_namespace_kind_to_string (namespace_kind,
          &namespace_kind_string, error))
    return NULL;

  if (!fetcher_select_uidvalidity (self, account_id, namespace_kind_string,
          namespace_id, uidvalidity, error))
    return NULL;

  object_id = fetcher_select_object_id (self, namespace_kind, account_id,
      namespace_id, uid, error);
  if (object_id == NULL)
    return NULL;

  return wyrebox_local_object_store_get_bytes (self->object_store, object_id,
      error);
}
