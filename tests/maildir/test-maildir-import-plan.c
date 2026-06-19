#include "wyrebox-maildir-import-plan.h"
#include "wyrebox-maildir-scanner.h"

#include <glib.h>
#include <glib/gstdio.h>

static void
assert_plan_entry (WyreboxMaildirImportPlanEntry *entry,
    WyreboxMaildirScanEntryKind kind, const char *mailbox_path,
    const char *source_path, const char *maildir_flag_suffix, guint flags)
{
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, kind);
  g_assert_cmpstr (entry->mailbox_path, ==, mailbox_path);
  g_assert_cmpstr (entry->source_path, ==, source_path);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==, maildir_flag_suffix);
  g_assert_cmpuint (entry->maildir_flags, ==, flags);
}

static void
assert_verification_failure (WyreboxMaildirImportPlanVerificationResult *result,
    WyreboxMaildirImportPlanVerificationStatus status, const char *failure_path)
{
  g_assert_nonnull (result);
  g_assert_false (result->ok);
  g_assert_cmpint (result->status, ==, status);
  g_assert_cmpstr (result->failure_path, ==, failure_path);
}

static gboolean
copy_tree_recursive (const char *source_root, const char *target_root,
    GError **error)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  g_assert_cmpint (g_mkdir_with_parents (target_root, 0700), ==, 0);

  dir = g_dir_open (source_root, 0, error);
  if (dir == NULL)
    return FALSE;

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree gchar *source_path = g_build_filename (source_root, name, NULL);
    g_autofree gchar *target_path = g_build_filename (target_root, name, NULL);

    if (g_file_test (source_path, G_FILE_TEST_IS_DIR)) {
      if (!copy_tree_recursive (source_path, target_path, error))
        return FALSE;
      continue;
    }

    g_autoptr (GFile) source_file = g_file_new_for_path (source_path);
    g_autoptr (GFile) target_file = g_file_new_for_path (target_path);

    if (!g_file_copy (source_file, target_file, G_FILE_COPY_OVERWRITE, NULL,
            NULL, NULL, error))
      return FALSE;
  }

  return TRUE;
}

static void
test_import_plan_dry_run_verifies_clean_fixture (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanVerificationResult) result = NULL;
  g_autoptr (GPtrArray) scan_entries = NULL;
  WyreboxMaildirImportPlanEntry *entry = NULL;

  g_assert_nonnull (fixture_root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  plan = wyrebox_maildir_import_plan_new_from_scan_entries (fixture_root,
      scan_entries, &error);
  g_assert_nonnull (plan);
  g_assert_no_error (error);
  g_assert_cmpuint (wyrebox_maildir_import_plan_get_mailbox_count (plan), ==,
      4);
  g_assert_cmpuint (wyrebox_maildir_import_plan_get_message_count (plan), ==,
      8);
  g_assert_cmpuint (wyrebox_maildir_import_plan_get_entries (plan)->len, ==,
      scan_entries->len);

  entry = g_ptr_array_index (wyrebox_maildir_import_plan_get_entries (plan), 3);
  assert_plan_entry (entry, WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "Projects",
      ".Projects", NULL, 0);
  g_assert_cmpuint (entry->size_bytes, ==, 0);
  g_assert_null (entry->sha256_digest);

  wyrebox_maildir_scan_entry_clear (g_ptr_array_index (scan_entries, 3));

  entry = g_ptr_array_index (wyrebox_maildir_import_plan_get_entries (plan), 3);
  assert_plan_entry (entry, WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "Projects",
      ".Projects", NULL, 0);
  g_assert_cmpuint (entry->size_bytes, ==, 0);
  g_assert_null (entry->sha256_digest);

  entry = g_ptr_array_index (wyrebox_maildir_import_plan_get_entries (plan), 4);
  g_assert_cmpuint (entry->size_bytes, >, 0);
  g_assert_nonnull (entry->sha256_digest);
  g_assert_cmpuint (strlen (entry->sha256_digest), ==, 64);

  result = wyrebox_maildir_import_plan_dry_run_verify_current (plan,
      fixture_root, &error);
  g_assert_nonnull (result);
  g_assert_true (result->ok);
  g_assert_cmpint (result->status, ==, WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_OK);
  g_assert_null (result->failure_path);
  g_assert_no_error (error);
}

