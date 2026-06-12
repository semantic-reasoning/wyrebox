#include "wyrebox-daemon-message-search-dispatcher.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

typedef enum
{
  BAD_SEARCH_CHUNK_MISSING_QUERY_ID,
  BAD_SEARCH_CHUNK_REQUEST_ID,
  BAD_SEARCH_CHUNK_CORRELATION_ID,
  BAD_SEARCH_CHUNK_MESSAGE_ID_PRESENT,
} BadMessageSearchChunkMode;

static gboolean
search_messages_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-ids";
  g_autoptr (GBytes) bytes = NULL;
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-search");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpstr (request->criteria_token, ==, "unseen");

  if (was_called != NULL)
    *was_called = TRUE;

  bytes = g_bytes_new_static (payload, strlen (payload));
  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      "request-search",
      NULL, "query-1", identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
    search_message_chunk_without_correlation_fixture
    (const WyreboxDaemonRequestIdentity * identity,
    const WyreboxDaemonMessageSearchRequest * request,
    WyreboxDaemonStreamChunkFrame * out_chunk,
    gpointer user_data, GError ** error)
{
  const char *payload = "message-ids";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  gboolean *was_called = user_data;

  (void) request;

  if (was_called != NULL)
    *was_called = TRUE;

  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id, NULL, "query-1", NULL, 0, bytes, TRUE, error);
}

static gboolean
fail_search_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  (void) identity;
  (void) request;
  (void) out_chunk;

  *was_called = TRUE;
  return FALSE;
}

static gboolean
search_message_bad_chunk_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-ids";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  BadMessageSearchChunkMode mode = *(BadMessageSearchChunkMode *) user_data;

  (void) request;

  switch (mode) {
    case BAD_SEARCH_CHUNK_MISSING_QUERY_ID:
      out_chunk->request_id = g_strdup (identity->request_id);
      out_chunk->correlation_id = g_strdup (identity->correlation_id);
      out_chunk->chunk_index = 0;
      out_chunk->bytes = g_bytes_ref (bytes);
      out_chunk->end_of_stream = TRUE;
      return TRUE;
    case BAD_SEARCH_CHUNK_REQUEST_ID:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          "request-other",
          NULL, "query-1", identity->correlation_id, 0, bytes, TRUE, error);
    case BAD_SEARCH_CHUNK_CORRELATION_ID:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          identity->request_id,
          NULL, "query-1", "correlation-other", 0, bytes, TRUE, error);
    case BAD_SEARCH_CHUNK_MESSAGE_ID_PRESENT:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          identity->request_id,
          "message-1", NULL, identity->correlation_id, 0, bytes, TRUE, error);
  }

  g_assert_not_reached ();
}

static void
assert_bad_search_chunk_becomes_error_frame (BadMessageSearchChunkMode mode)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new
      (search_message_bad_chunk_fixture, &mode, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_message_search_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new (search_messages_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-search");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-search-1");
  g_assert_cmpstr (frame.stream_chunk.query_id, ==, "query-1");
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);
  g_assert_null (frame.stream_chunk.message_id);
}

static void
    test_message_search_dispatcher_rejects_unauthorized_caller_with_error_frame
    (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new (search_messages_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "skill",
          "account-1",
          "fact-importer", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_message_search_dispatcher_rejects_account_mismatch_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-2", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new (search_messages_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_message_search_dispatcher_rejects_missing_request_id_before_service (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new (search_messages_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_message_search_dispatch (service,
          "",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_message_search_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new
      (fail_search_without_error, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_message_search_dispatcher_rejects_invalid_chunk_request_id (void)
{
  assert_bad_search_chunk_becomes_error_frame (BAD_SEARCH_CHUNK_REQUEST_ID);
}

static void
test_message_search_dispatcher_rejects_invalid_chunk_correlation_id (void)
{
  assert_bad_search_chunk_becomes_error_frame (BAD_SEARCH_CHUNK_CORRELATION_ID);
}

static void
    test_message_search_dispatcher_normalizes_missing_chunk_correlation_id
    (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageSearchService) service = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_search_service_new
      (search_message_chunk_without_correlation_fixture, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_search_dispatch (service,
          "request-search",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-search-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-search");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-search-1");
  g_assert_cmpstr (frame.stream_chunk.request_id, ==, "request-search");
  g_assert_cmpstr (frame.stream_chunk.correlation_id, ==, "imap-search-1");
  g_assert_cmpstr (frame.stream_chunk.query_id, ==, "query-1");
  g_assert_null (frame.stream_chunk.message_id);
}

static void
test_message_search_dispatcher_rejects_invalid_chunk_missing_query_id (void)
{
  assert_bad_search_chunk_becomes_error_frame
      (BAD_SEARCH_CHUNK_MISSING_QUERY_ID);
}

static void
test_message_search_dispatcher_rejects_invalid_chunk_message_id (void)
{
  assert_bad_search_chunk_becomes_error_frame
      (BAD_SEARCH_CHUNK_MESSAGE_ID_PRESENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "handles-valid-envelope",
      test_message_search_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_message_search_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-account-mismatch-with-error-frame",
      test_message_search_dispatcher_rejects_account_mismatch_with_error_frame);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-missing-request-id-before-service",
      test_message_search_dispatcher_rejects_missing_request_id_before_service);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_message_search_dispatcher_converts_silent_failure_to_error_frame);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-invalid-chunk-request-id",
      test_message_search_dispatcher_rejects_invalid_chunk_request_id);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-invalid-chunk-correlation-id",
      test_message_search_dispatcher_rejects_invalid_chunk_correlation_id);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "normalizes-missing-chunk-correlation-id",
      test_message_search_dispatcher_normalizes_missing_chunk_correlation_id);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-invalid-chunk-missing-query-id",
      test_message_search_dispatcher_rejects_invalid_chunk_missing_query_id);
  g_test_add_func ("/daemon-api/message-search-dispatcher/"
      "rejects-invalid-chunk-message-id-present",
      test_message_search_dispatcher_rejects_invalid_chunk_message_id);

  return g_test_run ();
}
