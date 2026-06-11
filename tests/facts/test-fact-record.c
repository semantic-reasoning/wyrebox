#include "wyrebox-fact-record.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_fact_record_copies_owned_fields (void)
{
  const char *args[] = {
    "mail-1",
    "example.test",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "sender_domain",
          args, "header:from", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (record.predicate, ==, "sender_domain");
  g_assert_nonnull (record.args);
  g_assert_cmpstr (record.args[0], ==, "mail-1");
  g_assert_cmpstr (record.args[1], ==, "example.test");
  g_assert_null (record.args[2]);
  g_assert_cmpstr (record.source, ==, "header:from");
  g_assert_cmpuint (record.confidence_ppm, ==, 1000000);
  g_assert_cmpuint (record.created_at_unix_us, ==, 1800000000000000);
  g_assert_cmpuint (record.retracted_at_unix_us, ==, 0);
}

static void
test_fact_record_rejects_invalid_predicate (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_false (wyrebox_fact_record_init (&record,
          "SenderDomain", args, "header:from", 1000000,
          1800000000000000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (record.predicate);
  g_assert_null (record.args);
  g_assert_null (record.source);
}

static void
test_fact_record_rejects_missing_provenance (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_false (wyrebox_fact_record_init (&record,
          "participant", args, "", 1000000, 1800000000000000, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_record_marks_retraction_after_creation (void)
{
  const char *args[] = {
    "mail-1",
    "parent-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "references", args, "header:references", 1000000,
          1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_fact_record_mark_retracted (&record,
          1800000000000001, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (record.retracted_at_unix_us, ==, 1800000000000001);
}

static void
test_fact_record_rejects_retraction_before_creation (void)
{
  const char *args[] = {
    "mail-1",
    NULL,
  };
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxFactRecord) record = { 0 };

  g_assert_true (wyrebox_fact_record_init (&record,
          "participant", args, "header:to", 1000000, 1800000000000000, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_fact_record_mark_retracted (&record,
          1799999999999999, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/facts/fact-record/copies-owned-fields",
      test_fact_record_copies_owned_fields);
  g_test_add_func ("/facts/fact-record/rejects-invalid-predicate",
      test_fact_record_rejects_invalid_predicate);
  g_test_add_func ("/facts/fact-record/rejects-missing-provenance",
      test_fact_record_rejects_missing_provenance);
  g_test_add_func ("/facts/fact-record/marks-retraction-after-creation",
      test_fact_record_marks_retraction_after_creation);
  g_test_add_func ("/facts/fact-record/rejects-retraction-before-creation",
      test_fact_record_rejects_retraction_before_creation);

  return g_test_run ();
}
