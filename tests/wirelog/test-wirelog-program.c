#include "wyrebox-wirelog-program.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_valid_source_parses_and_finalizes (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;

  program = wyrebox_wirelog_program_new_from_source ("edge(1, 2).\n", &error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
}

static void
test_invalid_source_fails (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;

  program = wyrebox_wirelog_program_new_from_source ("not valid", &error);
  g_assert_null (program);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_null_source_fails (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;

  program = wyrebox_wirelog_program_new_from_source (NULL, &error);
  g_assert_null (program);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_empty_source_fails (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;

  program = wyrebox_wirelog_program_new_from_source ("", &error);
  g_assert_null (program);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/program/valid-source",
      test_valid_source_parses_and_finalizes);
  g_test_add_func ("/wirelog/program/invalid-source",
      test_invalid_source_fails);
  g_test_add_func ("/wirelog/program/null-source", test_null_source_fails);
  g_test_add_func ("/wirelog/program/empty-source", test_empty_source_fails);

  return g_test_run ();
}
