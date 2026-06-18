#include "wyrebox-maildir-scanner.h"

#include <glib/gstdio.h>
#include <string.h>

struct _WyreboxMaildirScanner
{
  GObject parent_instance;
};

G_DEFINE_TYPE (WyreboxMaildirScanner, wyrebox_maildir_scanner, G_TYPE_OBJECT)
     static void
         wyrebox_maildir_scanner_class_init (WyreboxMaildirScannerClass *klass)
{
}

static void
wyrebox_maildir_scanner_init (WyreboxMaildirScanner *self)
{
}

void
wyrebox_maildir_scan_entry_clear (WyreboxMaildirScanEntry *entry)
{
  if (entry == NULL)
    return;

  g_clear_pointer (&entry->mailbox_path, g_free);
  g_clear_pointer (&entry->source_path, g_free);
  entry->kind = 0;
}

void
wyrebox_maildir_scan_entry_free (WyreboxMaildirScanEntry *entry)
{
  if (entry == NULL)
    return;

  wyrebox_maildir_scan_entry_clear (entry);
  g_free (entry);
}

static WyreboxMaildirScanner *
wyrebox_maildir_scanner_new_internal (void)
{
  return g_object_new (WYREBOX_TYPE_MAILDIR_SCANNER, NULL);
}

WyreboxMaildirScanner *
wyrebox_maildir_scanner_new (void)
{
  return wyrebox_maildir_scanner_new_internal ();
}

static void
scan_entry_clear_free (gpointer data)
{
  wyrebox_maildir_scan_entry_free (data);
}

static GPtrArray *
scan_entry_array_new (void)
{
  return g_ptr_array_new_with_free_func (scan_entry_clear_free);
}

static WyreboxMaildirScanEntry *
scan_entry_new (WyreboxMaildirScanEntryKind kind,
    gchar *mailbox_path, gchar *source_path)
{
  WyreboxMaildirScanEntry *entry = g_new0 (WyreboxMaildirScanEntry, 1);

  entry->kind = kind;
  entry->mailbox_path = mailbox_path != NULL ? mailbox_path : g_strdup ("");
  entry->source_path = source_path != NULL ? source_path : g_strdup ("");

  return entry;
}

static gint
compare_scan_entry (gconstpointer left, gconstpointer right)
{
  const WyreboxMaildirScanEntry *const *left_entry = left;
  const WyreboxMaildirScanEntry *const *right_entry = right;
  gint result = 0;

  result = g_strcmp0 ((*left_entry)->mailbox_path,
      (*right_entry)->mailbox_path);
  if (result != 0)
    return result;

  result = (gint) (*left_entry)->kind - (gint) (*right_entry)->kind;
  if (result != 0)
    return result;

  return g_strcmp0 ((*left_entry)->source_path, (*right_entry)->source_path);
}

static gint
compare_scan_string (gconstpointer left, gconstpointer right)
{
  const gchar *const *left_string = left;
  const gchar *const *right_string = right;

  return g_strcmp0 (*left_string, *right_string);
}

static gchar *
strip_leading_dot (const gchar *name)
{
  if (name == NULL)
    return g_strdup ("");

  return g_str_has_prefix (name, ".") ? g_strdup (name + 1) : g_strdup (name);
}

static gchar *
normalize_mailbox_path_from_relative_dir (const gchar *relative_dir)
{
  g_auto (GStrv) parts = NULL;
  g_autoptr (GPtrArray) normalized = NULL;

  if (relative_dir == NULL || relative_dir[0] == '\0')
    return g_strdup ("");

  parts = g_strsplit (relative_dir, G_DIR_SEPARATOR_S, -1);
  normalized = g_ptr_array_new_with_free_func (g_free);

  for (guint index = 0; parts[index] != NULL; index++) {
    g_auto (GStrv) dot_parts = NULL;
    g_autofree gchar *component = NULL;

    if (parts[index][0] == '\0')
      continue;

    if (g_strcmp0 (parts[index], "cur") == 0 ||
        g_strcmp0 (parts[index], "new") == 0 ||
        g_strcmp0 (parts[index], "tmp") == 0 ||
        g_strcmp0 (parts[index], "maildirfolder") == 0)
      continue;

    component = strip_leading_dot (parts[index]);
    if (component[0] == '\0')
      continue;

    dot_parts = g_strsplit (component, ".", -1);
    for (guint dot_index = 0; dot_parts[dot_index] != NULL; dot_index++) {
      if (dot_parts[dot_index][0] == '\0')
        continue;

      g_ptr_array_add (normalized, g_strdup (dot_parts[dot_index]));
    }
  }

  g_ptr_array_add (normalized, NULL);

  return g_strjoinv ("/", (gchar **) normalized->pdata);
}

