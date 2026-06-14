#include "wyrebox-daemon-message-fetch-dispatcher.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

typedef enum
{
  BAD_FETCH_CHUNK_QUERY_ID,
  BAD_FETCH_CHUNK_REQUEST_ID,
  BAD_FETCH_CHUNK_CORRELATION_ID,
  BAD_FETCH_CHUNK_MISSING_MESSAGE_ID,
} BadFetchChunkMode;

static gboolean
fetch_message_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-bytes";
  g_autoptr (GBytes) bytes = NULL;
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-fetch");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpuint (request->mailbox_uid, ==, 42);

  if (was_called != NULL)
    *was_called = TRUE;

  bytes = g_bytes_new_static (payload, strlen (payload));
  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      "request-fetch",
      "message-1", NULL, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
fail_fetch_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
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
fetch_bad_chunk_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageFetchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk,
    gpointer user_data, GError **error)
{
  const char *payload = "message-bytes";
  g_autoptr (GBytes) bytes = g_bytes_new_static (payload, strlen (payload));
  BadFetchChunkMode mode = *(BadFetchChunkMode *) user_data;

  (void) request;

  switch (mode) {
    case BAD_FETCH_CHUNK_QUERY_ID:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          identity->request_id,
          NULL, "query-1", identity->correlation_id, 0, bytes, TRUE, error);
    case BAD_FETCH_CHUNK_REQUEST_ID:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          "request-other",
          "message-1", NULL, identity->correlation_id, 0, bytes, TRUE, error);
    case BAD_FETCH_CHUNK_CORRELATION_ID:
      return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
          identity->request_id,
          "message-1", NULL, "correlation-other", 0, bytes, TRUE, error);
    case BAD_FETCH_CHUNK_MISSING_MESSAGE_ID:
      out_chunk->request_id = g_strdup (identity->request_id);
      out_chunk->correlation_id = g_strdup (identity->correlation_id);
      out_chunk->chunk_index = 0;
      out_chunk->bytes = g_bytes_ref (bytes);
      out_chunk->end_of_stream = TRUE;
      return TRUE;
  }

  g_assert_not_reached ();
}

static void
assert_bad_fetch_chunk_becomes_error_frame (BadFetchChunkMode mode)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fetch_bad_chunk_fixture,
      &mode, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_message_fetch_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpstr (frame.request_id, ==, "request-fetch");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-fetch-1");
  g_assert_cmpstr (frame.stream_chunk.message_id, ==, "message-1");
  g_assert_cmpint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);
}

static void
    test_message_fetch_dispatcher_rejects_unauthorized_caller_with_error_frame
    (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "skill",
          "account-1",
          "fact-importer", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_message_fetch_dispatcher_rejects_account_mismatch_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-2", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_message_fetch_dispatcher_rejects_missing_request_id_before_service (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fetch_message_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_message_fetch_dispatch (service,
          "",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_message_fetch_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fail_fetch_without_error,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_message_fetch_dispatch (service,
          "request-fetch",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-fetch-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_message_fetch_service_sets_error_on_silent_failure (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMessageFetchService) service = NULL;
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_request_identity_init (&identity,
          "request-fetch",
          "dovecot", "account-1", "dovecot-storage", "imap-fetch-1", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox",
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 77, 42, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_message_fetch_service_new (fail_fetch_without_error,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_message_fetch_service_handle_identity (service,
          &identity, &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_message_fetch_dispatcher_rejects_query_chunk (void)
{
  assert_bad_fetch_chunk_becomes_error_frame (BAD_FETCH_CHUNK_QUERY_ID);
}

static void
test_message_fetch_dispatcher_rejects_request_mismatch (void)
{
  assert_bad_fetch_chunk_becomes_error_frame (BAD_FETCH_CHUNK_REQUEST_ID);
}

static void
test_message_fetch_dispatcher_rejects_correlation_mismatch (void)
{
  assert_bad_fetch_chunk_becomes_error_frame (BAD_FETCH_CHUNK_CORRELATION_ID);
}

static void
test_message_fetch_dispatcher_rejects_missing_message_id (void)
{
  assert_bad_fetch_chunk_becomes_error_frame
      (BAD_FETCH_CHUNK_MISSING_MESSAGE_ID);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "handles-valid-envelope",
      test_message_fetch_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_message_fetch_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-account-mismatch-with-error-frame",
      test_message_fetch_dispatcher_rejects_account_mismatch_with_error_frame);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-missing-request-id-before-service",
      test_message_fetch_dispatcher_rejects_missing_request_id_before_service);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_message_fetch_dispatcher_converts_silent_failure_to_error_frame);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "service-sets-error-on-silent-failure",
      test_message_fetch_service_sets_error_on_silent_failure);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-query-chunk", test_message_fetch_dispatcher_rejects_query_chunk);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-request-mismatch",
      test_message_fetch_dispatcher_rejects_request_mismatch);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-correlation-mismatch",
      test_message_fetch_dispatcher_rejects_correlation_mismatch);
  g_test_add_func ("/daemon-api/message-fetch-dispatcher/"
      "rejects-missing-message-id",
      test_message_fetch_dispatcher_rejects_missing_message_id);

  return g_test_run ();
}
