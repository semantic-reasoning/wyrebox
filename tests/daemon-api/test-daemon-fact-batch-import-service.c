#include "wyrebox-daemon-fact-mutation-service.h"
#include "wyrebox-daemon-audit-payload.h"
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
init_batch (WyreboxDaemonFactBatchImportRequest *request,
    const WyreboxDaemonFactMutationRequest *const *entries, guint n_entries)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_fact_batch_import_request_init (request,
          entries, n_entries, &error));
  g_assert_no_error (error);
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
assert_journal_events (const char *root,
    const WyreboxJournalEventType *event_types,
    guint n_event_types, WyreboxDaemonAuditPayload *out_audit)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);

  for (guint i = 0; i < n_event_types; i++) {
    g_auto (WyreboxJournalRecord) record = { 0 };

    g_assert_true (wyrebox_journal_reader_read_next (reader,
            &record, &eof, &error));
    g_assert_no_error (error);
    g_assert_false (eof);
    g_assert_cmpint (record.event_type, ==, event_types[i]);
    g_assert_cmpuint (record.sequence, ==, i + 1);

    if (record.event_type == WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED &&
        out_audit != NULL) {
      g_assert_true (wyrebox_daemon_audit_payload_decode (record.payload,
              out_audit, &error));
      g_assert_no_error (error);
    }
  }

  g_auto (WyreboxJournalRecord) record = { 0 };
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
test_fact_batch_import_service_handles_valid_batch (void)
{
  const WyreboxJournalEventType expected[] = {
    WYREBOX_JOURNAL_EVENT_FACT_INSERTED,
    WYREBOX_JOURNAL_EVENT_FACT_RETRACTED,
    WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED,
  };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) insert = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) retract = { 0 };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };
  g_autofree char *expected_marker = NULL;
  const WyreboxDaemonFactMutationRequest *entries[] = { &insert, &retract };

  g_assert_nonnull (root);
  init_mutation (&insert, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  init_mutation (&retract, WYREBOX_DAEMON_FACT_MUTATION_RETRACT, "account-1");
  init_batch (&batch, entries, G_N_ELEMENTS (entries));
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          "trusted-tool", "account-1", "fact-importer", "correlation-1",
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_fact_mutation_service_handle_batch_identity
      (service, &identity, &batch, &frame, &error));
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
  assert_journal_events (root, expected, G_N_ELEMENTS (expected), &audit);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_FACT_BATCH_IMPORT);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_SUCCESS);
  g_assert_cmpstr (audit.request_id, ==, "request-1");
  g_assert_cmpstr (audit.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (audit.caller_identity, ==, "trusted-tool");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.tool_identity, ==, "fact-importer");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpuint (audit.mutation_count, ==, 2);
  g_assert_null (audit.predicate_id);
  g_assert_cmpuint (audit.final_journal_offset, ==,
      frame.success.journal_offset);
  g_assert_cmpuint (audit.final_journal_sequence, ==,
      frame.success.journal_sequence);

  remove_tree (root);
}

static void
test_fact_batch_import_service_rejects_invalid_item_without_writes (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  batch.scope_id = g_strdup ("account-1");
  batch.entries = g_ptr_array_new ();
  g_ptr_array_add (batch.entries, NULL);
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_batch_identity
      (service, &identity, &batch, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  assert_journal_is_empty (root);

  remove_tree (root);
}

static void
test_fact_batch_import_service_rejects_late_invalid_item_without_writes (void)
{
  char *invalid_args[] = { "", NULL };
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) first = { 0 };
  WyreboxDaemonFactMutationRequest invalid = {
    .mutation = WYREBOX_DAEMON_FACT_MUTATION_INSERT,
    .predicate_id = "project_mention",
    .scope_id = "account-1",
    .arguments = invalid_args,
  };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  init_mutation (&first, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  batch.scope_id = g_strdup ("account-1");
  batch.entries = g_ptr_array_new ();
  g_ptr_array_add (batch.entries, &first);
  g_ptr_array_add (batch.entries, &invalid);
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_batch_identity
      (service, &identity, &batch, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  assert_journal_is_empty (root);

  remove_tree (root);
}

static void
test_fact_batch_import_service_rejects_mixed_scope_without_writes (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) first = { 0 };
  g_auto (WyreboxDaemonFactMutationRequest) second = { 0 };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_nonnull (root);
  init_mutation (&first, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  init_mutation (&second, WYREBOX_DAEMON_FACT_MUTATION_RETRACT, "account-2");
  batch.scope_id = g_strdup ("account-1");
  batch.entries = g_ptr_array_new ();
  g_ptr_array_add (batch.entries, &first);
  g_ptr_array_add (batch.entries, &second);
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1", "trusted-tool", "account-1", "fact-importer", NULL,
          &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_batch_identity
      (service, &identity, &batch, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  assert_journal_is_empty (root);

  remove_tree (root);
}

static void
assert_batch_denied_without_writes (const char *caller_identity,
    const char *account_identity)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-fact-batch-service-XXXXXX", NULL);
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (WyreboxDaemonFactMutationService) service = NULL;
  g_auto (WyreboxDaemonFactMutationRequest) insert = { 0 };
  g_auto (WyreboxDaemonFactBatchImportRequest) batch = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  const WyreboxDaemonFactMutationRequest *entries[] = { &insert };

  g_assert_nonnull (root);
  init_mutation (&insert, WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-1");
  init_batch (&batch, entries, G_N_ELEMENTS (entries));
  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-1",
          caller_identity, account_identity, "fact-importer", NULL, &error));
  g_assert_no_error (error);

  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  service = wyrebox_daemon_fact_mutation_service_new (writer);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_fact_mutation_service_handle_batch_identity
      (service, &identity, &batch, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
  assert_journal_is_empty (root);

  remove_tree (root);
}

static void
test_fact_batch_import_service_rejects_unauthorized_caller (void)
{
  assert_batch_denied_without_writes ("admin-cli", "account-1");
}

static void
test_fact_batch_import_service_rejects_account_mismatch (void)
{
  assert_batch_denied_without_writes ("trusted-tool", "account-2");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/fact-batch-import-service/handles-valid-batch",
      test_fact_batch_import_service_handles_valid_batch);
  g_test_add_func ("/daemon-api/fact-batch-import-service/"
      "rejects-invalid-item-without-writes",
      test_fact_batch_import_service_rejects_invalid_item_without_writes);
  g_test_add_func ("/daemon-api/fact-batch-import-service/"
      "rejects-late-invalid-item-without-writes",
      test_fact_batch_import_service_rejects_late_invalid_item_without_writes);
  g_test_add_func ("/daemon-api/fact-batch-import-service/"
      "rejects-mixed-scope-without-writes",
      test_fact_batch_import_service_rejects_mixed_scope_without_writes);
  g_test_add_func ("/daemon-api/fact-batch-import-service/"
      "rejects-unauthorized-caller",
      test_fact_batch_import_service_rejects_unauthorized_caller);
  g_test_add_func ("/daemon-api/fact-batch-import-service/"
      "rejects-account-mismatch",
      test_fact_batch_import_service_rejects_account_mismatch);

  return g_test_run ();
}
