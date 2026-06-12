#include "wyrebox-daemon-request-router.h"
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
test_request_router_routes_fact_mutation (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");

  remove_tree (root);
}

static void
test_request_router_rejects_unauthorized_fact_mutation_with_error_frame (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "dovecot";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service,
          &request_frame, &frame, &error));
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
test_request_router_rejects_missing_fact_mutation_payload (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = NULL;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.error.request_id, ==, "request-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_rejects_unsupported_operation_with_error_frame (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);

  request_frame.request_id = "request-1";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_request_router_route (service,
          &request_frame, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.error.request_id, ==, "request-1");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_request_router_rejects_missing_request_id_without_error_frame (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-request-router-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) mutation = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  WyreboxDaemonDecodedRequestFrame request_frame = { 0 };

  g_assert_nonnull (root);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&mutation,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  request_frame.request_id = "";
  request_frame.caller_identity = "skill";
  request_frame.account_identity = "account-1";
  request_frame.tool_identity = "fact-importer";
  request_frame.correlation_id = "correlation-1";
  request_frame.operation =
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_FACT_MUTATION;
  request_frame.fact_mutation = &mutation;

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_request_router_route (service,
          &request_frame, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/request-router/routes-fact-mutation",
      test_request_router_routes_fact_mutation);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-unauthorized-fact-mutation-with-error-frame",
      test_request_router_rejects_unauthorized_fact_mutation_with_error_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-fact-mutation-payload",
      test_request_router_rejects_missing_fact_mutation_payload);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-unsupported-operation-with-error-frame",
      test_request_router_rejects_unsupported_operation_with_error_frame);
  g_test_add_func ("/daemon-api/request-router/"
      "rejects-missing-request-id-without-error-frame",
      test_request_router_rejects_missing_request_id_without_error_frame);

  return g_test_run ();
}
