#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-success-receipt.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>
#include <sysexits.h>

typedef enum
{
  FAKE_SERVER_SUCCESS,
  FAKE_SERVER_DAEMON_ERROR,
  FAKE_SERVER_TRUNCATED_RESPONSE,
} FakeServerResponse;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerResponse response;
  WyreboxDaemonErrorClass error_class;
  const guint8 *expected_message;
  gsize expected_message_size;
} FakeServer;

typedef struct
{
  int exit_status;
  GBytes *stdout_bytes;
  GBytes *stderr_bytes;
} LmtpRunResult;

static void
lmtp_run_result_clear (LmtpRunResult *result)
{
  if (result == NULL)
    return;

  result->exit_status = -1;
  g_clear_pointer (&result->stdout_bytes, g_bytes_unref);
  g_clear_pointer (&result->stderr_bytes, g_bytes_unref);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (LmtpRunResult, lmtp_run_result_clear)
/* *INDENT-ON* */

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

static char *
make_socket_path (char **out_root)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-postfix-lmtp-exec-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "wyrebox.sock", NULL);
}

static const char *
lmtp_executable_path (void)
{
  const char *path = g_getenv ("WYREBOX_POSTFIX_LMTP_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");

  return path;
}

static char *
bytes_to_string (GBytes *bytes)
{
  gsize size = 0;
  const char *data = (const char *) g_bytes_get_data (bytes, &size);

  return g_strndup (data, size);
}

static void
assert_stream_contains (GBytes *bytes, const char *needle)
{
  g_autofree char *text = bytes_to_string (bytes);

  g_assert_nonnull (strstr (text, needle));
}

static void
assert_stream_omits (GBytes *bytes, const char *needle)
{
  g_autofree char *text = bytes_to_string (bytes);

  g_assert_null (strstr (text, needle));
}

static void
assert_stdout_equals (GBytes *stdout_bytes, const char *expected)
{
  g_autofree char *stdout_text = bytes_to_string (stdout_bytes);

  g_assert_cmpstr (stdout_text, ==, expected);
}

static void
assert_message_equal (GBytes *actual, const guint8 *expected,
    gsize expected_size)
{
  const guint8 *actual_data = NULL;
  gsize actual_size = 0;

  g_assert_nonnull (actual);
  actual_data = (const guint8 *) g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected, expected_size);
}

static GBytes *
build_success_response (const WyreboxDaemonDecodedRequestFrame *decoded)
{
  WyreboxDaemonSuccessReceipt receipt = {
    g_strdup (decoded->request_id),
    g_strdup ("journal:128:9"),
    128,
    9,
    g_strdup ("daemon-free-form-summary-must-not-appear"),
  };
  g_auto (WyreboxDaemonSuccessReceipt) auto_receipt = receipt;
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_response_frame_init_success (&response,
          &auto_receipt, decoded->correlation_id, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  return g_steal_pointer (&encoded);
}

static GBytes *
build_daemon_error_response (const WyreboxDaemonDecodedRequestFrame *decoded,
    WyreboxDaemonErrorClass error_class)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          decoded->request_id,
          error_class,
          "daemon-free-form-message-must-not-appear",
          "daemon-free-form-retry-must-not-appear", &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, decoded->correlation_id, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  return g_steal_pointer (&encoded);
}

