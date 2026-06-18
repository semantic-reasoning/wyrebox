#pragma once

#include <gio/gio.h>
#include <glib.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

#define WYREBOX_DAEMON_DEFAULT_RUNTIME_DIR "/run/wyrebox"
#define WYREBOX_DAEMON_DEFAULT_SOCKET_PATH "/run/wyrebox/wyrebox.sock"
#define WYREBOX_DAEMON_DEFAULT_FACT_DUMP_DIR "/run/wyrebox/facts"

const char *wyrebox_daemon_runtime_get_default_runtime_dir (void);

const char *wyrebox_daemon_runtime_get_default_socket_path (void);

const char *wyrebox_daemon_runtime_get_default_fact_dump_dir (void);

/*
 * Returns: (transfer full): caller-owned runtime fact dump directory file.
 * Free with g_object_unref().
 */
GFile *wyrebox_daemon_runtime_get_default_fact_dump_file (void);

typedef enum {
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_VALID,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_INVALID,
} WyreboxDaemonDeliveryStorageValidationStatus;

typedef enum {
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_NONE,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_INVALID_ARGUMENT,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_JOURNAL_UNAVAILABLE,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_OBJECT_STORE_UNAVAILABLE,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_UNSAFE_JOURNAL_SUFFIX,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_MISSING_OBJECT,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_SIZE_MISMATCH,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_HASH_MISMATCH,
  WYREBOX_DAEMON_DELIVERY_STORAGE_VALIDATION_FAILURE_REPLAY_VALIDATION_FAILED,
} WyreboxDaemonDeliveryStorageValidationFailureCategory;

typedef struct {
  WyreboxDaemonDeliveryStorageValidationStatus status;
  WyreboxDaemonDeliveryStorageValidationFailureCategory failure_category;
  guint64 safe_end_offset;
  gboolean has_last_safe_sequence;
  guint64 last_safe_sequence;
  gboolean has_unsafe_offset;
  guint64 unsafe_offset;
} WyreboxDaemonDeliveryStorageValidationReport;

/*
 * Produces a structured, read-only report for the delivery journal and
 * immutable raw object store. The report is filled on both valid and invalid
 * storage outcomes. Returns TRUE only when storage is valid.
 */
gboolean wyrebox_daemon_runtime_validate_delivery_storage_report (
    const char *journal_root_dir,
    const char *object_root_dir,
    WyreboxDaemonDeliveryStorageValidationReport *out_report,
    GError **error);

/*
 * Validates the delivery journal and immutable raw object store before the
 * daemon serves requests.
 */
gboolean wyrebox_daemon_runtime_validate_delivery_storage (
    const char *journal_root_dir,
    const char *object_root_dir,
    GError **error);

/*
 * Prepare a DuckDB catalog for serving, validating the journal state against
 * any recorded materialization checkpoint before applying migrations.
 */
gboolean wyrebox_daemon_runtime_prepare_catalog (
    const char *journal_root_dir,
    const char *catalog_path,
    gboolean checkpoint_precondition_satisfied,
    GError **error);

typedef struct _WyreboxDaemonFactMutationService
    WyreboxDaemonFactMutationService;

gboolean wyrebox_daemon_runtime_catch_up_configured_wirelog_derived_views (
    WyreboxDaemonFactMutationService *fact_mutation_service,
    const char *catalog_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
