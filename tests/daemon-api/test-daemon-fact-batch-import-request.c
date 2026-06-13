#include "wyrebox-daemon-fact-batch-import-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
init_mutation (WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonFactMutationKind kind, const char *scope_id)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (request,
          kind, "project_mention", scope_id, args, &error));
  g_assert_no_error (error);
}

static void
test_fact_batch_import_request_copies_ordered_entries (void)
{
  g_auto (WyreboxDaemonFactMutationRequest) insert = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) retract = { 0 };
  const WyreboxDaemonFactMutationRequest *entries[] = { &insert, &retract };
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_mutation (&insert, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  init_mutation (&retract, WYREBOX_DAEMON_FACT_MUTATION_RETRACT, "account-1");

  g_assert_true (wyrebox_daemon_fact_batch_import_request_init (&request,
          entries, G_N_ELEMENTS (entries), &error));
  g_assert_no_error (error);
  g_assert_cmpuint (wyrebox_daemon_fact_batch_import_request_get_n_entries
      (&request), ==, 2);
  g_assert_cmpstr (wyrebox_daemon_fact_batch_import_request_get_scope_id
      (&request), ==, "account-1");
  g_assert_cmpint (wyrebox_daemon_fact_batch_import_request_get_entry
      (&request, 0)->mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  g_assert_cmpint (wyrebox_daemon_fact_batch_import_request_get_entry
      (&request, 1)->mutation, ==, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);

  g_free (insert.scope_id);
  insert.scope_id = g_strdup ("mutated-after-copy");
  g_assert_cmpstr (wyrebox_daemon_fact_batch_import_request_get_entry
      (&request, 0)->scope_id, ==, "account-1");
}

static void
test_fact_batch_import_request_rejects_empty (void)
{
  const WyreboxDaemonFactMutationRequest *entries[] = { NULL };
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_fact_batch_import_request_init (&request,
          entries, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_batch_import_request_rejects_null_entries (void)
{
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_fact_batch_import_request_init (&request,
          NULL, 1, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_batch_import_request_rejects_invalid_entry (void)
{
  WyreboxDaemonFactMutationRequest invalid = { 0 };
  const WyreboxDaemonFactMutationRequest *entries[] = { &invalid };
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_fact_batch_import_request_init (&request,
          entries, G_N_ELEMENTS (entries), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_batch_import_request_rejects_mixed_scope (void)
{
  g_auto (WyreboxDaemonFactMutationRequest) first = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) second = { 0 };
  const WyreboxDaemonFactMutationRequest *entries[] = { &first, &second };
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;

  init_mutation (&first, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  init_mutation (&second, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-2");

  g_assert_false (wyrebox_daemon_fact_batch_import_request_init (&request,
          entries, G_N_ELEMENTS (entries), &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_fact_batch_import_request_rejects_over_limit (void)
{
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_autofree const WyreboxDaemonFactMutationRequest **entries = NULL;
  g_auto (WyreboxDaemonFactBatchImportRequest) request = { 0 };
  g_autoptr (GError) error = NULL;
  guint n_entries = WYREBOX_DAEMON_FACT_BATCH_IMPORT_REQUEST_MAX_ENTRIES + 1;

  init_mutation (&mutation, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  entries = g_new0 (const WyreboxDaemonFactMutationRequest *, n_entries);
  for (guint i = 0; i < n_entries; i++)
    entries[i] = &mutation;

  g_assert_false (wyrebox_daemon_fact_batch_import_request_init (&request,
          entries, n_entries, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-batch-import-request/copies-ordered",
      test_fact_batch_import_request_copies_ordered_entries);
  g_test_add_func ("/daemon-api/fact-batch-import-request/rejects-empty",
      test_fact_batch_import_request_rejects_empty);
  g_test_add_func ("/daemon-api/fact-batch-import-request/rejects-null",
      test_fact_batch_import_request_rejects_null_entries);
  g_test_add_func ("/daemon-api/fact-batch-import-request/rejects-invalid",
      test_fact_batch_import_request_rejects_invalid_entry);
  g_test_add_func ("/daemon-api/fact-batch-import-request/rejects-mixed-scope",
      test_fact_batch_import_request_rejects_mixed_scope);
  g_test_add_func ("/daemon-api/fact-batch-import-request/rejects-over-limit",
      test_fact_batch_import_request_rejects_over_limit);

  return g_test_run ();
}