static void
write_truncated_response (GOutputStream *output)
{
  const guint8 partial_payload[] = "daemon-free-form-message-must-not-appear";
  guint8 frame_data[4 + sizeof (partial_payload)] = { 0 };
  gsize bytes_written = 0;
  g_autoptr (GError) error = NULL;

  frame_data[3] = sizeof (partial_payload) + 1;
  memcpy (frame_data + 4, partial_payload, sizeof (partial_payload));

  g_assert_true (g_output_stream_write_all (output,
          frame_data, sizeof (frame_data), &bytes_written, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, sizeof (frame_data));
}

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = (FakeServer *) user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GBytes) request = NULL;
  g_autoptr (GBytes) response = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  WyreboxDaemonDecodedRequestFrame decoded = { 0 };
  gpointer decoded_state = NULL;
  GDestroyNotify decoded_state_clear = NULL;

  connection = g_socket_listener_accept (server->listener, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  g_assert_nonnull (input);
  g_assert_nonnull (output);

  request = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  g_assert_nonnull (request);

  g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
          request,
          &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION);
  g_assert_cmpstr (decoded.request_id, ==,
      "postfix-lmtp:stable-delivery-1");
  g_assert_cmpstr (decoded.caller_identity, ==, "postfix");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "wyrebox-postfix-lmtp");
  g_assert_cmpstr (decoded.correlation_id, ==,
      "postfix-lmtp:stable-delivery-1");
  g_assert_nonnull (decoded.delivery_ingestion);
  g_assert_cmpstr (decoded.delivery_ingestion->delivery_id, ==,
      "stable-delivery-1");
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==,
      "sender@example.com");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
      "alice@example.com");
  g_assert_null (decoded.delivery_ingestion->recipients[1]);
  assert_message_equal (decoded.delivery_ingestion->message_bytes,
      server->expected_message, server->expected_message_size);

  switch (server->response) {
    case FAKE_SERVER_SUCCESS:
      response = build_success_response (&decoded);
      break;
    case FAKE_SERVER_DAEMON_ERROR:
      response = build_daemon_error_response (&decoded, server->error_class);
      break;
    case FAKE_SERVER_TRUNCATED_RESPONSE:
      write_truncated_response (output);
      break;
  }

  if (response != NULL) {
    const guint8 *response_data = NULL;
    gsize response_size = 0;

    response_data = (const guint8 *) g_bytes_get_data (response,
        &response_size);
    g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
            response_data, response_size, &error));
    g_assert_no_error (error);
  }

  if (decoded_state_clear != NULL)
    decoded_state_clear (decoded_state);

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, &error));
  g_assert_no_error (error);

  return NULL;
}

static void
fake_server_start (FakeServer *server,
    const char *socket_path,
    FakeServerResponse response,
    WyreboxDaemonErrorClass error_class,
    const guint8 *expected_message, gsize expected_message_size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->response = response;
  server->error_class = error_class;
  server->expected_message = expected_message;
  server->expected_message_size = expected_message_size;

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener,
          address,
          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error));
  g_assert_no_error (error);

  server->thread =
      g_thread_new ("fake-postfix-lmtp-exec-server",
      fake_server_thread_main, server);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL)
    g_thread_join (server->thread);

  g_clear_object (&server->listener);
}

static void
run_lmtp (const char *const *arguments,
    const guint8 *stdin_data, gsize stdin_size, LmtpRunResult *result)
{
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GBytes) stdin_bytes = NULL;
  g_autoptr (GBytes) stdout_bytes = NULL;
  g_autoptr (GBytes) stderr_bytes = NULL;
  g_autoptr (GError) error = NULL;

  lmtp_run_result_clear (result);
  stdin_bytes = g_bytes_new_static (stdin_data, stdin_size);

  subprocess = g_subprocess_newv (arguments,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDIN_PIPE |
          G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (g_subprocess_communicate (subprocess,
          stdin_bytes, NULL, &stdout_bytes, &stderr_bytes, &error));
  g_assert_no_error (error);

  result->exit_status = g_subprocess_get_exit_status (subprocess);
  result->stdout_bytes = g_steal_pointer (&stdout_bytes);
  result->stderr_bytes = g_steal_pointer (&stderr_bytes);
}

