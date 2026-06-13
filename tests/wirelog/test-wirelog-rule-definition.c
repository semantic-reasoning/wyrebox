#include "wyrebox-wirelog-rule-definition.h"
#include "wyrebox-wirelog-rule-version.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

static const char *valid_rules =
    ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
    ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
    "show_in_virtual_folder(view_id, message_id) :- "
    "project_keyword(message_id, view_id).\n";

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((entry = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, entry, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static GFile *
write_temp_rule_file (const char *contents, gssize length, char **out_path)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GError) error = NULL;

  dir = g_dir_make_tmp ("wyrebox-wirelog-rule-definition-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "rules.wl", NULL);
  g_assert_true (g_file_set_contents (path, contents, length, &error));
  g_assert_no_error (error);

  if (out_path != NULL)
    *out_path = g_strdup (path);

  return g_file_new_for_path (path);
}

static void
remove_temp_rule_file (const char *path)
{
  g_autofree char *dir = NULL;

  dir = g_path_get_dirname (path);
  remove_tree (dir);
}

static void
test_valid_file_loads_definition (void)
{
  g_autofree char *path = NULL;
  g_autofree char *expected_ref = NULL;
  g_autofree char *expected_hash = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  file = write_temp_rule_file (valid_rules, -1, &path);
  expected_ref = g_strdup_printf ("file:%s", path);
  expected_hash = wyrebox_wirelog_rule_version_hash (valid_rules, &error);
  g_assert_no_error (error);

  definition = wyrebox_wirelog_rule_definition_load_file (file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (definition);
  g_assert_cmpstr (definition->definition_ref, ==, expected_ref);
  g_assert_cmpstr (definition->rules_source, ==, valid_rules);
  g_assert_cmpstr (definition->rule_version_hash, ==, expected_hash);
  g_assert_cmpstr (definition->relation_name, ==, "show_in_virtual_folder");

  remove_temp_rule_file (path);
}

static void
test_missing_file_errors (void)
{
  g_autofree char *dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  dir = g_dir_make_tmp ("wyrebox-wirelog-rule-definition-XXXXXX", &error);
  g_assert_no_error (error);
  path = g_build_filename (dir, "missing.wl", NULL);
  file = g_file_new_for_path (path);
  definition = wyrebox_wirelog_rule_definition_load_file (file, &error);
  g_assert_null (definition);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error (&error);

  remove_tree (dir);
}

static void
test_empty_file_errors (void)
{
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  file = write_temp_rule_file ("", 0, &path);
  definition = wyrebox_wirelog_rule_definition_load_file (file, &error);
  g_assert_null (definition);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  remove_temp_rule_file (path);
}

static void
test_invalid_wirelog_syntax_errors (void)
{
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  file = write_temp_rule_file ("not valid", -1, &path);
  definition = wyrebox_wirelog_rule_definition_load_file (file, &error);
  g_assert_null (definition);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_temp_rule_file (path);
}

static void
test_embedded_nul_errors (void)
{
  const char bytes[] = { 'e', 'd', 'g', 'e', '(', '1', ',', ' ', '2', ')',
    '.', '\0', '\n'
  };
  g_autofree char *path = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  file = write_temp_rule_file (bytes, sizeof (bytes), &path);
  definition = wyrebox_wirelog_rule_definition_load_file (file, &error);
  g_assert_null (definition);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  remove_temp_rule_file (path);
}

static void
test_null_file_errors (void)
{
  g_autoptr (WyreboxWirelogRuleDefinition) definition = NULL;
  g_autoptr (GError) error = NULL;

  definition = wyrebox_wirelog_rule_definition_load_file (NULL, &error);
  g_assert_null (definition);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/rule-definition/valid-file",
      test_valid_file_loads_definition);
  g_test_add_func ("/wirelog/rule-definition/missing-file",
      test_missing_file_errors);
  g_test_add_func ("/wirelog/rule-definition/empty-file",
      test_empty_file_errors);
  g_test_add_func ("/wirelog/rule-definition/invalid-syntax",
      test_invalid_wirelog_syntax_errors);
  g_test_add_func ("/wirelog/rule-definition/embedded-nul",
      test_embedded_nul_errors);
  g_test_add_func ("/wirelog/rule-definition/null-file", test_null_file_errors);

  return g_test_run ();
}
