#include "wyrebox-daemon-fact-mutation-dispatcher.h"
#include "wyrebox-journal-reader.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

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

static void
assert_journal_is_empty (const char *root)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
init_mutation (WyreboxDaemonFactMutationRequest *request,
    WyreboxDaemonFactMutationKind kind)
{
  const char *args[] = { "mail-1", NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_mutation_request_init (request,
          kind, "project_mention", "account-1", args, &error));
  g_assert_no_error (error);
}

static void
init_batch (WyreboxDaemonFactBatchImportRequest *request,
    const WyreboxDaemonFactMutationRequest *const *entries, guint n_entries)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_batch_import_request_init (request,
          entries, n_entries, &error));
  g_assert_no_error (error);
}

static void
test_fact_batch_import_dispatcher_handles_valid_envelope (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) insert = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) retract = { 0 };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autofree char *expected_marker = NULL;
  const WyreboxDaemonFactMutationRequest *entries[] = { &insert, &retract };

  g_assert_nonnull (root);
  init_mutation (&insert, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  init_mutation (&retract, WYREBOX_DAEMON_FACT_MUTATION_RETRACT);
  init_batch (&batch, entries, G_N_ELEMENTS (entries));

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_batch_import_dispatch (service,
          "request-1",
          "trusted-tool",
          "account-1",
          "fact-importer", "correlation-1", &batch, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpuint (frame.success.journal_sequence, ==, 2);
  expected_marker = g_strdup_printf ("journal:%" G_GUINT64_FORMAT ":2",
      frame.success.journal_offset);
  g_assert_cmpstr (frame.success.durable_marker, ==, expected_marker);
  g_assert_cmpstr (frame.success.summary, ==,
      "fact_batch_import count=2 scope_id=account-1");

  remove_tree (root);
}

static void
test_fact_batch_import_dispatcher_rejects_unauthorized_with_error_frame (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) insert = { 0 };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const WyreboxDaemonFactMutationRequest *entries[] = { &insert };

  g_assert_nonnull (root);
  init_mutation (&insert, WYREBOX_DAEMON_FACT_MUTATION_INSERT);
  init_batch (&batch, entries, G_N_ELEMENTS (entries));

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_batch_import_dispatch (service,
          "request-1",
          "admin-cli",
          "account-1",
          "fact-importer", "correlation-1", &batch, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  assert_journal_is_empty (root);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-batch-import-dispatcher/"
      "handles-valid-envelope",
      test_fact_batch_import_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/fact-batch-import-dispatcher/"
      "rejects-unauthorized-with-error-frame",
      test_fact_batch_import_dispatcher_rejects_unauthorized_with_error_frame);

  return g_test_run ();
}
