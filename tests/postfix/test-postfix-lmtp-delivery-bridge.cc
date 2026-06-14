#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-success-receipt.h"
#include "wyrebox-postfix-lmtp-delivery-bridge.h"
#include "wyrebox-postfix-lmtp-session.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

typedef enum
{
  FAKE_SERVER_SUCCESS,
  FAKE_SERVER_DAEMON_ERROR,
  FAKE_SERVER_CLOSE_AFTER_REQUEST,
  FAKE_SERVER_TRUNCATED_RESPONSE,
  FAKE_SERVER_MISMATCHED_SUCCESS,
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
      g_dir_make_tmp ("wyrebox-postfix-lmtp-bridge-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "wyrebox.sock", NULL);
}

static GInputStream *
stream_from_bytes (const guint8 *data, gsize size)
{
  g_autoptr (GBytes) bytes = g_bytes_new_static (data, size);

  return g_memory_input_stream_new_from_bytes (bytes);
}

static void
assert_bytes_equal (GBytes *actual, const guint8 *expected, gsize expected_size)
{
  const guint8 *actual_data = NULL;
  gsize actual_size = 0;

  g_assert_nonnull (actual);
  actual_data = (const guint8 *) g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpmem (actual_data, actual_size, expected, expected_size);
}

static void
build_lmtp_request (const char *socket_path,
    const guint8 *conversation,
    gsize conversation_size,
    WyreboxPostfixLmtpOptions *options,
    WyreboxDaemonRequestIdentity *identity,
    WyreboxDaemonDeliveryIngestionRequest *request)
{
  const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--socket", socket_path,
    NULL
  };
  int argc = 0;
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GError) error = NULL;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_bytes (conversation, conversation_size);
  g_assert_true (wyrebox_postfix_lmtp_session_build (argc,
          argv, input, G_MAXSIZE, options, identity, request, &error));
  g_assert_no_error (error);
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
build_mismatched_success_response (const WyreboxDaemonDecodedRequestFrame
    *decoded)
{
  WyreboxDaemonSuccessReceipt receipt = {
    g_strdup ("postfix-lmtp:different-delivery"),
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
  g_assert_true (decoded.delivery_ingestion->queue_id == NULL ||
      decoded.delivery_ingestion->queue_id[0] == '\0');
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==,
      "sender@example.com");
  g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
      "alice@example.com");
  g_assert_null (decoded.delivery_ingestion->recipients[1]);
  assert_bytes_equal (decoded.delivery_ingestion->message_bytes,
      server->expected_message, server->expected_message_size);

  switch (server->response) {
    case FAKE_SERVER_SUCCESS:
      response = build_success_response (&decoded);
      break;
    case FAKE_SERVER_DAEMON_ERROR:
      response = build_daemon_error_response (&decoded, server->error_class);
      break;
    case FAKE_SERVER_MISMATCHED_SUCCESS:
      response = build_mismatched_success_response (&decoded);
      break;
    case FAKE_SERVER_CLOSE_AFTER_REQUEST:
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
      g_thread_new ("fake-postfix-lmtp-bridge-server",
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
assert_reply_values (const WyreboxPostfixLmtpReply *reply,
    WyreboxPostfixLmtpReplyOutcome outcome,
    int reply_code, const char *enhanced_status)
{
  g_assert_cmpint (reply->outcome, ==, outcome);
  g_assert_cmpint (reply->reply_code, ==, reply_code);
  g_assert_cmpstr (reply->enhanced_status, ==, enhanced_status);
}

static void
assert_reply_omits (const WyreboxPostfixLmtpReply *reply, const char *needle)
{
  g_assert_null (strstr (reply->reply_text, needle));
  g_assert_null (strstr (reply->log_message, needle));
}

static void
assert_bridge_with_fake_server (FakeServerResponse response,
    WyreboxDaemonErrorClass error_class,
    WyreboxPostfixLmtpReplyOutcome expected_outcome,
    int expected_reply_code,
    const char *expected_enhanced_status,
    const char *expected_log_context)
{
  const guint8 conversation[] =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n"
      "From: sender@example.com\r\n"
      "\r\n"
      "body\0xff\r\n"
      ".\r\n";
  const guint8 expected_message[] =
      "From: sender@example.com\r\n\r\nbody\0xff\r\n";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = {};
  g_autoptr (GError) error = NULL;
  FakeServer server = { 0 };

  build_lmtp_request (socket_path,
      conversation, sizeof (conversation) - 1, &options, &identity, &request);
  fake_server_start (&server,
      socket_path, response, error_class, expected_message,
      sizeof (expected_message) - 1);

  g_assert_true (wyrebox_postfix_lmtp_delivery_bridge_deliver (&options,
          &identity, &request, &reply, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      expected_outcome, expected_reply_code, expected_enhanced_status);
  g_assert_nonnull (strstr (reply.log_message, expected_log_context));
  assert_reply_omits (&reply, "daemon-free-form");
  assert_reply_omits (&reply, "body");

  fake_server_join (&server);
  remove_tree (root);
}

static void
test_success_returns_250_and_sends_delivery_ingestion (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_DELIVERED,
      250, "2.0.0", "durable_marker=journal:128:9");
}

static void
test_daemon_permanent_failure_returns_554 (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_DAEMON_ERROR,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_PERMANENT_FAILURE,
      554, "5.6.0", "daemon_error_class=permanentFailure");
}

static void
test_daemon_conflict_returns_451 (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_DAEMON_ERROR,
      WYREBOX_DAEMON_ERROR_CONFLICT,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "daemon_error_class=conflict");
}

static void
test_close_after_request_returns_451 (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_CLOSE_AFTER_REQUEST,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "local_error_domain=");
}

static void
test_truncated_response_returns_451 (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_TRUNCATED_RESPONSE,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "local_error_domain=");
}

static void
test_mismatched_success_identity_returns_451 (void)
{
  assert_bridge_with_fake_server (FAKE_SERVER_MISMATCHED_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0", "local_error_domain=");
}

static void
test_missing_socket_returns_451 (void)
{
  const guint8 conversation[] =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<alice@example.com>\r\n"
      "DATA\r\n" "Subject: hi\r\n" "\r\n" "body\r\n" ".\r\n";
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_auto (WyreboxPostfixLmtpReply) reply = {};
  g_autoptr (GError) error = NULL;

  build_lmtp_request ("/tmp/wyrebox-lmtp-bridge-missing.sock",
      conversation, sizeof (conversation) - 1, &options, &identity, &request);
  (void) g_remove ("/tmp/wyrebox-lmtp-bridge-missing.sock");

  g_assert_true (wyrebox_postfix_lmtp_delivery_bridge_deliver (&options,
          &identity, &request, &reply, &error));
  g_assert_no_error (error);

  assert_reply_values (&reply,
      WYREBOX_POSTFIX_LMTP_REPLY_OUTCOME_TEMPORARY_FAILURE,
      451, "4.3.0");
  g_assert_nonnull (strstr (reply.log_message, "local_error_domain="));
  assert_reply_omits (&reply, "body");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/lmtp-delivery-bridge/success",
      test_success_returns_250_and_sends_delivery_ingestion);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/permanent-failure",
      test_daemon_permanent_failure_returns_554);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/conflict",
      test_daemon_conflict_returns_451);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/close-after-request",
      test_close_after_request_returns_451);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/truncated-response",
      test_truncated_response_returns_451);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/mismatched-success",
      test_mismatched_success_identity_returns_451);
  g_test_add_func ("/postfix/lmtp-delivery-bridge/missing-socket",
      test_missing_socket_returns_451);

  return g_test_run ();
}
