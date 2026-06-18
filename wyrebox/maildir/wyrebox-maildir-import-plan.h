#pragma once

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
gboolean wyrebox_maildir_import_plan_verify_current (
    WyreboxMaildirImportPlan *self,
    const gchar *root_path,
    GError **error);

G_END_DECLS
/* *INDENT-ON* */
