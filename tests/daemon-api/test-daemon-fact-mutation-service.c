#include "wyrebox-daemon-fact-mutation-service.h"
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
test_fact_mutation_service_handles_request (void)
{
  const char *args[] = { "mail-1", "project-a", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");
  g_assert_cmpuint (frame.success.journal_offset, ==, 0);
  g_assert_cmpuint (frame.success.journal_sequence, ==, 1);

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type, ==, WYREBOX_JOURNAL_EVENT_FACT_INSERTED);
  g_assert_cmpuint (record.sequence, ==, 1);

  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_identity_without_caller (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", NULL, "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_unauthorized_identity (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "dovecot", "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
assert_fact_mutation_service_rejects_caller (const char *caller_identity)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          caller_identity, "account-1", "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_legacy_skill_identity (void)
{
  assert_fact_mutation_service_rejects_caller ("skill");
}

static void
test_fact_mutation_service_rejects_admin_cli_identity (void)
{
  assert_fact_mutation_service_rejects_caller ("admin-cli");
}

static void
test_fact_mutation_service_rejects_identity_without_account_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", NULL, "fact-importer", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_mismatched_account_scope (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-2", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_INSERT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_rejects_null_identity_request (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_test_expect_message (NULL, G_LOG_LEVEL_CRITICAL, "*request != NULL*");
  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, NULL, &frame, &error));
  g_test_assert_expected_messages ();
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);

  assert_journal_is_empty (root);
  remove_tree (root);
}

static void
test_fact_mutation_service_handles_identity (void)
{
  const char *args[] = { "mail-1", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-mutation-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request,
          WYREBOX_DAEMON_FACT_MUTATION_RETRACT,
          "project_mention", "account-1", args, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:0:1");

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-mutation-service/handles-request",
      test_fact_mutation_service_handles_request);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-identity-without-caller",
      test_fact_mutation_service_rejects_identity_without_caller);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-unauthorized-identity",
      test_fact_mutation_service_rejects_unauthorized_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-legacy-skill-identity",
      test_fact_mutation_service_rejects_legacy_skill_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-admin-cli-identity",
      test_fact_mutation_service_rejects_admin_cli_identity);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-identity-without-account-scope",
      test_fact_mutation_service_rejects_identity_without_account_scope);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-mismatched-account-scope",
      test_fact_mutation_service_rejects_mismatched_account_scope);
  g_test_add_func ("/daemon-api/fact-mutation-service/"
      "rejects-null-identity-request",
      test_fact_mutation_service_rejects_null_identity_request);
  g_test_add_func ("/daemon-api/fact-mutation-service/handles-identity",
      test_fact_mutation_service_handles_identity);

  return g_test_run ();
}
