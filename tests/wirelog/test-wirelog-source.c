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

static void
test_rules_and_facts_build_deterministic_source_and_parse (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autofree char *source = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (GError) error = NULL;

  add_fact (facts, "source", "mail-1", "project-alpha",
      "dictionary:subject:alpha", 100);
  add_fact (facts, "source", "mail-2", "project-beta",
      "dictionary:subject:beta", 101);

  source =
      wyrebox_wirelog_source_build ("project(mail, key) :- source(mail, key).",
      facts, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (source, ==,
      "project(mail, key) :- source(mail, key).\n"
      "source(\"mail-1\", \"project-alpha\").\n"
      "source(\"mail-2\", \"project-beta\").\n");

  program =
      wyrebox_wirelog_program_new_from_rules_and_facts
      ("project(mail, key) :- source(mail, key).", facts, &error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
}

static void
test_fact_only_source_parses (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autofree char *source = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (GError) error = NULL;

  add_fact (facts, "source", "mail-1", "project-alpha",
      "dictionary:subject:alpha", 100);

  source = wyrebox_wirelog_source_build (NULL, facts, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (source, ==, "source(\"mail-1\", \"project-alpha\").\n");

  program = wyrebox_wirelog_program_new_from_rules_and_facts (NULL, facts,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
}

static void
test_empty_fact_array_with_rules_parses (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autofree char *source = NULL;
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (GError) error = NULL;

  source = wyrebox_wirelog_source_build ("edge(1, 2).\n", facts, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (source, ==, "edge(1, 2).\n");

  program = wyrebox_wirelog_program_new_from_rules_and_facts ("edge(1, 2).\n",
      facts, &error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
}

static void
test_invalid_fact_record_fails_before_parse (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autofree char *source = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxFactRecord *record = g_new0 (WyreboxFactRecord, 1);

  record->predicate = g_strdup ("bad");
  record->args = g_new0 (char *, 3);
  record->args[0] = g_strdup ("mail-1");
  record->args[1] = g_strdup ("");
  record->source = g_strdup ("test:invalid");
  record->confidence_ppm = 1000000;
  record->created_at_unix_us = 100;
  g_ptr_array_add (facts, record);

  source = wyrebox_wirelog_source_build ("edge(1, 2).", facts, &error);
  g_assert_null (source);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_invalid_rule_text_fails_in_program_path (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (GError) error = NULL;

  add_fact (facts, "source", "mail-1", "project-alpha",
      "dictionary:subject:alpha", 100);

  program = wyrebox_wirelog_program_new_from_rules_and_facts ("not valid",
      facts, &error);
  g_assert_null (program);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_fact_escaping_is_preserved (void)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autofree char *source = NULL;
  g_autoptr (GError) error = NULL;

  add_fact (facts, "note", "mail-1", "quote \" slash \\ line\n tab\t",
      "test:escaping", 100);

  source = wyrebox_wirelog_source_build (NULL, facts, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (source, ==,
      "note(\"mail-1\", \"quote \\\" slash \\\\ line\\n tab\\t\").\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/source/rules-and-facts",
      test_rules_and_facts_build_deterministic_source_and_parse);
  g_test_add_func ("/wirelog/source/fact-only", test_fact_only_source_parses);
  g_test_add_func ("/wirelog/source/empty-facts-with-rules",
      test_empty_fact_array_with_rules_parses);
  g_test_add_func ("/wirelog/source/invalid-fact-record",
      test_invalid_fact_record_fails_before_parse);
  g_test_add_func ("/wirelog/source/invalid-rule-text",
      test_invalid_rule_text_fails_in_program_path);
  g_test_add_func ("/wirelog/source/fact-escaping",
      test_fact_escaping_is_preserved);

  return g_test_run ();
}