static gchar *
build_relative_path (const gchar *left, const gchar *right)
{
  if (left == NULL || left[0] == '\0')
    return g_strdup (right);

  return g_build_filename (left, right, NULL);
}

static gchar *
build_source_path (const gchar *relative_dir, const gchar *child_name)
{
  if (relative_dir == NULL || relative_dir[0] == '\0')
    return g_build_filename (child_name, NULL);

  return g_build_filename (relative_dir, child_name, NULL);
}

static gboolean
directory_exists (const gchar *path)
{
  return g_file_test (path, G_FILE_TEST_IS_DIR);
}

static gboolean
maildir_root_has_full_layout (const gchar *dir_path)
{
  g_autofree gchar *cur_path = g_build_filename (dir_path, "cur", NULL);
  g_autofree gchar *new_path = g_build_filename (dir_path, "new", NULL);
  g_autofree gchar *tmp_path = g_build_filename (dir_path, "tmp", NULL);

  return directory_exists (cur_path) && directory_exists (new_path) &&
      directory_exists (tmp_path);
}

static gboolean
maildir_root_has_partial_layout (const gchar *dir_path)
{
  g_autofree gchar *cur_path = g_build_filename (dir_path, "cur", NULL);
  g_autofree gchar *new_path = g_build_filename (dir_path, "new", NULL);
  g_autofree gchar *tmp_path = g_build_filename (dir_path, "tmp", NULL);
  gboolean has_cur = directory_exists (cur_path);
  gboolean has_new = directory_exists (new_path);
  gboolean has_tmp = directory_exists (tmp_path);

  return (has_cur || has_new || has_tmp) && !(has_cur && has_new && has_tmp);
}

static gboolean
scan_message_dir (const gchar *root_path, const gchar *relative_dir,
    const gchar *message_dir_name, GPtrArray *entries, GError **error)
{
  g_autofree gchar *mailbox_dir_path = NULL;
  g_autofree gchar *message_dir_path = NULL;
  g_autoptr (GDir) dir = NULL;
  g_autoptr (GPtrArray) names = NULL;
  const gchar *name = NULL;

  mailbox_dir_path = build_relative_path (root_path, relative_dir);
  message_dir_path = g_build_filename (mailbox_dir_path, message_dir_name,
      NULL);

  dir = g_dir_open (message_dir_path, 0, error);
  if (dir == NULL)
    return FALSE;

  names = g_ptr_array_new_with_free_func (g_free);
  while ((name = g_dir_read_name (dir)) != NULL)
    g_ptr_array_add (names, g_strdup (name));

  g_ptr_array_sort (names, compare_scan_string);

  for (guint index = 0; index < names->len; index++) {
    g_autofree gchar *entry_path = NULL;
    g_autofree gchar *mailbox_path = NULL;
    g_autofree gchar *source_path = NULL;
    const gchar *entry_name = g_ptr_array_index (names, index);

    entry_path = g_build_filename (message_dir_path, entry_name, NULL);
    if (!g_file_test (entry_path, G_FILE_TEST_IS_REGULAR))
      continue;

    source_path = build_source_path (relative_dir, message_dir_name);
    g_autofree gchar *source_leaf = g_build_filename (source_path, entry_name,
        NULL);
    mailbox_path = normalize_mailbox_path_from_relative_dir (relative_dir);
    g_ptr_array_add (entries,
        scan_entry_new (WYREBOX_MAILDIR_SCAN_ENTRY_MESSAGE,
            g_steal_pointer (&mailbox_path), g_steal_pointer (&source_leaf)));
  }

  return TRUE;
}

