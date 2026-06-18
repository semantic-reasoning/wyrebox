#include "wyrebox-daemon-runtime.h"

#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-delivery-replay-validator.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

#include <duckdb.h>

#include <string.h>

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

static const gchar *
runtime_safe_prefix_stop_reason_to_string (WyreboxJournalSafePrefixStopReason
    reason)
{
  switch (reason) {
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EOF:
      return "eof";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_MISSING_SEGMENT:
      return "missing-segment";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_EMPTY_SEGMENT:
      return "empty-segment";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_HEADER:
      return "partial-header";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_PARTIAL_RECORD:
      return "partial-record";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_MAGIC:
      return "invalid-magic";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_HEADER_SIZE:
      return "invalid-header-size";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_VERSION:
      return "invalid-version";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SEQUENCE:
      return "invalid-sequence";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_INVALID_SIZE:
      return "invalid-size";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_ZERO_EVENT_TYPE_LENGTH:
      return "zero-event-type-length";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_UNKNOWN_EVENT_TYPE:
      return "unknown-event-type";
    case WYREBOX_JOURNAL_SAFE_PREFIX_STOP_CHECKSUM_MISMATCH:
      return "checksum-mismatch";
    default:
      return "unknown";
  }
}

static void
    runtime_delivery_storage_report_init
    (WyreboxDaemonDeliveryStorageValidationReport * report)
{
  memset (report, 0, sizeof (*report));
  report->status = WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_INVALID;
  report->failure_category =
      WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_REPLAY_VALIDATION_FAILED;
}

static void
    runtime_delivery_storage_report_apply_safe_prefix
    (WyreboxDaemonDeliveryStorageValidationReport * report,
    const WyreboxJournalSafePrefix * prefix)
{
  report->safe_end_offset = prefix->safe_end_offset;
  report->has_last_safe_sequence = prefix->has_last_safe_sequence;
  report->last_safe_sequence = prefix->last_safe_sequence;
  report->has_unsafe_offset = prefix->unsafe_suffix_found;
  report->unsafe_offset = prefix->unsafe_offset;
}

static WyreboxDaemonDeliveryStorageValidationFailureCategory
    runtime_delivery_storage_failure_category_for_replay_error
    (const GError * error)
{
  const char *message = NULL;

  if (error == NULL || error->message == NULL)
    return
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_REPLAY_VALIDATION_FAILED;

  message = error->message;
  if (strstr (message, "mismatched size") != NULL)
    return WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_SIZE_MISMATCH;

  if (strstr (message, "SHA-256 mismatch") != NULL ||
      strstr (message, "failed SHA-256 verification") != NULL)
    return WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_HASH_MISMATCH;

  if (strstr (message, "references unavailable raw object") != NULL)
    return WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_MISSING_OBJECT;

  return
      WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_REPLAY_VALIDATION_FAILED;
}

static gboolean
    runtime_scan_journal_safe_prefix_for_delivery_storage
    (WyreboxJournalReader * journal_reader,
    WyreboxDaemonDeliveryStorageValidationReport * report, GError ** error)
{
  WyreboxJournalSafePrefix prefix = { 0 };
  const gchar *stop_reason = NULL;

  if (!wyrebox_journal_reader_scan_safe_prefix (journal_reader, &prefix, error))
    return FALSE;

  runtime_delivery_storage_report_apply_safe_prefix (report, &prefix);
  if (!prefix.unsafe_suffix_found)
    return TRUE;

  report->status = WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_INVALID;
  report->failure_category =
      WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_UNSAFE_JOURNAL_SUFFIX;

  stop_reason = runtime_safe_prefix_stop_reason_to_string (prefix.stop_reason);
  if (prefix.has_last_safe_sequence) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "startup delivery storage validation failed: journal unsafe suffix "
        "found, stop reason %s, unsafe offset %" G_GUINT64_FORMAT
        ", safe end offset %" G_GUINT64_FORMAT ", last safe sequence %"
        G_GUINT64_FORMAT ", available size %" G_GUINT64_FORMAT
        ", required size %" G_GUINT64_FORMAT,
        stop_reason, prefix.unsafe_offset, prefix.safe_end_offset,
        prefix.last_safe_sequence, prefix.unsafe_available_size,
        prefix.unsafe_required_size);
  } else {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "startup delivery storage validation failed: journal unsafe suffix "
        "found, stop reason %s, unsafe offset %" G_GUINT64_FORMAT
        ", safe end offset %" G_GUINT64_FORMAT ", last safe sequence none, "
        "available size %" G_GUINT64_FORMAT ", required size %"
        G_GUINT64_FORMAT,
        stop_reason, prefix.unsafe_offset, prefix.safe_end_offset,
        prefix.unsafe_available_size, prefix.unsafe_required_size);
  }

  return FALSE;
}

