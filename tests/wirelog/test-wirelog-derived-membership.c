#include "wyrebox-wirelog-derived-membership.h"
#include "wyrebox-wirelog-evaluation.h"
#include "wyrebox-wirelog-program.h"
#include "wyrebox-wirelog-source.h"
#include "wyrebox-fact-record.h"

#include <gio/gio.h>
#include <glib.h>

static WyreboxWirelogDerivedMembership *
membership_at (GPtrArray *memberships, guint index)
{
  return g_ptr_array_index (memberships, index);
}

static void
test_parse_simple_rows_preserves_order (void)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  memberships = wyrebox_wirelog_derived_membership_parse_csv
      ("view-a,msg-1\nview-b,msg-2\n", &error);
  g_assert_no_error (error);
  g_assert_nonnull (memberships);
  g_assert_cmpuint (memberships->len, ==, 2);
  g_assert_cmpstr (membership_at (memberships, 0)->view_id, ==, "view-a");
  g_assert_cmpstr (membership_at (memberships, 0)->message_id, ==, "msg-1");
  g_assert_cmpstr (membership_at (memberships, 1)->view_id, ==, "view-b");
  g_assert_cmpstr (membership_at (memberships, 1)->message_id, ==, "msg-2");
}

static void
test_parse_quoted_values (void)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  memberships = wyrebox_wirelog_derived_membership_parse_csv
      ("\"view, one\",\"msg \"\"quoted\"\"\"\n", &error);
  g_assert_no_error (error);
  g_assert_nonnull (memberships);
  g_assert_cmpuint (memberships->len, ==, 1);
  g_assert_cmpstr (membership_at (memberships, 0)->view_id, ==, "view, one");
  g_assert_cmpstr (membership_at (memberships, 0)->message_id, ==,
      "msg \"quoted\"");
}

static void
test_parse_preserves_duplicates (void)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  memberships = wyrebox_wirelog_derived_membership_parse_csv
      ("view-a,msg-1\nview-a,msg-1\n", &error);
  g_assert_no_error (error);
  g_assert_nonnull (memberships);
  g_assert_cmpuint (memberships->len, ==, 2);
  g_assert_cmpstr (membership_at (memberships, 0)->view_id, ==, "view-a");
  g_assert_cmpstr (membership_at (memberships, 1)->view_id, ==, "view-a");
  g_assert_cmpstr (membership_at (memberships, 0)->message_id, ==, "msg-1");
  g_assert_cmpstr (membership_at (memberships, 1)->message_id, ==, "msg-1");
}

static void
assert_parse_fails (const char *csv)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  memberships = wyrebox_wirelog_derived_membership_parse_csv (csv, &error);
  g_assert_null (memberships);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_parse_rejects_empty_view_id (void)
{
  assert_parse_fails (",msg-1\n");
}

static void
test_parse_rejects_empty_message_id (void)
{
  assert_parse_fails ("view-a,\n");
}

static void
test_parse_rejects_wrong_column_count (void)
{
  assert_parse_fails ("view-a\n");
  assert_parse_fails ("view-a,msg-1,extra\n");
}

static void
test_parse_rejects_malformed_quotes (void)
{
  assert_parse_fails ("\"view-a,msg-1\n");
  assert_parse_fails ("\"view-a\"x,msg-1\n");
  assert_parse_fails ("view\"-a,msg-1\n");
}

static void
test_parse_rejects_invalid_args (void)
{
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  memberships = wyrebox_wirelog_derived_membership_parse_csv (NULL, &error);
  g_assert_null (memberships);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static WyreboxWirelogProgram *
new_integer_relation_program (GError **error)
{
  return wyrebox_wirelog_program_new_from_source
      (".decl source(a: int64, b: int64)\n"
      ".decl pair(a: int64, b: int64)\n"
      "pair(a, b) :- source(a, b).\n" "source(7, 11).\n", error);
}

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
    const char *predicate, const char *arg0, const char *arg1)
{
  const char *args[] = {
    arg0,
    arg1,
    NULL,
  };
  g_autoptr (GError) error = NULL;
  WyreboxFactRecord *record = g_new0 (WyreboxFactRecord, 1);

  g_assert_true (wyrebox_fact_record_init (record,
          predicate, args, "test:derived-membership", 1000000, 100, &error));
  g_assert_no_error (error);
  g_ptr_array_add (facts, record);
}

static WyreboxWirelogProgram *
new_symbol_membership_program (GError **error)
{
  g_autoptr (GPtrArray) facts = new_fact_array ();

  add_fact (facts, "source_membership", "view-alpha", "message-1");

  return wyrebox_wirelog_program_new_from_rules_and_facts
      (".decl source_membership(view_id: symbol, message_id: symbol)\n"
      ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
      "show_in_virtual_folder(view_id, message_id) :- "
      "source_membership(view_id, message_id).", facts, error);
}

static void
test_helper_propagates_missing_relation_failure (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  program = new_integer_relation_program (&error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
  g_assert_true (wyrebox_wirelog_program_evaluate (program, &evaluation,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (evaluation);

  memberships =
      wyrebox_wirelog_derived_membership_parse_evaluation_relation
      (evaluation, "missing_relation", &error);
  g_assert_null (memberships);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

static void
test_helper_rejects_symbol_csv_export (void)
{
  g_autoptr (WyreboxWirelogProgram) program = NULL;
  g_autoptr (WyreboxWirelogEvaluation) evaluation = NULL;
  g_autoptr (GPtrArray) memberships = NULL;
  g_autoptr (GError) error = NULL;

  program = new_symbol_membership_program (&error);
  g_assert_no_error (error);
  g_assert_nonnull (program);
  g_assert_true (wyrebox_wirelog_program_evaluate (program, &evaluation,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (evaluation);

  memberships =
      wyrebox_wirelog_derived_membership_parse_evaluation_relation
      (evaluation, "show_in_virtual_folder", &error);
  g_assert_null (memberships);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/derived-membership/parse-simple-rows",
      test_parse_simple_rows_preserves_order);
  g_test_add_func ("/wirelog/derived-membership/parse-quoted-values",
      test_parse_quoted_values);
  g_test_add_func ("/wirelog/derived-membership/preserve-duplicates",
      test_parse_preserves_duplicates);
  g_test_add_func ("/wirelog/derived-membership/reject-empty-view-id",
      test_parse_rejects_empty_view_id);
  g_test_add_func ("/wirelog/derived-membership/reject-empty-message-id",
      test_parse_rejects_empty_message_id);
  g_test_add_func ("/wirelog/derived-membership/reject-wrong-column-count",
      test_parse_rejects_wrong_column_count);
  g_test_add_func ("/wirelog/derived-membership/reject-malformed-quotes",
      test_parse_rejects_malformed_quotes);
  g_test_add_func ("/wirelog/derived-membership/reject-invalid-args",
      test_parse_rejects_invalid_args);
  g_test_add_func ("/wirelog/derived-membership/helper-missing-relation",
      test_helper_propagates_missing_relation_failure);
  g_test_add_func ("/wirelog/derived-membership/helper-symbol-csv-unsupported",
      test_helper_rejects_symbol_csv_export);

  return g_test_run ();
}
