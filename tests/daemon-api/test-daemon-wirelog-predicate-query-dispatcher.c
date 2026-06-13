#include "wyrebox-daemon-wirelog-predicate-query-dispatcher.h"
#include "wyrebox-daemon-audit-payload.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  const char *request_id;
  const char *query_id;
  const char *correlation_id;
  const char *message_id;
  const char *service_error_message;
  gboolean fail_without_error;
  GIOErrorEnum service_error_code;
} WirelogPredicateQueryFixture;

static gboolean
query_predicate_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, gpointer user_data,
    GError **error)
{
  WirelogPredicateQueryFixture *fixture = user_data;
  g_autoptr (GBytes) bytes = NULL;

  g_assert_true (g_strcmp0 (identity->caller_identity, "admin-cli") == 0
      || g_strcmp0 (identity->caller_identity, "trusted-tool") == 0);
  g_assert_cmpstr (request->query_id, ==, "query-1");
  g_assert_cmpstr (request->predicate_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (request->scope_id, ==, "account-1");
  g_assert_null (request->bindings[0]);

  if (fixture != NULL && fixture->fail_without_error)
    return FALSE;
  if (fixture != NULL && fixture->service_error_message != NULL) {
    g_set_error (error, G_IO_ERROR,
        fixture->service_error_code != 0 ? fixture->service_error_code :
        G_IO_ERROR_FAILED, "%s", fixture->service_error_message);
    return FALSE;
  }

  bytes = g_bytes_new_static ("rows", strlen ("rows"));
  if (fixture != NULL && fixture->message_id != NULL) {
    out_chunk->request_id = g_strdup (identity->request_id);
    out_chunk->message_id = g_strdup (fixture->message_id);
    out_chunk->query_id = g_strdup (request->query_id);
    out_chunk->correlation_id = g_strdup (identity->correlation_id);
    out_chunk->bytes = g_bytes_ref (bytes);
    out_chunk->end_of_stream = TRUE;
    return TRUE;
  }

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      fixture != NULL && fixture->request_id != NULL
      ? fixture->request_id : identity->request_id,
      NULL,
      fixture != NULL && fixture->query_id != NULL
      ? fixture->query_id : request->query_id,
      fixture != NULL ? fixture->correlation_id : identity->correlation_id,
      0, bytes, TRUE, error);
}

static void
init_request (WyreboxDaemonWirelogPredicateQueryRequest *request)
{
  const char *bindings[] = { NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (request,
          "query-1", "show_in_virtual_folder.v1", "account-1", bindings,
          &error));
  g_assert_no_error (error);
}

static void
init_custom_request (WyreboxDaemonWirelogPredicateQueryRequest *request,
    const char *predicate_id, const char *const *bindings)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_request_init (request,
          "query-1", predicate_id, "account-1", bindings, &error));
  g_assert_no_error (error);
}

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
assert_one_wirelog_failure_audit (const char *root,
    WyreboxDaemonAuditPayload *out_audit)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_auto (WyreboxJournalRecord) record = { 0 };
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (root, &error);
  g_assert_no_error (error);
  g_assert_true (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_false (eof);
  g_assert_cmpint (record.event_type, ==,
      WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED);
  g_assert_cmpuint (record.sequence, ==, 1);
  g_assert_true (wyrebox_daemon_audit_payload_decode (record.payload,
          out_audit, &error));
  g_assert_no_error (error);

  wyrebox_journal_record_clear (&record);
  g_assert_false (wyrebox_journal_reader_read_next (reader,
          &record, &eof, &error));
  g_assert_no_error (error);
  g_assert_true (eof);
}

static void
test_wirelog_predicate_query_dispatcher_handles_valid_envelope (void)
{
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-1");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (frame.stream_chunk.request_id, ==, "request-1");
  g_assert_cmpstr (frame.stream_chunk.query_id, ==, "query-1");
  g_assert_null (frame.stream_chunk.message_id);
  g_assert_true (frame.stream_chunk.end_of_stream);

  wyrebox_daemon_response_frame_clear (&frame);
  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-2", "trusted-tool", "account-1", "fact-tool",
          "correlation-2", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-2");
  g_assert_cmpstr (frame.correlation_id, ==, "correlation-2");
}

