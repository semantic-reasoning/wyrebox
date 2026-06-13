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
test_fact_mutation_dispatcher_handles_valid_envelope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_dispatch (service,
          "request-1",
          "trusted-tool",
          "account-1",
          "fact-importer", "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);

  remove_tree (root);
}

static void
    test_fact_mutation_dispatcher_rejects_unauthorized_caller_with_error_frame
    (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_dispatch (service,
          "request-1",
          "dovecot",
          "account-1",
          "fact-importer", "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
    test_fact_mutation_dispatcher_rejects_account_scope_mismatch_with_error_frame
    (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_dispatch (service,
          "request-1",
          "trusted-tool",
          "account-2",
          "fact-importer", "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_dispatcher_rejects_missing_request_id_before_service (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-dispatcher-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_dispatch (service,
          "",
          "trusted-tool",
          "account-1",
          "fact-importer", "correlation-1", &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-mutation-dispatcher/"
      "handles-valid-envelope",
      test_fact_mutation_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/fact-mutation-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_fact_mutation_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/fact-mutation-dispatcher/"
      "rejects-account-scope-mismatch-with-error-frame",
      test_fact_mutation_dispatcher_rejects_account_scope_mismatch_with_error_frame);
  g_test_add_func ("/daemon-api/fact-mutation-dispatcher/"
      "rejects-missing-request-id-before-service",
      test_fact_mutation_dispatcher_rejects_missing_request_id_before_service);

  return g_test_run ();
}
