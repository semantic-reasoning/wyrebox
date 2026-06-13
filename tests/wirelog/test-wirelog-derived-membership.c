#include "wyrebox-wirelog-derived-membership.h"
#include "wyrebox-wirelog-evaluation.h"
#include "wyrebox-wirelog-program.h"

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

  return g_test_run ();
}
