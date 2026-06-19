#include "wyrebox-maildir-import-plan.h"

#include <string.h>

struct _WyreboxMaildirImportPlan
{
  GObject parent_instance;
  gchar *root_path;
  GPtrArray *entries;
  guint mailbox_count;
  guint message_count;
};

G_DEFINE_TYPE (WyreboxMaildirImportPlan, wyrebox_maildir_import_plan,
    G_TYPE_OBJECT)
     void wyrebox_maildir_import_plan_entry_clear (WyreboxMaildirImportPlanEntry
    *entry)
{
  if (entry == NULL)
    return;

  g_clear_pointer (&entry->mailbox_path, g_free);
  g_clear_pointer (&entry->source_path, g_free);
  g_clear_pointer (&entry->maildir_flag_suffix, g_free);
  g_clear_pointer (&entry->sha256_digest, g_free);
  entry->maildir_flags = 0;
  entry->size_bytes = 0;
  entry->kind = 0;
}

void wyrebox_maildir_import_plan_verification_result_clear
    (WyreboxMaildirImportPlanVerificationResult * result)
{
  if (result == NULL)
    return;

  result->ok = FALSE;
  result->status = WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_UNKNOWN;
  g_clear_pointer (&result->failure_path, g_free);
}

void wyrebox_maildir_import_plan_verification_result_free
    (WyreboxMaildirImportPlanVerificationResult * result)
{
  if (result == NULL)
    return;

  wyrebox_maildir_import_plan_verification_result_clear (result);
  g_free (result);
}

static void
maildir_import_plan_entry_free (gpointer data)
{
  WyreboxMaildirImportPlanEntry *entry = data;

  if (entry == NULL)
    return;

  wyrebox_maildir_import_plan_entry_clear (entry);
  g_free (entry);
}

static void
wyrebox_maildir_import_plan_finalize (GObject *object)
{
  WyreboxMaildirImportPlan *self = WYREBOX_MAILDIR_IMPORT_PLAN (object);

  g_clear_pointer (&self->root_path, g_free);
  g_clear_pointer (&self->entries, g_ptr_array_unref);

  G_OBJECT_CLASS (wyrebox_maildir_import_plan_parent_class)->finalize (object);
}

static void
wyrebox_maildir_import_plan_class_init (WyreboxMaildirImportPlanClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = wyrebox_maildir_import_plan_finalize;
}

static void
wyrebox_maildir_import_plan_init (WyreboxMaildirImportPlan *self)
{
}

static WyreboxMaildirImportPlanEntry *
maildir_import_plan_entry_copy (const gchar *root_path,
    const WyreboxMaildirScanEntry *scan_entry, GError **error)
{
  WyreboxMaildirImportPlanEntry *entry = g_new0 (WyreboxMaildirImportPlanEntry,
      1);
  g_autofree gchar *digest = NULL;
  g_autofree gchar *absolute_path = NULL;
  g_autofree gchar *contents = NULL;
  gsize contents_len = 0;
  gboolean is_message = scan_entry->kind == WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE;

  entry->mailbox_path = g_strdup (scan_entry->mailbox_path);
  entry->source_path = g_strdup (scan_entry->source_path);
  entry->maildir_flag_suffix = g_strdup (scan_entry->maildir_flag_suffix);
  entry->maildir_flags = scan_entry->maildir_flags;
  entry->kind = scan_entry->kind;

  if (!is_message)
    return entry;

  absolute_path = g_build_filename (root_path, scan_entry->source_path, NULL);
  if (!g_file_get_contents (absolute_path, &contents, &contents_len, error)) {
    wyrebox_maildir_import_plan_entry_clear (entry);
    g_free (entry);
    return NULL;
  }

  entry->size_bytes = (guint64) contents_len;
  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
      (const guchar *) contents, contents_len);
  entry->sha256_digest = g_steal_pointer (&digest);
  return entry;
}

static gboolean
copy_scan_entries (WyreboxMaildirImportPlan *self, const gchar *root_path,
    GPtrArray *scan_entries, GError **error)
{
  g_return_val_if_fail (scan_entries != NULL, FALSE);
  g_return_val_if_fail (root_path != NULL && root_path[0] != '\0', FALSE);

  self->entries =
      g_ptr_array_new_with_free_func (maildir_import_plan_entry_free);
  self->root_path = g_strdup (root_path);
  for (guint index = 0; index < scan_entries->len; index++) {
    WyreboxMaildirScanEntry *scan_entry = g_ptr_array_index (scan_entries,
        index);
    WyreboxMaildirImportPlanEntry *entry = maildir_import_plan_entry_copy
        (root_path, scan_entry, error);

    if (entry == NULL)
      return FALSE;

    if (scan_entry->kind == WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX)
      self->mailbox_count++;
    else
      self->message_count++;

    g_ptr_array_add (self->entries, entry);
  }

  if (self->entries->len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "no Maildir scan entries were provided");
    return FALSE;
  }

  return TRUE;
}

