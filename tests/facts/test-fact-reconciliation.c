#include "wyrebox-fact-reconciliation.h"

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

static WyreboxFactRecord *
new_fact (const char *predicate,
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

  return record;
}

static void
add_fact (GPtrArray *facts,
    const char *predicate,
    const char *arg0,
    const char *arg1, const char *source, guint64 created_at_unix_us)
{
  g_ptr_array_add (facts,
      new_fact (predicate, arg0, arg1, source, created_at_unix_us));
}

static const WyreboxFactRecord *
fact_at (GPtrArray *facts, guint index)
{
  g_assert_nonnull (facts);
  g_assert_cmpuint (facts->len, >, index);

  return g_ptr_array_index (facts, index);
}

static void
assert_fact (const WyreboxFactRecord *fact,
    const char *predicate,
    const char *arg0,
    const char *arg1,
    const char *source,
    guint64 created_at_unix_us, guint64 retracted_at_unix_us)
{
  g_assert_cmpstr (fact->predicate, ==, predicate);
  g_assert_cmpstr (fact->args[0], ==, arg0);
  g_assert_cmpstr (fact->args[1], ==, arg1);
  g_assert_null (fact->args[2]);
  g_assert_cmpstr (fact->source, ==, source);
  g_assert_cmpuint (fact->confidence_ppm, ==, 1000000);
  g_assert_cmpuint (fact->created_at_unix_us, ==, created_at_unix_us);
  g_assert_cmpuint (fact->retracted_at_unix_us, ==, retracted_at_unix_us);
}

static void
test_empty_previous_nonempty_new_inserts_in_new_order (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  add_fact (next,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (next,
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 200, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  assert_fact (fact_at (changes, 0),
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100, 0);
  assert_fact (fact_at (changes, 1),
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101, 0);
}

static void
test_nonempty_previous_empty_new_retracts_in_previous_order (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (previous,
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 200, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  assert_fact (fact_at (changes, 0),
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100, 200);
  assert_fact (fact_at (changes, 1),
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101, 200);
}

static void
test_overlap_suppresses_unchanged_facts (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (previous,
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101);
  add_fact (next,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      500);
  add_fact (next,
      "reference_candidate", "mail-1", "REF-1", "regex:subject:ref", 501);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 600, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  assert_fact (fact_at (changes, 0),
      "amount_candidate", "mail-1", "42", "regex:subject:amount", 101, 600);
  assert_fact (fact_at (changes, 1),
      "reference_candidate", "mail-1", "REF-1", "regex:subject:ref", 501, 0);
}

static void
test_dictionary_rule_update_retracts_old_then_inserts_new (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (next,
      "project_keyword", "mail-1", "project-beta", "dictionary:subject:a", 300);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 400, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  assert_fact (fact_at (changes, 0),
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100, 400);
  assert_fact (fact_at (changes, 1),
      "project_keyword", "mail-1", "project-beta", "dictionary:subject:a",
      300, 0);
}

static void
test_provenance_change_is_significant (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (next,
      "project_keyword", "mail-1", "project-alpha", "dictionary:from:b", 300);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 400, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  assert_fact (fact_at (changes, 0),
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100, 400);
  assert_fact (fact_at (changes, 1),
      "project_keyword", "mail-1", "project-alpha", "dictionary:from:b", 300,
      0);
}

static void
test_rejects_duplicate_identities_in_previous_and_new (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      101);

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, next, 200,
        &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  g_ptr_array_set_size (previous, 0);
  add_fact (next,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (next,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      101);

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, next, 200,
        &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
}

static void
test_rejects_invalid_inputs_and_retraction_timestamp (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (NULL, next, 200, &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, NULL, 200,
        &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, next, 0, &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, next, 99,
        &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }

  g_ptr_array_add (next, NULL);
  {
    g_autoptr (GError) error = NULL;
    g_autoptr (GPtrArray) changes = NULL;

    changes = wyrebox_fact_reconciliation_reconcile (previous, next, 200,
        &error);
    g_assert_null (changes);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
}

static void
test_input_records_are_not_mutated_and_outputs_are_owned_copies (void)
{
  g_autoptr (GPtrArray) previous = new_fact_array ();
  g_autoptr (GPtrArray) next = new_fact_array ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) changes = NULL;
  const WyreboxFactRecord *previous_fact = NULL;
  const WyreboxFactRecord *next_fact = NULL;
  const WyreboxFactRecord *retraction = NULL;
  const WyreboxFactRecord *insert = NULL;

  add_fact (previous,
      "project_keyword", "mail-1", "project-alpha", "dictionary:subject:a",
      100);
  add_fact (next,
      "project_keyword", "mail-1", "project-beta", "dictionary:subject:a", 300);
  previous_fact = fact_at (previous, 0);
  next_fact = fact_at (next, 0);

  changes = wyrebox_fact_reconciliation_reconcile (previous, next, 400, &error);
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  retraction = fact_at (changes, 0);
  insert = fact_at (changes, 1);
  g_assert_true (retraction != previous_fact);
  g_assert_true (insert != next_fact);
  g_assert_true (retraction->args != previous_fact->args);
  g_assert_true (insert->args != next_fact->args);
  g_assert_cmpuint (previous_fact->retracted_at_unix_us, ==, 0);
  g_assert_cmpuint (next_fact->retracted_at_unix_us, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/facts/reconciliation/inserts-in-new-order",
      test_empty_previous_nonempty_new_inserts_in_new_order);
  g_test_add_func ("/facts/reconciliation/retracts-in-previous-order",
      test_nonempty_previous_empty_new_retracts_in_previous_order);
  g_test_add_func ("/facts/reconciliation/suppresses-overlap",
      test_overlap_suppresses_unchanged_facts);
  g_test_add_func ("/facts/reconciliation/dictionary-rule-update",
      test_dictionary_rule_update_retracts_old_then_inserts_new);
  g_test_add_func ("/facts/reconciliation/provenance-change-significant",
      test_provenance_change_is_significant);
  g_test_add_func ("/facts/reconciliation/rejects-duplicate-identities",
      test_rejects_duplicate_identities_in_previous_and_new);
  g_test_add_func ("/facts/reconciliation/rejects-invalid-inputs",
      test_rejects_invalid_inputs_and_retraction_timestamp);
  g_test_add_func ("/facts/reconciliation/inputs-immutable",
      test_input_records_are_not_mutated_and_outputs_are_owned_copies);

  return g_test_run ();
}