static void
test_import_plan_dry_run_reports_digest_mismatch (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autofree gchar *message_path = NULL;
  g_autofree gchar *modified_root = NULL;
  g_autofree gchar *message_contents = NULL;
  g_autofree gchar *copy_root = NULL;
  gsize message_len = 0;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanVerificationResult) result = NULL;
  g_autoptr (GPtrArray) scan_entries = NULL;

  g_assert_nonnull (fixture_root);
  copy_root = g_dir_make_tmp ("wyrebox-maildir-import-plan-XXXXXX", NULL);
  g_assert_nonnull (copy_root);
  g_assert_true (copy_tree_recursive (fixture_root, copy_root, &error));
  g_assert_no_error (error);

  modified_root = copy_root;
  copy_root = NULL;
  message_path = g_build_filename (modified_root, ".Projects", "new",
      "project-new", NULL);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, modified_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  plan = wyrebox_maildir_import_plan_new_from_scan_entries (modified_root,
      scan_entries, &error);
  g_assert_nonnull (plan);
  g_assert_no_error (error);

  g_assert_true (wyrebox_maildir_import_plan_verify_current (plan,
          modified_root, &error));
  g_assert_no_error (error);

  g_assert_true (g_file_get_contents (message_path, &message_contents,
          &message_len, &error));
  g_assert_no_error (error);
  message_contents[0] = 'X';
  g_assert_true (g_file_set_contents (message_path, message_contents,
          message_len, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  result = wyrebox_maildir_import_plan_dry_run_verify_current (plan,
      modified_root, &error);
  g_assert_nonnull (result);
  assert_verification_failure (result,
      WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_DIGEST_MISMATCH,
      ".Projects/new/project-new");
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_import_plan_dry_run_reports_layout_drift (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autofree gchar *modified_root = NULL;
  g_autofree gchar *extra_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanVerificationResult) result = NULL;
  g_autoptr (GPtrArray) scan_entries = NULL;

  g_assert_nonnull (fixture_root);
  modified_root = g_dir_make_tmp ("wyrebox-maildir-import-plan-drift-XXXXXX",
      NULL);
  g_assert_nonnull (modified_root);
  g_assert_true (copy_tree_recursive (fixture_root, modified_root, &error));
  g_assert_no_error (error);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, modified_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  plan = wyrebox_maildir_import_plan_new_from_scan_entries (modified_root,
      scan_entries, &error);
  g_assert_nonnull (plan);
  g_assert_no_error (error);

  extra_path = g_build_filename (modified_root, ".Projects", "new",
      "extra-message", NULL);
  g_assert_true (g_file_set_contents (extra_path, "Subject: drift\r\n\r\nx\r\n",
          -1, &error));
  g_assert_no_error (error);

  g_clear_error (&error);
  result = wyrebox_maildir_import_plan_dry_run_verify_current (plan,
      modified_root, &error);
  g_assert_nonnull (result);
  assert_verification_failure (result,
      WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_LAYOUT_DRIFT,
      ".Projects/new/project-new");
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_import_plan_dry_run_rejects_empty_scan (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) scan_entries = g_ptr_array_new ();
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanVerificationResult) result = NULL;

  plan = wyrebox_maildir_import_plan_new_from_scan_entries ("empty-root",
      scan_entries, &error);
  g_assert_null (plan);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  g_clear_error (&error);
  plan = g_object_new (WYREBOX_TYPE_MAILDIR_IMPORT_PLAN, NULL);
  result = wyrebox_maildir_import_plan_dry_run_verify_current (plan,
      "empty-root", &error);
  g_assert_nonnull (result);
  assert_verification_failure (result,
      WYREBOX_MAILDIR_IMPORT_PLAN_VERIFY_EMPTY_PLAN, NULL);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/maildir/import-plan/dry-run-verifies-clean-fixture",
      test_import_plan_dry_run_verifies_clean_fixture);
  g_test_add_func ("/maildir/import-plan/dry-run-reports-digest-mismatch",
      test_import_plan_dry_run_reports_digest_mismatch);
  g_test_add_func ("/maildir/import-plan/dry-run-reports-layout-drift",
      test_import_plan_dry_run_reports_layout_drift);
  g_test_add_func ("/maildir/import-plan/dry-run-rejects-empty-scan",
      test_import_plan_dry_run_rejects_empty_scan);

  return g_test_run ();
}