static void
test_wirelog_predicate_query_dispatcher_rejects_unauthorized_caller (void)
{
  const char *callers[] = { "postfix-helper", "dovecot-plugin", "unknown",
    NULL
  };
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  for (guint i = 0; callers[i] != NULL; i++) {
    wyrebox_daemon_response_frame_clear (&frame);
    g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
            "request-1", callers[i], "account-1", "fact-skill",
            "correlation-1", &request, &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
  }
}

static void
test_wirelog_predicate_query_dispatcher_audits_unauthorized_caller (void)
{
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *root =
      g_dir_make_tmp ("wyrebox-wirelog-query-audit-XXXXXX", NULL);

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);
  wyrebox_daemon_wirelog_predicate_query_service_set_audit_writer (service,
      writer);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "postfix-helper", "account-1", "fact-skill",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);

  assert_one_wirelog_failure_audit (root, &audit);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE);
  g_assert_cmpstr (audit.request_id, ==, "request-1");
  g_assert_cmpstr (audit.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (audit.caller_identity, ==, "postfix-helper");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.tool_identity, ==, "fact-skill");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpstr (audit.query_id, ==, "query-1");
  g_assert_cmpstr (audit.template_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (audit.error_class, ==, "permissionDenied");
  g_assert_cmpint (audit.error_code, ==, G_IO_ERROR_PERMISSION_DENIED);

  remove_tree (root);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_scope_mismatch (void)
{
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-2", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_empty_identity_scope (void)
{
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "trusted-tool", "", "fact-agent", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_unknown_predicate (void)
{
  const char *bindings[] = { NULL };
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_custom_request (&request, "project_mention", bindings);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_wrong_binding_count (void)
{
  const char *bindings[] = { "mail-1", NULL };
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_custom_request (&request, "show_in_virtual_folder.v1", bindings);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_wirelog_predicate_query_dispatcher_converts_silent_failure (void)
{
  WirelogPredicateQueryFixture fixture = {.fail_without_error = TRUE };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_wirelog_predicate_query_dispatcher_audits_execution_failure (void)
{
  WirelogPredicateQueryFixture fixture = {
    .service_error_message = "predicate service failed",
    .service_error_code = G_IO_ERROR_FAILED,
  };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *root =
      g_dir_make_tmp ("wyrebox-wirelog-query-audit-XXXXXX", NULL);

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);
  wyrebox_daemon_wirelog_predicate_query_service_set_audit_writer (service,
      writer);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
  g_assert_cmpstr (frame.error.message, ==, "predicate service failed");

  assert_one_wirelog_failure_audit (root, &audit);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE);
  g_assert_cmpstr (audit.request_id, ==, "request-1");
  g_assert_cmpstr (audit.correlation_id, ==, "correlation-1");
  g_assert_cmpstr (audit.caller_identity, ==, "admin-cli");
  g_assert_cmpstr (audit.account_identity, ==, "account-1");
  g_assert_cmpstr (audit.tool_identity, ==, "fact-tool");
  g_assert_cmpstr (audit.scope_id, ==, "account-1");
  g_assert_cmpstr (audit.query_id, ==, "query-1");
  g_assert_cmpstr (audit.template_id, ==, "show_in_virtual_folder.v1");
  g_assert_cmpstr (audit.error_class, ==, "internalError");
  g_assert_cmpstr (audit.error_message, ==, "predicate service failed");

  remove_tree (root);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_request (void)
{
  WirelogPredicateQueryFixture fixture = {.request_id = "other-request" };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_query (void)
{
  WirelogPredicateQueryFixture fixture = {.query_id = "other-query" };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
}

static void
test_wirelog_predicate_query_dispatcher_audits_bad_chunk (void)
{
  WirelogPredicateQueryFixture fixture = {.message_id = "message-1" };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_auto (WyreboxDaemonAuditPayload) audit = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *root =
      g_dir_make_tmp ("wyrebox-wirelog-query-audit-XXXXXX", NULL);

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);
  wyrebox_daemon_wirelog_predicate_query_service_set_audit_writer (service,
      writer);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpstr (frame.error.message, ==,
      "wirelog predicate query stream chunk must not contain message_id");
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);

  assert_one_wirelog_failure_audit (root, &audit);
  g_assert_cmpint (audit.operation, ==,
      WYREBOX_DAEMON_AUDIT_OPERATION_WIRELOG_PREDICATE_QUERY);
  g_assert_cmpint (audit.outcome, ==, WYREBOX_DAEMON_AUDIT_OUTCOME_FAILURE);
  g_assert_cmpstr (audit.error_message, ==,
      "wirelog predicate query stream chunk must not contain message_id");
  g_assert_cmpint (audit.error_code, ==, G_IO_ERROR_INVALID_ARGUMENT);

  remove_tree (root);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_message_chunk (void)
{
  WirelogPredicateQueryFixture fixture = {.message_id = "message-1" };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
}

static void
test_wirelog_predicate_query_dispatcher_success_writes_no_audit (void)
{
  WirelogPredicateQueryFixture fixture = { 0 };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree gchar *root =
      g_dir_make_tmp ("wyrebox-wirelog-query-audit-XXXXXX", NULL);

  g_assert_nonnull (root);
  writer = wyrebox_journal_writer_new (root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);
  wyrebox_daemon_wirelog_predicate_query_service_set_audit_writer (service,
      writer);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  assert_journal_is_empty (root);

  remove_tree (root);
}

static void
test_wirelog_predicate_query_dispatcher_rejects_bad_correlation (void)
{
  WirelogPredicateQueryFixture fixture = {.correlation_id = "other-corr" };
  g_auto (WyreboxDaemonWirelogPredicateQueryRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) service = NULL;
  g_autoptr (GError) error = NULL;

  init_request (&request);
  service = wyrebox_daemon_wirelog_predicate_query_service_new
      (query_predicate_fixture, &fixture, NULL);

  g_assert_true (wyrebox_daemon_wirelog_predicate_query_dispatch (service,
          "request-1", "admin-cli", "account-1", "fact-skill", "correlation-1",
          &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/handles-valid-envelope",
      test_wirelog_predicate_query_dispatcher_handles_valid_envelope);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-unauthorized-caller",
      test_wirelog_predicate_query_dispatcher_rejects_unauthorized_caller);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/audits-unauthorized-caller",
      test_wirelog_predicate_query_dispatcher_audits_unauthorized_caller);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-scope-mismatch",
      test_wirelog_predicate_query_dispatcher_rejects_scope_mismatch);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-empty-identity-scope",
      test_wirelog_predicate_query_dispatcher_rejects_empty_identity_scope);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-unknown-predicate",
      test_wirelog_predicate_query_dispatcher_rejects_unknown_predicate);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-wrong-binding-count",
      test_wirelog_predicate_query_dispatcher_rejects_wrong_binding_count);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/converts-silent-failure",
      test_wirelog_predicate_query_dispatcher_converts_silent_failure);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/audits-execution-failure",
      test_wirelog_predicate_query_dispatcher_audits_execution_failure);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-chunk-request",
      test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_request);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-chunk-query",
      test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_query);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/audits-bad-chunk",
      test_wirelog_predicate_query_dispatcher_audits_bad_chunk);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-message-chunk",
      test_wirelog_predicate_query_dispatcher_rejects_message_chunk);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/success-writes-no-audit",
      test_wirelog_predicate_query_dispatcher_success_writes_no_audit);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-correlation",
      test_wirelog_predicate_query_dispatcher_rejects_bad_correlation);

  return g_test_run ();
}
