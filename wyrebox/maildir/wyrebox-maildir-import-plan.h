#pragma once

#include "wyrebox-eml-ingestor.h"
#include "wyrebox-maildir-scanner.h"

#include <gio/gio.h>
#include <glib-object.h>

/* *INDENT-OFF* */
G_BEGIN_DECLS

typedef struct
{
  gchar *mailbox_path;
  gchar *source_path;
  gchar *maildir_flag_suffix;
  guint maildir_flags;
  guint64 size_bytes;
  gchar *sha256_digest;
  WyreboxMaildirScanEntryKind kind;
} WyreboxMaildirImportPlanEntry;

void wyrebox_maildir_import_plan_entry_clear (
    WyreboxMaildirImportPlanEntry *entry);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMaildirImportPlanEntry,
    wyrebox_maildir_import_plan_entry_clear)

typedef enum
{
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_UNKNOWN = 0,
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_OK,
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_EMPTY_PLAN,
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_LAYOUT_DRIFT,
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_SIZE_MISMATCH,
  WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_DIGEST_MISMATCH,
} WyreboxMaildirImportPlanVerificationStatus;

typedef struct
{
  gboolean ok;
  WyreboxMaildirImportPlanVerificationStatus status;
  gchar *failure_path;
} WyreboxMaildirImportPlanVerificationResult;

void wyrebox_maildir_import_plan_verification_result_clear (
    WyreboxMaildirImportPlanVerificationResult *result);

void wyrebox_maildir_import_plan_verification_result_free (
    WyreboxMaildirImportPlanVerificationResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (
    WyreboxMaildirImportPlanVerificationResult,
    wyrebox_maildir_import_plan_verification_result_clear)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (
    WyreboxMaildirImportPlanVerificationResult,
    wyrebox_maildir_import_plan_verification_result_free)

typedef struct
{
  gchar *mailbox_path;
  gchar *source_path;
  gchar *maildir_flag_suffix;
  guint maildir_flags;
  WyreboxEmlIngestResult ingest_result;
} WyreboxMaildirImportPlanExecutionEntry;

void wyrebox_maildir_import_plan_execution_entry_clear (
    WyreboxMaildirImportPlanExecutionEntry *entry);

void wyrebox_maildir_import_plan_execution_entry_free (
    WyreboxMaildirImportPlanExecutionEntry *entry);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMaildirImportPlanExecutionEntry,
    wyrebox_maildir_import_plan_execution_entry_clear)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WyreboxMaildirImportPlanExecutionEntry,
    wyrebox_maildir_import_plan_execution_entry_free)

typedef enum
{
  WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_UNKNOWN = 0,
  WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_OK,
  WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_REFUSED,
  WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_SOURCE_READ_FAILED,
  WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_INGEST_FAILED,
} WyreboxMaildirImportPlanExecutionStatus;

typedef struct
{
  gboolean ok;
  WyreboxMaildirImportPlanExecutionStatus status;
  gchar *failure_path;
  GPtrArray *entries;
} WyreboxMaildirImportPlanExecutionResult;

void wyrebox_maildir_import_plan_execution_result_clear (
    WyreboxMaildirImportPlanExecutionResult *result);

void wyrebox_maildir_import_plan_execution_result_free (
    WyreboxMaildirImportPlanExecutionResult *result);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxMaildirImportPlanExecutionResult,
    wyrebox_maildir_import_plan_execution_result_clear)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (WyreboxMaildirImportPlanExecutionResult,
    wyrebox_maildir_import_plan_execution_result_free)

#define WYREBOX_TYPE_MAILDIR_IMPORT_PLAN \
  (wyrebox_maildir_import_plan_get_type ())

G_DECLARE_FINAL_TYPE (WyreboxMaildirImportPlan,
    wyrebox_maildir_import_plan,
    WYREBOX,
    MAILDIR_IMPORT_PLAN,
    GObject)

/*
 * @root_path: (type filename): Maildir tree root that produced
 *   @scan_entries.
 * @scan_entries: (transfer none): ordered scan output from
 *   wyrebox_maildir_scanner_scan().
 *
 * Returns: (transfer full): a new import plan that owns a deep copy of the
 * scanned entries, their normalized Maildir flags, and per-message byte-size
 * and SHA-256 verification metadata.
 */
WyreboxMaildirImportPlan *wyrebox_maildir_import_plan_new_from_scan_entries (
    const gchar *root_path,
    GPtrArray *scan_entries,
    GError **error);

/*
 * Returns: (transfer none): ordered entries owned by the plan.
 */
GPtrArray *wyrebox_maildir_import_plan_get_entries (
    WyreboxMaildirImportPlan *self);

guint wyrebox_maildir_import_plan_get_mailbox_count (
    WyreboxMaildirImportPlan *self);

guint wyrebox_maildir_import_plan_get_message_count (
    WyreboxMaildirImportPlan *self);

/*
 * Verifies that the current contents under @root_path still match the plan's
 * recorded mailbox layout, message sizes, and SHA-256 digests.
 */
WyreboxMaildirImportPlanVerificationResult *
wyrebox_maildir_import_plan_dry_run_verify_current (
    WyreboxMaildirImportPlan *self,
    const gchar *root_path,
    GError **error);

/*
 * Verifies that the current contents under @root_path still match the plan's
 * recorded mailbox layout, message sizes, and SHA-256 digests.
 */
gboolean wyrebox_maildir_import_plan_verify_current (
    WyreboxMaildirImportPlan *self,
    const gchar *root_path,
    GError **error);

WyreboxMaildirImportPlanExecutionResult *
wyrebox_maildir_import_plan_execute (WyreboxMaildirImportPlan *self,
    const gchar *root_path,
    WyreboxEmlIngestor *ingestor,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