static gboolean scan_directory_recursive (const gchar * root_path,
    const gchar * relative_dir, GPtrArray * entries, gboolean * out_found,
    GError ** error);

static gboolean
scan_maildir_root (const gchar *root_path, const gchar *relative_dir,
    GPtrArray *entries, gboolean *out_found, GError **error)
{
  g_autofree gchar *dir_path = NULL;
  g_autofree gchar *mailbox_path = NULL;
  gboolean has_maildir_children = FALSE;

  dir_path = build_relative_path (root_path, relative_dir);
  mailbox_path = normalize_mailbox_path_from_relative_dir (relative_dir);
  if (maildir_root_has_partial_layout (dir_path)) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA, "Maildir mailbox root is missing tmp/");
    return FALSE;
  }

  has_maildir_children = maildir_root_has_full_layout (dir_path);

  if (!has_maildir_children)
    return scan_directory_recursive (root_path, relative_dir, entries,
        out_found, error);

  *out_found = TRUE;
  g_ptr_array_add (entries, scan_entry_new (WYREBOX_MAILDIR_SCAN_ENTRY_MAILBOX,
          g_steal_pointer (&mailbox_path),
          relative_dir == NULL || relative_dir[0] == '\0' ? g_strdup (".") :
          g_strdup (relative_dir)));

  if (!scan_message_dir (root_path, relative_dir, "cur", entries, error))
    return FALSE;

  if (!scan_message_dir (root_path, relative_dir, "new", entries, error))
    return FALSE;

  return scan_directory_recursive (root_path, relative_dir, entries,
      out_found, error);
}

static gboolean
scan_directory_recursive (const gchar *root_path, const gchar *relative_dir,
    GPtrArray *entries, gboolean *out_found, GError **error)
{
  g_autofree gchar *dir_path = NULL;
  g_autoptr (GDir) dir = NULL;
  g_autoptr (GPtrArray) child_dirs = NULL;
  const gchar *name = NULL;

  dir_path = build_relative_path (root_path, relative_dir);
  dir = g_dir_open (dir_path, 0, error);
  if (dir == NULL)
    return FALSE;

  child_dirs = g_ptr_array_new_with_free_func (g_free);
  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree gchar *child_path = NULL;

    if (g_strcmp0 (name, "cur") == 0 || g_strcmp0 (name, "new") == 0 ||
        g_strcmp0 (name, "tmp") == 0 || g_strcmp0 (name, "maildirfolder") == 0)
      continue;

    child_path = g_build_filename (dir_path, name, NULL);
    if (g_file_test (child_path, G_FILE_TEST_IS_DIR))
      g_ptr_array_add (child_dirs, g_strdup (name));
  }

  g_ptr_array_sort (child_dirs, compare_scan_string);

  for (guint index = 0; index < child_dirs->len; index++) {
    g_autofree gchar *child_relative = NULL;
    const gchar *child_name = g_ptr_array_index (child_dirs, index);

    child_relative = build_source_path (relative_dir, child_name);
    if (!scan_maildir_root (root_path, child_relative, entries, out_found,
            error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
sort_and_validate_plan (GPtrArray *entries, GError **error)
{
  if (entries->len == 0) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "no Maildir mailboxes were found");
    return FALSE;
  }

  g_ptr_array_sort (entries, compare_scan_entry);
  return TRUE;
}

gboolean
wyrebox_maildir_scanner_scan (WyreboxMaildirScanner *self,
    const gchar *root_path, GPtrArray **out_entries, GError **error)
{
  g_autoptr (GPtrArray) entries = NULL;
  gboolean found_mailbox = FALSE;

  g_return_val_if_fail (WYREBOX_IS_MAILDIR_SCANNER (self), FALSE);
  g_return_val_if_fail (root_path != NULL && root_path[0] != '\0', FALSE);
  g_return_val_if_fail (out_entries != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  *out_entries = NULL;
  entries = scan_entry_array_new ();

  if (!scan_maildir_root (root_path, NULL, entries, &found_mailbox, error))
    return FALSE;

  if (!found_mailbox) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "no Maildir mailboxes were found");
    return FALSE;
  }

  if (!sort_and_validate_plan (entries, error))
    return FALSE;

  *out_entries = g_steal_pointer (&entries);
  return TRUE;
}
