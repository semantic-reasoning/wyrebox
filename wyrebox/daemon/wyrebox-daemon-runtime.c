#include "wyrebox-daemon-runtime.h"

#include "wyrebox-daemon-fact-mutation-service.h"

#include <duckdb.h>

typedef char *duckdb_owned_string;

static void
duckdb_database_clear (duckdb_database *database)
{
  if (*database != NULL)
    duckdb_close (database);
}

static void
duckdb_connection_clear (duckdb_connection *connection)
{
  if (*connection != NULL)
    duckdb_disconnect (connection);
}

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

static void
duckdb_config_clear (duckdb_config *config)
{
  if (*config != NULL)
    duckdb_destroy_config (config);
}

static void
duckdb_owned_string_clear (char **value)
{
  if (*value != NULL)
    duckdb_free (*value);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_database, duckdb_database_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_connection, duckdb_connection_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_config, duckdb_config_clear)
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_owned_string, duckdb_owned_string_clear)
/* *INDENT-ON* */

const char *
wyrebox_daemon_runtime_get_default_runtime_dir (void)
{
  return WYREBOX_DAEMON_DEFAULT_RUNTIME_DIR;
}

const char *
wyrebox_daemon_runtime_get_default_socket_path (void)
{
  return WYREBOX_DAEMON_DEFAULT_SOCKET_PATH;
}

const char *
wyrebox_daemon_runtime_get_default_fact_dump_dir (void)
{
  return WYREBOX_DAEMON_DEFAULT_FACT_DUMP_DIR;
}

GFile *
wyrebox_daemon_runtime_get_default_fact_dump_file (void)
{
  return g_file_new_for_path (WYREBOX_DAEMON_DEFAULT_FACT_DUMP_DIR);
}

static gboolean
runtime_open_catalog_read_only (const char *catalog_path,
    duckdb_database *out_database, duckdb_connection *out_connection,
    GError **error)
{
  g_auto (duckdb_config) config = NULL;
  char *open_error = NULL;

  if (duckdb_create_config (&config) != DuckDBSuccess ||
      duckdb_set_config (config, "access_mode", "READ_ONLY") != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "failed to configure DuckDB catalog read-only open");
    return FALSE;
  }

  if (duckdb_open_ext (catalog_path, out_database, config, &open_error) !=
      DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "failed to open DuckDB catalog '%s': %s",
        catalog_path, open_error != NULL ? open_error : "unknown DuckDB error");
    if (open_error != NULL)
      duckdb_free (open_error);
    return FALSE;
  }

  if (duckdb_connect (*out_database, out_connection) != DuckDBSuccess) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "failed to connect to DuckDB catalog '%s'",
        catalog_path);
    return FALSE;
  }

  return TRUE;
}

static GPtrArray *
runtime_load_account_ids (const char *catalog_path, GError **error)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;
  g_auto (duckdb_result) result = { 0 };
  g_autoptr (GPtrArray) account_ids = NULL;

  if (!runtime_open_catalog_read_only (catalog_path, &database, &connection,
          error))
    return NULL;

  if (duckdb_query (connection,
          "SELECT account_id FROM accounts ORDER BY account_id ASC;",
          &result) != DuckDBSuccess) {
    const char *detail = duckdb_result_error (&result);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "failed to enumerate accounts from DuckDB catalog '%s': %s",
        catalog_path,
        detail != NULL && *detail != '\0' ? detail : "unknown DuckDB error");
    return NULL;
  }

  account_ids = g_ptr_array_new_with_free_func (g_free);

  for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
    g_auto (duckdb_owned_string) value = NULL;

    if (duckdb_value_is_null (&result, 0, row)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB catalog '%s' contains an account with NULL account_id",
          catalog_path);
      return NULL;
    }

    value = duckdb_value_varchar (&result, 0, row);
    if (value == NULL || *value == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "DuckDB catalog '%s' contains an account with empty account_id",
          catalog_path);
      return NULL;
    }

    g_ptr_array_add (account_ids, g_strdup (value));
  }

  return g_steal_pointer (&account_ids);
}

gboolean
    wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views
    (WyreboxDaemonFactMutationService * fact_mutation_service,
    const char *catalog_path, GError ** error)
{
  g_autoptr (GPtrArray) account_ids = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (fact_mutation_service == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact mutation service is required");
    return FALSE;
  }

  if (!WYREBOX_IS_DAEMON_FACT_MUTATION_SERVICE (fact_mutation_service)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "fact mutation service is invalid");
    return FALSE;
  }

  if (catalog_path == NULL || *catalog_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "DuckDB catalog path is required");
    return FALSE;
  }

  account_ids = runtime_load_account_ids (catalog_path, error);
  if (account_ids == NULL)
    return FALSE;

  for (guint i = 0; i < account_ids->len; i++) {
    const char *account_id = g_ptr_array_index (account_ids, i);
    g_autoptr (GError) catch_up_error = NULL;

    if (!wyrebox_daemon_fact_mutation_service_catch_up_wirelog_derived_view
        (fact_mutation_service, account_id, &catch_up_error)) {
      g_propagate_prefixed_error (error, g_steal_pointer (&catch_up_error),
          "failed to catch up Wirelog derived view for account '%s': ",
          account_id);
      return FALSE;
    }
  }

  return TRUE;
}