gboolean
wyrebox_daemon_runtime_validate_delivery_storage_report (const char
    *journal_root_dir, const char *object_root_dir,
    WyreboxDaemonDeliveryStorageValidationReport *out_report, GError **error)
{
  g_autoptr (WyreboxJournalReader) journal_reader = NULL;
  g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
  g_autoptr (WyreboxDeliveryReplayValidator) validator = NULL;
  g_autoptr (GError) local_error = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (out_report != NULL, FALSE);

  runtime_delivery_storage_report_init (out_report);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    out_report->failure_category =
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_INVALID_ARGUMENT;
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "journal root directory is required");
    return FALSE;
  }

  if (object_root_dir == NULL || *object_root_dir == '\0') {
    out_report->failure_category =
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_INVALID_ARGUMENT;
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "object root directory is required");
    return FALSE;
  }

  journal_reader = wyrebox_journal_reader_new (journal_root_dir, error);
  if (journal_reader == NULL) {
    out_report->failure_category =
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_JOURNAL_UNAVAILABLE;
    return FALSE;
  }

  if (!runtime_scan_journal_safe_prefix_for_delivery_storage (journal_reader,
          out_report, error)) {
    if (out_report->failure_category ==
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_REPLAY_VALIDATION_FAILED)
    {
      out_report->failure_category =
          WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_JOURNAL_UNAVAILABLE;
    }
    return FALSE;
  }

  object_store = wyrebox_local_object_store_open_existing (object_root_dir,
      error);
  if (object_store == NULL) {
    out_report->failure_category =
        WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_OBJECT_STORE_UNAVAILABLE;
    return FALSE;
  }

  validator = wyrebox_delivery_replay_validator_new (journal_reader,
      object_store);
  if (validator == NULL)
    return FALSE;

  if (!wyrebox_delivery_replay_validator_validate_all (validator, &local_error)) {
    out_report->failure_category =
        runtime_delivery_storage_failure_category_for_replay_error
        (local_error);
    g_propagate_prefixed_error (error, g_steal_pointer (&local_error),
        "startup delivery storage validation failed: ");
    return FALSE;
  }

  out_report->status = WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_VALID;
  out_report->failure_category =
      WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_NONE;
  return TRUE;
}

gboolean
wyrebox_daemon_runtime_validate_delivery_storage (const char *journal_root_dir,
    const char *object_root_dir, GError **error)
{
  WyreboxDaemonDeliveryStorageValidationReport report = { 0 };

  return wyrebox_daemon_runtime_validate_delivery_storage_report
      (journal_root_dir, object_root_dir, &report, error);
}

gboolean
wyrebox_daemon_runtime_prepare_catalog (const char *journal_root_dir,
    const char *catalog_path,
    gboolean checkpoint_precondition_satisfied, GError **error)
{
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxJournalReader) journal_reader = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (journal_root_dir == NULL || *journal_root_dir == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "journal root directory is required");
    return FALSE;
  }

  if (catalog_path == NULL || *catalog_path == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "DuckDB catalog path is required");
    return FALSE;
  }

  store = wyrebox_schema_metadata_store_new_duckdb (catalog_path, error);
  if (store == NULL)
    return FALSE;

  journal_reader = wyrebox_journal_reader_new (journal_root_dir, error);
  if (journal_reader == NULL)
    return FALSE;

  migration = wyrebox_schema_migration_new ();
  return wyrebox_schema_migration_run_store_to_current_with_journal (migration,
      store, journal_reader, checkpoint_precondition_satisfied, error);
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