static void
test_success_writes_protocol_reply_and_sends_delivery (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "Subject: hi\r\n"
      "\r\n"
      "body\0xff\r\n"
      ".\r\n"
      "QUIT\r\n";
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\0xff\r\n";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--socket", socket_path,
    NULL
  };
  FakeServer server = { 0 };
  g_auto (LmtpRunResult) result = { 0 };

  fake_server_start (&server,
      socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_message, sizeof (expected_message) - 1);

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_OK);
  assert_stdout_equals (result.stdout_bytes,
      "250 2.0.0 Delivery accepted\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=delivered");
  assert_stream_contains (result.stderr_bytes, "durable_marker=journal:128:9");
  assert_stream_omits (result.stderr_bytes, "daemon-free-form-summary");
  assert_stream_omits (result.stderr_bytes, "body");
  assert_stream_omits (result.stderr_bytes, "250 2.0.0");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_daemon_permanent_failure_writes_permanent_reply (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "Subject: hi\r\n"
      "\r\n"
      "body-bytes-must-not-appear\r\n"
      ".\r\n";
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody-bytes-must-not-appear\r\n";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--socket", socket_path,
    NULL
  };
  FakeServer server = { 0 };
  g_auto (LmtpRunResult) result = { 0 };

  fake_server_start (&server,
      socket_path,
      FAKE_SERVER_DAEMON_ERROR,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
      expected_message, sizeof (expected_message) - 1);

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_DATAERR);
  assert_stdout_equals (result.stdout_bytes, "554 5.6.0 Message rejected\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=permanent_failure");
  assert_stream_contains (result.stderr_bytes,
      "daemon_error_class=permanentFailure");
  assert_stream_omits (result.stderr_bytes, "daemon-free-form-message");
  assert_stream_omits (result.stderr_bytes, "daemon-free-form-retry");
  assert_stream_omits (result.stderr_bytes, "body-bytes-must-not-appear");
  assert_stream_omits (result.stderr_bytes, "554 5.6.0");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_truncated_daemon_response_writes_temporary_reply (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "Subject: hi\r\n"
      "\r\n"
      "body-bytes-must-not-appear\r\n"
      ".\r\n";
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody-bytes-must-not-appear\r\n";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--socket", socket_path,
    NULL
  };
  FakeServer server = { 0 };
  g_auto (LmtpRunResult) result = { 0 };

  fake_server_start (&server,
      socket_path,
      FAKE_SERVER_TRUNCATED_RESPONSE,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_message, sizeof (expected_message) - 1);

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stdout_equals (result.stdout_bytes,
      "451 4.3.0 Temporary local delivery failure\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=temporary_failure");
  assert_stream_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stream_omits (result.stderr_bytes, "daemon-free-form-message");
  assert_stream_omits (result.stderr_bytes, "body-bytes-must-not-appear");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_missing_socket_writes_temporary_reply (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "Subject: hi\r\n"
      "\r\n"
      "body-bytes-must-not-appear\r\n"
      ".\r\n";
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--socket", "/tmp/wyrebox-postfix-lmtp-missing.sock",
    NULL
  };
  g_auto (LmtpRunResult) result = { 0 };

  (void) g_remove ("/tmp/wyrebox-postfix-lmtp-missing.sock");

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stdout_equals (result.stdout_bytes,
      "451 4.3.0 Temporary local delivery failure\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=temporary_failure");
  assert_stream_contains (result.stderr_bytes,
      "request_id=postfix-lmtp:stable-delivery-1");
  assert_stream_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stream_omits (result.stderr_bytes, "body-bytes-must-not-appear");
}

static void
test_invalid_argv_writes_temporary_reply_without_parser_text (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "Subject: hi\r\n"
      "\r\n"
      "body-bytes-must-not-appear\r\n"
      ".\r\n";
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    NULL
  };
  g_auto (LmtpRunResult) result = { 0 };

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stdout_equals (result.stdout_bytes,
      "451 4.3.0 Temporary local delivery failure\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=temporary_failure");
  assert_stream_contains (result.stderr_bytes, "request_id=<none>");
  assert_stream_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stream_omits (result.stderr_bytes, "delivery-id is required");
  assert_stream_omits (result.stderr_bytes, "body-bytes-must-not-appear");
}

static void
test_malformed_transcript_writes_temporary_reply_without_parser_text (void)
{
  const guint8 conversation[] =
      "LHLO localhost\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "body-bytes-must-not-appear\r\n"
      ".\r\n";
  const char *argv[] = {
    lmtp_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    NULL
  };
  g_auto (LmtpRunResult) result = { 0 };

  run_lmtp (argv, conversation, sizeof (conversation) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stdout_equals (result.stdout_bytes,
      "451 4.3.0 Temporary local delivery failure\r\n");
  assert_stream_contains (result.stderr_bytes, "outcome=temporary_failure");
  assert_stream_contains (result.stderr_bytes, "request_id=<none>");
  assert_stream_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stream_omits (result.stderr_bytes, "LMTP RCPT command is out of order");
  assert_stream_omits (result.stderr_bytes, "body-bytes-must-not-appear");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/postfix/lmtp-executable/success-writes-protocol-reply-and-sends-delivery",
      test_success_writes_protocol_reply_and_sends_delivery);
  g_test_add_func
      ("/postfix/lmtp-executable/daemon-permanent-failure-writes-permanent-reply",
      test_daemon_permanent_failure_writes_permanent_reply);
  g_test_add_func
      ("/postfix/lmtp-executable/truncated-daemon-response-writes-temporary-reply",
      test_truncated_daemon_response_writes_temporary_reply);
  g_test_add_func
      ("/postfix/lmtp-executable/missing-socket-writes-temporary-reply",
      test_missing_socket_writes_temporary_reply);
  g_test_add_func
      ("/postfix/lmtp-executable/invalid-argv-writes-temporary-reply-without-parser-text",
      test_invalid_argv_writes_temporary_reply_without_parser_text);
  g_test_add_func
      ("/postfix/lmtp-executable/malformed-transcript-writes-temporary-reply-without-parser-text",
      test_malformed_transcript_writes_temporary_reply_without_parser_text);

  return g_test_run ();
}
