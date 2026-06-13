#include "wyrebox-daemon-wirelog-predicate-query-dispatcher.h"

#include <gio/gio.h>
#include <string.h>

typedef struct
{
  const char *request_id;
  const char *query_id;
  const char *correlation_id;
  const char *message_id;
  gboolean fail_without_error;
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

  bytes = g_bytes_new_static ("rows", strlen ("rows"));
  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      fixture != NULL && fixture->request_id != NULL
      ? fixture->request_id : identity->request_id,
      fixture != NULL ? fixture->message_id : NULL,
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
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-chunk-request",
      test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_request);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-chunk-query",
      test_wirelog_predicate_query_dispatcher_rejects_bad_chunk_query);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-message-chunk",
      test_wirelog_predicate_query_dispatcher_rejects_message_chunk);
  g_test_add_func
      ("/daemon-api/wirelog-predicate-query/dispatcher/rejects-bad-correlation",
      test_wirelog_predicate_query_dispatcher_rejects_bad_correlation);

  return g_test_run ();
}
