#include "wyrebox-maildir-scanner.h"

#include <glib.h>
#include <glib/gstdio.h>

static void
assert_entry (WyreboxMaildirScanEntry *entry, WyreboxMaildirScanEntryKind kind,
    const char *mailbox_path, const char *source_path,
    const char *maildir_flag_suffix, guint maildir_flags)
{
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, kind);
  g_assert_cmpstr (entry->mailbox_path, ==, mailbox_path);
  g_assert_cmpstr (entry->source_path, ==, source_path);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==, maildir_flag_suffix);
  g_assert_cmpuint (entry->maildir_flags, ==, maildir_flags);
}

static WyreboxMaildirScanEntry *
find_entry_by_source_path (GPtrArray *entries, const char *source_path)
{
  for (guint index = 0; index < entries->len; index++) {
    WyreboxMaildirScanEntry *entry = g_ptr_array_index (entries, index);

    if (g_strcmp0 (entry->source_path, source_path) == 0)
      return entry;
  }

  return NULL;
}

static void
assert_plan_matches_fixture (GPtrArray *entries)
{
  g_assert_cmpuint (entries->len, ==, 12);

  assert_entry (g_ptr_array_index (entries, 0),
      WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "", ".", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 1),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "", "cur/inbox-old:2,S", "S",
      WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN);
  assert_entry (g_ptr_array_index (entries, 2),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "", "new/inbox-new:2,RS", "RS",
      WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN | WYREBOX_MAILDIR_MESSAGE_FLAG_REPLIED);
  assert_entry (g_ptr_array_index (entries, 3),
      WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "Projects", ".Projects", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 4),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Projects",
      ".Projects/cur/project-old:2,FS", "FS",
      WYREBOX_MAILDIR_MESSAGE_FLAG_FLAGGED | WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN);
  assert_entry (g_ptr_array_index (entries, 5),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Projects",
      ".Projects/new/project-new", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 6),
      WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "Projects/Archive",
      ".Projects/.Archive", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 7),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Projects/Archive",
      ".Projects/.Archive/cur/archive-old", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 8),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Projects/Archive",
      ".Projects/.Archive/new/archive-new", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 9),
      WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX, "Team/Support", ".Team.Support",
      NULL, 0);
  assert_entry (g_ptr_array_index (entries, 10),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Team/Support",
      ".Team.Support/cur/team-old", NULL, 0);
  assert_entry (g_ptr_array_index (entries, 11),
      WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE, "Team/Support",
      ".Team.Support/new/team-new", NULL, 0);
}

static void
test_scanner_parses_maildir_filename_flags (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  WyreboxMaildirScanEntry *entry = NULL;

  g_assert_nonnull (fixture_root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root, &entries,
          &error));
  g_assert_no_error (error);

  entry = find_entry_by_source_path (entries, "cur/inbox-old:2,S");
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==, "S");
  g_assert_cmpuint (entry->maildir_flags, ==,
      WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN);

  entry = find_entry_by_source_path (entries, "new/inbox-new:2,RS");
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==, "RS");
  g_assert_cmpuint (entry->maildir_flags, ==,
      WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN | WYREBOX_MAILDIR_MESSAGE_FLAG_REPLIED);

  entry = find_entry_by_source_path (entries, ".Projects/cur/project-old:2,FS");
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->maildir_flag_suffix, ==, "FS");
  g_assert_cmpuint (entry->maildir_flags, ==,
      WYREBOX_MAILDIR_MESSAGE_FLAG_FLAGGED | WYREBOX_MAILDIR_MESSAGE_FLAG_SEEN);

  entry = find_entry_by_source_path (entries, ".Projects/new/project-new");
  g_assert_nonnull (entry);
  g_assert_null (entry->maildir_flag_suffix);
  g_assert_cmpuint (entry->maildir_flags, ==, 0);

  entry = find_entry_by_source_path (entries, ".");
  g_assert_nonnull (entry);
  g_assert_null (entry->maildir_flag_suffix);
  g_assert_cmpuint (entry->maildir_flags, ==, 0);
}

