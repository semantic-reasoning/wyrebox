#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
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
assert_execution_entry (WyreboxMaildirImportPlanExecutionEntry *entry,
    WyreboxMaildirImportPlanEntry *plan_entry)
{
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->source_path, ==, plan_entry->source_path);
  g_assert_cmpstr (entry->mailbox_path, ==, plan_entry->mailbox_path);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==,
      plan_entry->maildir_flag_suffix);
  g_assert_cmpuint (entry->maildir_flags, ==, plan_entry->maildir_flags);
  g_assert_cmpuint (entry->ingest_result.size_bytes, ==,
      plan_entry->size_bytes);
}

static guint
count_directory_entries (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;
  guint count = 0;
  g_autoptr (GError) error = NULL;

  dir = g_dir_open (path, 0, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  while ((name = g_dir_read_name (dir)) != NULL) {
    (void) name;
    count++;
  }

  return count;
}

static guint
count_message_delivered_records (const char *journal_root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  guint count = 0;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  while (TRUE) {
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;

    if (!wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
      g_assert_no_error (error);
      g_assert_true (eof);
      return count;
    }

    if (record.event_type == WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED)
      count++;
  }
}

static void
assert_file_bytes_equal (const char *path, GBytes *expected)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *contents = NULL;
  gsize length = 0;
  const guint8 *expected_data = NULL;
  gsize expected_length = 0;

  g_assert_true (g_file_get_contents (path, &contents, &length, &error));
  g_assert_no_error (error);
  expected_data = g_bytes_get_data (expected, &expected_length);
  g_assert_cmpuint (length, ==, expected_length);
  g_assert_cmpmem (contents, length, expected_data, expected_length);
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

static char *
sanitize_message_id_component (const char *source_path)
{
  g_autofree gchar *sanitized = g_strdup (source_path);

  for (gchar * cursor = sanitized; *cursor != '\0'; cursor++) {
    if (!g_ascii_isalnum (*cursor) && *cursor != '.' && *cursor != '-')
      *cursor = '-';
  }

  return g_steal_pointer (&sanitized);
}

static gboolean
add_message_id_headers_to_maildir (const char *root_path, GPtrArray *entries,
    GError **error)
{
  for (guint index = 0; index < entries->len; index++) {
    WyreboxMaildirScanEntry *entry = g_ptr_array_index (entries, index);
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autofree gchar *rewritten = NULL;
    g_autofree gchar *component = NULL;
    gsize contents_len = 0;

    if (entry->kind != WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE)
      continue;

    path = g_build_filename (root_path, entry->source_path, NULL);
    if (!g_file_get_contents (path, &contents, &contents_len, error))
      return FALSE;

    component = sanitize_message_id_component (entry->source_path);
    {
      g_autoptr (GString) normalized = g_string_new (NULL);

      for (gsize offset = 0; offset < contents_len; offset++) {
        if (contents[offset] == '\n' &&
            (offset == 0 || contents[offset - 1] != '\r'))
          g_string_append (normalized, "\r\n");
        else
          g_string_append_c (normalized, contents[offset]);
      }

      rewritten = g_strdup_printf ("Message-ID: <%s@fixture.test>\r\n%s",
          component, normalized->str);
    }
    if (!g_file_set_contents (path, rewritten, -1, error))
      return FALSE;
  }

  return TRUE;
}

static void
test_import_plan_copies_deterministic_scan (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
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

  g_assert_true (wyrebox_maildir_import_plan_verify_current (plan,
          fixture_root, &error));
  g_assert_no_error (error);
}

static void
test_import_plan_verifies_modified_message_failure (void)
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
  g_assert_false (wyrebox_maildir_import_plan_verify_current (plan,
          modified_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_import_plan_rejects_layout_drift (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autofree gchar *modified_root = NULL;
  g_autofree gchar *extra_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
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
  g_assert_false (wyrebox_maildir_import_plan_verify_current (plan,
          modified_root, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_import_plan_rejects_empty_scan (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) scan_entries = g_ptr_array_new ();
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;

  plan = wyrebox_maildir_import_plan_new_from_scan_entries ("empty-root",
      scan_entries, &error);
  g_assert_null (plan);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_import_plan_executes_through_eml_ingestor (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autofree gchar *copy_root = NULL;
  g_autofree gchar *object_root = NULL;
  g_autofree gchar *journal_root = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanExecutionResult) result = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (GPtrArray) scan_entries = NULL;
  guint message_index = 0;
  guint64 previous_sequence = 0;

  g_assert_nonnull (fixture_root);
  copy_root = g_dir_make_tmp ("wyrebox-maildir-import-exec-XXXXXX", NULL);
  object_root = g_dir_make_tmp ("wyrebox-maildir-import-objects-XXXXXX", NULL);
  journal_root = g_dir_make_tmp ("wyrebox-maildir-import-journal-XXXXXX", NULL);
  g_assert_nonnull (copy_root);
  g_assert_nonnull (object_root);
  g_assert_nonnull (journal_root);

  g_assert_true (copy_tree_recursive (fixture_root, copy_root, &error));
  g_assert_no_error (error);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, copy_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  g_assert_true (add_message_id_headers_to_maildir (copy_root, scan_entries,
          &error));
  g_assert_no_error (error);

  g_clear_pointer (&scan_entries, g_ptr_array_unref);

  g_assert_true (wyrebox_maildir_scanner_scan (scanner, copy_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  plan = wyrebox_maildir_import_plan_new_from_scan_entries (copy_root,
      scan_entries, &error);
  g_assert_nonnull (plan);
  g_assert_no_error (error);
  g_assert_true (wyrebox_maildir_import_plan_verify_current (plan,
          copy_root, &error));
  g_assert_no_error (error);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  ingestor = wyrebox_eml_ingestor_new_with_journal (store, writer);
  g_assert_nonnull (ingestor);
  g_assert_true (wyrebox_eml_ingestor_has_journal_writer (ingestor));

  result = wyrebox_maildir_import_plan_execute (plan, copy_root, ingestor,
      &error);
  g_assert_nonnull (result);
  g_assert_true (result->ok);
  g_assert_cmpint (result->status, ==,
      WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_OK);
  g_assert_null (result->failure_path);
  g_assert_cmpuint (result->entries->len, ==,
      wyrebox_maildir_import_plan_get_message_count (plan));
  g_assert_no_error (error);

  for (guint index = 0; index < scan_entries->len; index++) {
    WyreboxMaildirImportPlanEntry *plan_entry = g_ptr_array_index
        (wyrebox_maildir_import_plan_get_entries (plan), index);

    if (plan_entry->kind != WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE)
      continue;

    {
      WyreboxMaildirImportPlanExecutionEntry *execution_entry =
          g_ptr_array_index (result->entries, message_index++);
      g_autoptr (GBytes) stored_bytes = NULL;
      g_autofree gchar *source_path = g_build_filename (copy_root,
          plan_entry->source_path, NULL);

      assert_execution_entry (execution_entry, plan_entry);
      g_assert_cmpuint (execution_entry->ingest_result.journal_sequence, >,
          previous_sequence);
      previous_sequence = execution_entry->ingest_result.journal_sequence;

      stored_bytes = wyrebox_local_object_store_get_bytes (store,
          execution_entry->ingest_result.object_key, &error);
      g_assert_no_error (error);
      g_assert_nonnull (stored_bytes);
      assert_file_bytes_equal (source_path, stored_bytes);
    }
  }

  g_assert_cmpuint (message_index, ==, result->entries->len);
  g_assert_cmpuint (count_message_delivered_records (journal_root), ==,
      result->entries->len);
}

static void
test_import_plan_refuses_without_journal_writer (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autofree gchar *copy_root = NULL;
  g_autofree gchar *object_root = NULL;
  g_autofree gchar *objects_dir = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (WyreboxMaildirImportPlan) plan = NULL;
  g_autoptr (WyreboxMaildirImportPlanExecutionResult) result = NULL;
  g_autoptr (WyreboxLocalObjectStore) store = NULL;
  g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
  g_autoptr (GPtrArray) scan_entries = NULL;

  g_assert_nonnull (fixture_root);
  copy_root = g_dir_make_tmp ("wyrebox-maildir-import-refuse-XXXXXX", NULL);
  object_root = g_dir_make_tmp ("wyrebox-maildir-import-refuse-objects-XXXXXX",
      NULL);
  g_assert_nonnull (copy_root);
  g_assert_nonnull (object_root);

  g_assert_true (copy_tree_recursive (fixture_root, copy_root, &error));
  g_assert_no_error (error);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, copy_root,
          &scan_entries, &error));
  g_assert_no_error (error);

  plan = wyrebox_maildir_import_plan_new_from_scan_entries (copy_root,
      scan_entries, &error);
  g_assert_nonnull (plan);
  g_assert_no_error (error);

  store = wyrebox_local_object_store_new (object_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  ingestor = wyrebox_eml_ingestor_new (store);
  g_assert_nonnull (ingestor);
  g_assert_false (wyrebox_eml_ingestor_has_journal_writer (ingestor));

  g_clear_error (&error);
  result = wyrebox_maildir_import_plan_execute (plan, copy_root, ingestor,
      &error);
  g_assert_nonnull (result);
  g_assert_false (result->ok);
  g_assert_cmpint (result->status, ==,
      WYREBOX_MAILDIR_IMPORT_PLAN_EXECUTION_REFUSED);
  g_assert_null (result->failure_path);
  g_assert_cmpuint (result->entries->len, ==, 0);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  objects_dir = g_build_filename (object_root, "objects", "sha256", NULL);
  g_assert_cmpuint (count_directory_entries (objects_dir), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/maildir/import-plan/copies-deterministic-scan",
      test_import_plan_copies_deterministic_scan);
  g_test_add_func ("/maildir/import-plan/verifies-modified-message-failure",
      test_import_plan_verifies_modified_message_failure);
  g_test_add_func ("/maildir/import-plan/rejects-layout-drift",
      test_import_plan_rejects_layout_drift);
  g_test_add_func ("/maildir/import-plan/rejects-empty-scan",
      test_import_plan_rejects_empty_scan);
  g_test_add_func ("/maildir/import-plan/executes-through-eml-ingestor",
      test_import_plan_executes_through_eml_ingestor);
  g_test_add_func ("/maildir/import-plan/refuses-without-journal-writer",
      test_import_plan_refuses_without_journal_writer);

  return g_test_run ();
}