WyreboxMaildirImportPlan *
wyrebox_maildir_import_plan_new_from_scan_entries (const gchar *root_path,
    GPtrArray *scan_entries, GError **error)
{
  g_autoptr (WyreboxMaildirImportPlan) self = NULL;

  g_return_val_if_fail (scan_entries != NULL, NULL);
  g_return_val_if_fail (root_path != NULL && root_path[0] != '\0', NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  self = g_object_new (WYREBOX_TYPE_MAILDIR_IMPORT_PLAN, NULL);
  if (!copy_scan_entries (self, root_path, scan_entries, error))
    return NULL;

  return g_steal_pointer (&self);
}

GPtrArray *
wyrebox_maildir_import_plan_get_entries (WyreboxMaildirImportPlan *self)
{
  g_return_val_if_fail (WYREBOX_IS_MAILDIR_IMPORT_PLAN (self), NULL);

  return self->entries;
}

guint
wyrebox_maildir_import_plan_get_mailbox_count (WyreboxMaildirImportPlan *self)
{
  g_return_val_if_fail (WYREBOX_IS_MAILDIR_IMPORT_PLAN (self), 0);

  return self->mailbox_count;
}

guint
wyrebox_maildir_import_plan_get_message_count (WyreboxMaildirImportPlan *self)
{
  g_return_val_if_fail (WYREBOX_IS_MAILDIR_IMPORT_PLAN (self), 0);

  return self->message_count;
}

static WyreboxMaildirImportPlanVerificationResult *
verification_result_new (WyreboxMaildirImportPlanVerificationStatus status,
    const gchar *failure_path)
{
  WyreboxMaildirImportPlanVerificationResult *result =
      g_new0 (WyreboxMaildirImportPlanVerificationResult, 1);

  result->ok = status == WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_OK;
  result->status = status;
  result->failure_path = g_strdup (failure_path);

  return result;
}

static gboolean
verify_scan_entry_against_plan (const gchar *root_path,
    const WyreboxMaildirScanEntry *scan_entry,
    const WyreboxMaildirImportPlanEntry *manifest_entry,
    WyreboxMaildirImportPlanVerificationResult **out_result, GError **error)
{
  g_autofree gchar *absolute_path = NULL;
  g_autofree gchar *contents = NULL;
  g_autofree gchar *digest = NULL;
  gsize contents_len = 0;

  if (scan_entry->kind != manifest_entry->kind ||
      g_strcmp0 (scan_entry->mailbox_path, manifest_entry->mailbox_path) != 0 ||
      g_strcmp0 (scan_entry->source_path, manifest_entry->source_path) != 0 ||
      g_strcmp0 (scan_entry->maildir_flag_suffix,
          manifest_entry->maildir_flag_suffix) != 0 ||
      scan_entry->maildir_flags != manifest_entry->maildir_flags) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Maildir layout drift detected at %s", manifest_entry->source_path);
    *out_result =
        verification_result_new
        (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_LAYOUT_DRIFT,
        manifest_entry->source_path);
    return FALSE;
  }

  if (manifest_entry->kind != WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE)
    return TRUE;

  absolute_path = g_build_filename (root_path, manifest_entry->source_path,
      NULL);
  if (!g_file_get_contents (absolute_path, &contents, &contents_len, error)) {
    *out_result =
        verification_result_new
        (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_LAYOUT_DRIFT,
        manifest_entry->source_path);
    return FALSE;
  }

  if ((guint64) contents_len != manifest_entry->size_bytes) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Maildir message size changed for %s", manifest_entry->source_path);
    *out_result =
        verification_result_new
        (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_SIZE_MISMATCH,
        manifest_entry->source_path);
    return FALSE;
  }

  digest = g_compute_checksum_for_data (G_CHECKSUM_SHA256,
      (const guchar *) contents, contents_len);
  if (g_strcmp0 (digest, manifest_entry->sha256_digest) != 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Maildir message digest changed for %s", manifest_entry->source_path);
    *out_result =
        verification_result_new
        (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_DIGEST_MISMATCH,
        manifest_entry->source_path);
    return FALSE;
  }

  return TRUE;
}

WyreboxMaildirImportPlanVerificationResult *
wyrebox_maildir_import_plan_dry_run_verify_current (WyreboxMaildirImportPlan
    *self, const gchar *root_path, GError **error)
{
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) current_entries = NULL;
  WyreboxMaildirImportPlanVerificationResult *result = NULL;

  g_return_val_if_fail (WYREBOX_IS_MAILDIR_IMPORT_PLAN (self), NULL);
  g_return_val_if_fail (root_path != NULL && root_path[0] != '\0', NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (self->entries == NULL || self->entries->len == 0) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Maildir import plan is empty");
    return
        verification_result_new (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_EMPTY_PLAN,
        NULL);
  }

  scanner = wyrebox_maildir_scanner_new ();
  if (!wyrebox_maildir_scanner_scan (scanner, root_path, &current_entries,
          error))
    return NULL;

  {
    guint matched_entries = MIN (self->entries->len, current_entries->len);

    for (guint index = 0; index < matched_entries; index++) {
      WyreboxMaildirScanEntry *scan_entry = g_ptr_array_index (current_entries,
          index);
      WyreboxMaildirImportPlanEntry *entry = g_ptr_array_index (self->entries,
          index);

      if (!verify_scan_entry_against_plan (root_path, scan_entry, entry,
              &result, error))
        return result;
    }

    if (current_entries->len != self->entries->len) {
      const gchar *failure_path = NULL;
      guint mismatch_index = matched_entries;

      if (mismatch_index < self->entries->len) {
        WyreboxMaildirImportPlanEntry *entry =
            g_ptr_array_index (self->entries, mismatch_index);
        failure_path = entry->source_path;
      } else if (mismatch_index < current_entries->len) {
        WyreboxMaildirScanEntry *scan_entry =
            g_ptr_array_index (current_entries, mismatch_index);
        failure_path = scan_entry->source_path;
      }

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
          "Maildir layout count changed");
      return
          verification_result_new
          (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_LAYOUT_DRIFT, failure_path);
    }
  }

  return verification_result_new (WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_OK, NULL);
}

gboolean
wyrebox_maildir_import_plan_verify_current (WyreboxMaildirImportPlan *self,
    const gchar *root_path, GError **error)
{
  g_autoptr (WyreboxMaildirImportPlanVerificationResult) result = NULL;

  result = wyrebox_maildir_import_plan_dry_run_verify_current (self,
      root_path, error);
  return result != NULL && result->ok;
}