static void
test_scanner_produces_deterministic_plan (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) first = NULL;
  g_autoptr (GPtrArray) second = NULL;

  g_assert_nonnull (fixture_root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_nonnull (scanner);

  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root, &first,
          &error));
  g_assert_no_error (error);
  assert_plan_matches_fixture (first);

  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root, &second,
          &error));
  g_assert_no_error (error);
  assert_plan_matches_fixture (second);
}

static void
test_scanner_ignores_tmp_and_noise (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  g_assert_nonnull (fixture_root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root, &entries,
          &error));
  g_assert_no_error (error);

  for (guint index = 0; index < entries->len; index++) {
    WyreboxMaildirScanEntry *entry = g_ptr_array_index (entries, index);

    g_assert_nonnull (entry);
    g_assert_null (g_strstr_len (entry->source_path, -1, "tmp/"));
    g_assert_null (g_strstr_len (entry->source_path, -1, "maildirfolder"));
    g_assert_null (g_strstr_len (entry->source_path, -1, "noise.txt"));
  }
}

static void
test_scanner_reports_missing_maildir_cleanly (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyrebox-maildir-empty-XXXXXX",
      NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  g_assert_nonnull (root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_false (wyrebox_maildir_scanner_scan (scanner, root, &entries,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (entries);
}

static void
test_scanner_rejects_missing_tmp (void)
{
  g_autofree char *root = g_dir_make_tmp ("wyrebox-maildir-missing-tmp-XXXXXX",
      NULL);
  g_autofree char *cur_dir = NULL;
  g_autofree char *new_dir = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  g_assert_nonnull (root);

  cur_dir = g_build_filename (root, "cur", NULL);
  new_dir = g_build_filename (root, "new", NULL);
  g_assert_cmpint (g_mkdir_with_parents (cur_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (new_dir, 0700), ==, 0);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_false (wyrebox_maildir_scanner_scan (scanner, root, &entries,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (entries);

  g_assert_cmpint (g_rmdir (new_dir), ==, 0);
  g_assert_cmpint (g_rmdir (cur_dir), ==, 0);
  g_assert_cmpint (g_rmdir (root), ==, 0);
}

static void
test_scanner_rejects_partial_maildir_amid_valid_mailbox (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-maildir-partial-amid-valid-XXXXXX", NULL);
  g_autofree char *cur_dir = NULL;
  g_autofree char *new_dir = NULL;
  g_autofree char *tmp_dir = NULL;
  g_autofree char *broken_dir = NULL;
  g_autofree char *broken_cur_dir = NULL;
  g_autofree char *broken_new_dir = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  g_assert_nonnull (root);

  cur_dir = g_build_filename (root, "cur", NULL);
  new_dir = g_build_filename (root, "new", NULL);
  tmp_dir = g_build_filename (root, "tmp", NULL);
  broken_dir = g_build_filename (root, ".Broken", NULL);
  broken_cur_dir = g_build_filename (broken_dir, "cur", NULL);
  broken_new_dir = g_build_filename (broken_dir, "new", NULL);

  g_assert_cmpint (g_mkdir_with_parents (cur_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (new_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (tmp_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (broken_cur_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (broken_new_dir, 0700), ==, 0);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_false (wyrebox_maildir_scanner_scan (scanner, root, &entries,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (entries);

  g_assert_cmpint (g_rmdir (broken_new_dir), ==, 0);
  g_assert_cmpint (g_rmdir (broken_cur_dir), ==, 0);
  g_assert_cmpint (g_rmdir (broken_dir), ==, 0);
  g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);
  g_assert_cmpint (g_rmdir (new_dir), ==, 0);
  g_assert_cmpint (g_rmdir (cur_dir), ==, 0);
  g_assert_cmpint (g_rmdir (root), ==, 0);
}

static void
test_scanner_rejects_cur_only_partial_maildir_amid_valid_mailbox (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-maildir-cur-only-partial-XXXXXX", NULL);
  g_autofree char *cur_dir = NULL;
  g_autofree char *new_dir = NULL;
  g_autofree char *tmp_dir = NULL;
  g_autofree char *broken_dir = NULL;
  g_autofree char *broken_cur_dir = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;

  g_assert_nonnull (root);

  cur_dir = g_build_filename (root, "cur", NULL);
  new_dir = g_build_filename (root, "new", NULL);
  tmp_dir = g_build_filename (root, "tmp", NULL);
  broken_dir = g_build_filename (root, ".BrokenCurOnly", NULL);
  broken_cur_dir = g_build_filename (broken_dir, "cur", NULL);

  g_assert_cmpint (g_mkdir_with_parents (cur_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (new_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (tmp_dir, 0700), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (broken_cur_dir, 0700), ==, 0);

  scanner = wyrebox_maildir_scanner_new ();
  g_assert_false (wyrebox_maildir_scanner_scan (scanner, root, &entries,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (entries);

  g_assert_cmpint (g_rmdir (broken_cur_dir), ==, 0);
  g_assert_cmpint (g_rmdir (broken_dir), ==, 0);
  g_assert_cmpint (g_rmdir (tmp_dir), ==, 0);
  g_assert_cmpint (g_rmdir (new_dir), ==, 0);
  g_assert_cmpint (g_rmdir (cur_dir), ==, 0);
  g_assert_cmpint (g_rmdir (root), ==, 0);
}

static void
test_scanner_distinguishes_same_content_entries (void)
{
  const char *fixture_root = g_getenv ("WYREBOX_MAILDIR_FIXTURE_DIR");
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxMaildirScanner) scanner = NULL;
  g_autoptr (GPtrArray) entries = NULL;
  gboolean saw_inbox_new = FALSE;
  gboolean saw_nested_new = FALSE;
  g_autofree gchar *project_new = NULL;
  g_autofree gchar *archive_new = NULL;
  g_autofree gchar *project_new_contents = NULL;
  g_autofree gchar *archive_new_contents = NULL;
  gsize project_new_len = 0;
  gsize archive_new_len = 0;

  g_assert_nonnull (fixture_root);
  scanner = wyrebox_maildir_scanner_new ();
  g_assert_true (wyrebox_maildir_scanner_scan (scanner, fixture_root, &entries,
          &error));
  g_assert_no_error (error);

  for (guint index = 0; index < entries->len; index++) {
    WyreboxMaildirScanEntry *entry = g_ptr_array_index (entries, index);

    if (g_strcmp0 (entry->source_path, "new/inbox-new:2,RS") == 0)
      saw_inbox_new = TRUE;
    if (g_strcmp0 (entry->source_path, ".Projects/new/project-new") == 0)
      saw_nested_new = TRUE;
  }

  g_assert_true (saw_inbox_new);
  g_assert_true (saw_nested_new);

  project_new = g_build_filename (fixture_root, ".Projects", "new",
      "project-new", NULL);
  archive_new = g_build_filename (fixture_root, ".Projects", ".Archive", "new",
      "archive-new", NULL);
  g_assert_true (g_file_get_contents (project_new, &project_new_contents,
          &project_new_len, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_get_contents (archive_new, &archive_new_contents,
          &archive_new_len, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (project_new_len, ==, archive_new_len);
  g_assert_cmpmem (project_new_contents, project_new_len,
      archive_new_contents, archive_new_len);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/maildir/scanner/deterministic-plan",
      test_scanner_produces_deterministic_plan);
  g_test_add_func ("/maildir/scanner/parses-maildir-filename-flags",
      test_scanner_parses_maildir_filename_flags);
  g_test_add_func ("/maildir/scanner/ignores-tmp-and-noise",
      test_scanner_ignores_tmp_and_noise);
  g_test_add_func ("/maildir/scanner/missing-maildir-cleanly",
      test_scanner_reports_missing_maildir_cleanly);
  g_test_add_func ("/maildir/scanner/rejects-missing-tmp",
      test_scanner_rejects_missing_tmp);
  g_test_add_func
      ("/maildir/scanner/rejects-partial-maildir-amid-valid-mailbox",
      test_scanner_rejects_partial_maildir_amid_valid_mailbox);
  g_test_add_func
      ("/maildir/scanner/rejects-cur-only-partial-maildir-amid-valid-mailbox",
      test_scanner_rejects_cur_only_partial_maildir_amid_valid_mailbox);
  g_test_add_func ("/maildir/scanner/distinguishes-same-content-entries",
      test_scanner_distinguishes_same_content_entries);

  return g_test_run ();
}
