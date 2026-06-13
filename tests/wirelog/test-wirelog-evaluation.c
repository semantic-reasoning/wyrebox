#include "wyrebox-fact-record.h"
#include "wyrebox-wirelog-evaluation.h"
#include "wyrebox-wirelog-source.h"

#include <gio/gio.h>
#include <glib.h>

static void
fact_record_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static GPtrArray *
new_fact_array (void)
{
  return g_ptr_array_new_with_free_func (fact_record_free);
}

static void
add_fact (GPtrArray *facts,
    const char *predicate,
    const char *arg0,
    const char *arg1, const char *source, guint64 created_at_unix_us)
{
  const char *args[] = {
    arg0,
    arg1,
    NULL,
  };
  g_autoptr (GError) error = NULL;
  WyreboxFactRecord *record = g_new0 (WyreboxFactRecord, 1);

  g_assert_true (wyrebox_fact_record_init (record,
          predicate, args, source, 1000000, created_at_unix_us, &error));
  g_assert_no_error (error);
  g_ptr_array_add (facts, record);
}

static WyreboxWirelogProgram *
new_project_program (GError **error)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();

  add_fact (facts, "source", "mail-1", "project-alpha",
      "dictionary:subject:alpha", 100);
  add_fact (facts, "source", "mail-2", "project-beta",
      "dictionary:subject:beta", 101);

  return wyrebox_wirelog_program_new_from_rules_and_facts
      (".decl source(mail: symbol, key: symbol)\n"
      ".decl project(mail: symbol, key: symbol)\n"
      "project(mail, key) :- source(mail, key).", facts, error);
}

static void
test_program_evaluates_and_reports_relation_cardinality (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GError) error = NULL;
  guint64 cardinality = 0;

  program = new_project_program (&error);
  g_assert_no_error (error);
  g_assert_nonnull (program);

  g_assert_true (wyrebox_wirelog_program_evaluate (program, &evaluation,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (evaluation);

  g_assert_true (wyrebox_wirelog_evaluation_get_relation_cardinality
      (evaluation, "project", &cardinality, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (cardinality, ==, 2);
}

static void
test_missing_relation_reports_zero_cardinality (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GError) error = NULL;
  guint64 cardinality = 99;

  program = new_project_program (&error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_wirelog_program_evaluate (program, &evaluation,
          &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_wirelog_evaluation_get_relation_cardinality
      (evaluation, "missing_relation", &cardinality, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (cardinality, ==, 0);
}

static void
test_invalid_evaluate_arguments_fail (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_wirelog_program_evaluate (NULL, &evaluation, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);
  g_assert_null (evaluation);

  program = new_project_program (&error);
  g_assert_no_error (error);
  g_assert_nonnull (program);

  g_assert_false (wyrebox_wirelog_program_evaluate (program, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_invalid_cardinality_arguments_fail (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GError) error = NULL;
  guint64 cardinality = 0;

  program = new_project_program (&error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_wirelog_program_evaluate (program, &evaluation,
          &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_wirelog_evaluation_get_relation_cardinality
      (NULL, "project", &cardinality, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_wirelog_evaluation_get_relation_cardinality
      (evaluation, NULL, &cardinality, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_wirelog_evaluation_get_relation_cardinality
      (evaluation, "", &cardinality, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_wirelog_evaluation_get_relation_cardinality
      (evaluation, "project", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/evaluation/relation-cardinality",
      test_program_evaluates_and_reports_relation_cardinality);
  g_test_add_func ("/wirelog/evaluation/missing-relation",
      test_missing_relation_reports_zero_cardinality);
  g_test_add_func ("/wirelog/evaluation/invalid-evaluate-arguments",
      test_invalid_evaluate_arguments_fail);
  g_test_add_func ("/wirelog/evaluation/invalid-cardinality-arguments",
      test_invalid_cardinality_arguments_fail);

  return g_test_run ();
}
