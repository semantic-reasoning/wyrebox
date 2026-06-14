#include "wyrebox-daemon-capnp-codec.h"
#include "wyrebox-daemon-error-frame.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-success-receipt.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

typedef enum
{
  FAKE_SERVER_SUCCESS,
  FAKE_SERVER_DAEMON_ERROR,
} FakeServerResponse;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerResponse response;
  WyreboxDaemonErrorClass error_class;
  const FakeServerResponse *responses;
  const WyreboxDaemonErrorClass *error_classes;
  const char *const *expected_recipients;
  guint expected_request_count;
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
make_temp_root (void)
{
  g_autofree char *root =
      g_dir_make_tmp ("wyrebox-postfix-lmtp-listener-XXXXXX", NULL);

  g_assert_nonnull (root);
  return g_steal_pointer (&root);
}

static const char *
listener_executable_path (void)
{
  const char *path = g_getenv ("WYREBOX_POSTFIX_LMTP_LISTENER_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");

  return path;
}

static char *
read_lmtp_line (GInputStream *input)
{
  g_autoptr (GByteArray) line = g_byte_array_new ();
  gboolean pending_cr = FALSE;

  for (;;) {
    guint8 byte = 0;
    gssize bytes_read = 0;
    g_autoptr (GError) error = NULL;

    bytes_read = g_input_stream_read (input, &byte, sizeof byte, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpint (bytes_read, >, 0);

    if (pending_cr) {
      g_assert_cmpuint (byte, ==, '\n');
      return g_strndup ((const char *) line->data, line->len);
    }

    if (byte == '\r') {
      pending_cr = TRUE;
      continue;
    }

    g_byte_array_append (line, &byte, 1);
  }
}

static void
write_all (GOutputStream *output, const char *text)
{
  gsize bytes_written = 0;
  g_autoptr (GError) error = NULL;

  g_assert_true (g_output_stream_write_all (output,
          text, strlen (text), &bytes_written, NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, strlen (text));
}

static void
assert_reply_prefix (GInputStream *input, const char *prefix)
{
  g_autofree char *line = read_lmtp_line (input);

  g_assert_true (g_str_has_prefix (line, prefix));
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
    g_strdup ("journal:8:2"),
    8,
    2,
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

static gpointer
fake_server_thread_main (gpointer user_data)
{
  FakeServer *server = (FakeServer *) user_data;
  g_assert_nonnull (server->expected_recipients);
  g_assert_cmpuint (server->expected_request_count, >, 0);

  for (guint request_index = 0; request_index < server->expected_request_count;
      request_index++) {
    g_autoptr (GError) error = NULL;
    g_autoptr (GSocketConnection) connection = NULL;
    g_autoptr (GBytes) request = NULL;
    g_autoptr (GBytes) response = NULL;
    g_autofree char *expected_delivery_id = NULL;
    g_autofree char *expected_request_id = NULL;
    GInputStream *input = NULL;
    GOutputStream *output = NULL;
    WyreboxDaemonDecodedRequestFrame decoded = { 0 };
    gpointer decoded_state = NULL;
    GDestroyNotify decoded_state_clear = NULL;
    FakeServerResponse response_kind = server->response;
    WyreboxDaemonErrorClass error_class = server->error_class;

    connection = g_socket_listener_accept (server->listener, NULL, NULL,
        &error);
    g_assert_no_error (error);
    g_assert_nonnull (connection);

    input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
    request = wyrebox_daemon_frame_io_read_payload (input, &error);
    g_assert_no_error (error);
    g_assert_nonnull (request);

    g_assert_true (wyrebox_daemon_capnp_codec_decode_request_frame (NULL,
            request,
            &decoded, &decoded_state, &decoded_state_clear, NULL, &error));
    g_assert_no_error (error);

    expected_delivery_id = g_strdup_printf ("stable-delivery-1/rcpt/%u",
        request_index + 1);
    expected_request_id = g_strdup_printf ("postfix-lmtp:%s",
        expected_delivery_id);

    g_assert_cmpint (decoded.operation, ==,
        WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION);
    g_assert_cmpstr (decoded.request_id, ==, expected_request_id);
    g_assert_cmpstr (decoded.correlation_id, ==, expected_request_id);
    g_assert_cmpstr (decoded.caller_identity, ==, "postfix");
    g_assert_cmpstr (decoded.account_identity, ==, "account-1");
    g_assert_cmpstr (decoded.tool_identity, ==,
        "wyrebox-postfix-lmtp-listener");
    g_assert_cmpstr (decoded.delivery_ingestion->delivery_id, ==,
        expected_delivery_id);
    g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==,
        "sender@example.com");
    g_assert_cmpstr (decoded.delivery_ingestion->recipients[0], ==,
        server->expected_recipients[request_index]);
    g_assert_null (decoded.delivery_ingestion->recipients[1]);
    assert_message_equal (decoded.delivery_ingestion->message_bytes,
        server->expected_message, server->expected_message_size);

    if (server->responses != NULL)
      response_kind = server->responses[request_index];
    if (server->error_classes != NULL)
      error_class = server->error_classes[request_index];

    switch (response_kind) {
      case FAKE_SERVER_SUCCESS:
        response = build_success_response (&decoded);
        break;
      case FAKE_SERVER_DAEMON_ERROR:
        response = build_daemon_error_response (&decoded, error_class);
        break;
    }
    {
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

    g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL,
            &error));
    g_assert_no_error (error);
  }

  g_assert_null (server->expected_recipients[server->expected_request_count]);

  return NULL;
}

static guint
count_expected_recipients (const char *const *expected_recipients)
{
  guint count = 0;

  g_assert_nonnull (expected_recipients);
  for (; expected_recipients[count] != NULL; count++);
  g_assert_cmpuint (count, >, 0);

  return count;
}

static void
fake_server_start_sequence (FakeServer *server,
    const char *socket_path, FakeServerResponse response,
    WyreboxDaemonErrorClass error_class,
    const FakeServerResponse *responses,
    const WyreboxDaemonErrorClass *error_classes,
    const char *const *expected_recipients,
    const guint8 *expected_message, gsize expected_message_size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->response = response;
  server->error_class = error_class;
  server->responses = responses;
  server->error_classes = error_classes;
  server->expected_recipients = expected_recipients;
  server->expected_request_count =
      count_expected_recipients (expected_recipients);
  server->expected_message = expected_message;
  server->expected_message_size = expected_message_size;

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener,
          address,
          G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error));
  g_assert_no_error (error);

  server->thread =
      g_thread_new ("fake-postfix-lmtp-listener-daemon",
      fake_server_thread_main, server);
}

static void
fake_server_start (FakeServer *server,
    const char *socket_path, FakeServerResponse response,
    WyreboxDaemonErrorClass error_class,
    const char *const *expected_recipients,
    const guint8 *expected_message, gsize expected_message_size)
{
  fake_server_start_sequence (server,
      socket_path,
      response,
      error_class,
      NULL,
      NULL,
      expected_recipients,
      expected_message, expected_message_size);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL)
    g_thread_join (server->thread);

  g_clear_object (&server->listener);
}

static void
wait_for_socket (const char *socket_path)
{
  for (guint i = 0; i < 200; i++) {
    if (g_file_test (socket_path, G_FILE_TEST_EXISTS))
      return;

    g_usleep (10000);
  }

  g_error ("listener socket did not appear: %s", socket_path);
}

static GSubprocess *
start_listener (const char *listen_socket_path, const char *daemon_socket_path)
{
  const char *argv[] = {
    listener_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--listen-socket", listen_socket_path,
    "--daemon-socket", daemon_socket_path,
    NULL
  };
  g_autoptr (GError) error = NULL;
  GSubprocess *subprocess = NULL;

  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);
  wait_for_socket (listen_socket_path);
  return subprocess;
}

static GSubprocess *
start_listener_with_max_message_bytes (const char *listen_socket_path,
    const char *daemon_socket_path, const char *max_message_bytes)
{
  const char *argv[] = {
    listener_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "stable-delivery-1",
    "--listen-socket", listen_socket_path,
    "--daemon-socket", daemon_socket_path,
    "--max-message-bytes", max_message_bytes,
    NULL
  };
  g_autoptr (GError) error = NULL;
  GSubprocess *subprocess = NULL;

  subprocess = g_subprocess_newv (argv,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE), &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);
  wait_for_socket (listen_socket_path);
  return subprocess;
}

static GSocketConnection *
connect_to_listener (const char *socket_path)
{
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GError) error = NULL;
  GSocketConnection *connection = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client,
      G_SOCKET_CONNECTABLE (address), NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);
  return connection;
}

static void
finish_listener (GSubprocess *listener)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (g_subprocess_wait (listener, NULL, &error));
  g_assert_no_error (error);
  g_assert_true (g_subprocess_get_successful (listener));
}

static void
test_successful_single_recipient_transaction (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = { "alice@example.com", NULL };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_successful_transaction_closes_before_second_mail (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = { "alice@example.com", NULL };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "221 ");

  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_daemon_unavailable_returns_temporary_failure (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody-bytes-must-not-appear\r\n.\r\n");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

static void
test_daemon_permanent_failure_returns_permanent_reply (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = { "alice@example.com", NULL };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_DAEMON_ERROR,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "554 5.6.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_message_size_limit_rejects_oversized_data (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener_with_max_message_bytes (listen_socket_path,
      daemon_socket_path, "12");
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "12345678901\r\n.\r\n");
  assert_reply_prefix (input, "552 5.3.4");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

static void
test_message_size_limit_allows_exact_limit (void)
{
  const guint8 expected_message[] = "1234567890\r\n";
  const char *expected_recipients[] = { "alice@example.com", NULL };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener_with_max_message_bytes (listen_socket_path,
      daemon_socket_path, "12");
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "1234567890\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_successful_multi_recipient_transaction (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\n.body\r\n";
  const char *expected_recipients[] = {
    "alice@example.com",
    "bob@example.com",
    NULL
  };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\n..body\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_multi_recipient_mixed_daemon_replies_preserve_rcpt_order (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = {
    "alice@example.com",
    "bob@example.com",
    "carol@example.com",
    NULL
  };
  const FakeServerResponse responses[] = {
    FAKE_SERVER_SUCCESS,
    FAKE_SERVER_DAEMON_ERROR,
    FAKE_SERVER_DAEMON_ERROR
  };
  const WyreboxDaemonErrorClass error_classes[] = {
    WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
    WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
    WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE
  };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start_sequence (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      responses,
      error_classes,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<carol@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "554 5.6.0");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_invalid_later_recipient_is_rejected_before_data (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = { "alice@example.com", NULL };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_SUCCESS,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<>\r\n");
  assert_reply_prefix (input, "501 5.5.4 Bad recipient address");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "250 2.0.0 Delivery accepted");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_multi_recipient_daemon_permanent_failure_fans_out (void)
{
  const guint8 expected_message[] =
      "Subject: hi\r\n\r\nbody\r\n";
  const char *expected_recipients[] = {
    "alice@example.com",
    "bob@example.com",
    NULL
  };
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "wyrebox.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  FakeServer server = { 0 };
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  fake_server_start (&server,
      daemon_socket_path,
      FAKE_SERVER_DAEMON_ERROR,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
      expected_recipients,
      expected_message, sizeof (expected_message) - 1);
  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody\r\n.\r\n");
  assert_reply_prefix (input, "554 5.6.0");
  assert_reply_prefix (input, "554 5.6.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_multi_recipient_daemon_unavailable_fans_out (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "Subject: hi\r\n\r\nbody-bytes-must-not-appear\r\n.\r\n");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

static void
test_multi_recipient_message_size_limit_fans_out (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener_with_max_message_bytes (listen_socket_path,
      daemon_socket_path, "12");
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, "12345678901\r\n.\r\n");
  assert_reply_prefix (input, "552 5.3.4");
  assert_reply_prefix (input, "552 5.3.4");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

static void
test_multi_recipient_empty_data_local_failure_fans_out (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "354 ");
  write_all (output, ".\r\n");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "451 4.3.0");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

static void
test_rset_clears_multi_recipient_state (void)
{
  g_autofree char *root = make_temp_root ();
  g_autofree char *daemon_socket_path = g_build_filename (root,
      "missing-daemon.sock", NULL);
  g_autofree char *listen_socket_path = g_build_filename (root,
      "wyrebox-lmtp.sock", NULL);
  g_autoptr (GSubprocess) listener = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;

  listener = start_listener (listen_socket_path, daemon_socket_path);
  connection = connect_to_listener (listen_socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  assert_reply_prefix (input, "220 ");
  write_all (output, "LHLO localhost\r\n");
  assert_reply_prefix (input, "250-");
  assert_reply_prefix (input, "250 ");
  write_all (output, "MAIL FROM:<sender@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<alice@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RCPT TO:<bob@example.com>\r\n");
  assert_reply_prefix (input, "250 ");
  write_all (output, "RSET\r\n");
  assert_reply_prefix (input, "250 2.0.0 Reset state");
  write_all (output, "DATA\r\n");
  assert_reply_prefix (input, "503 5.5.1 Bad command sequence");
  write_all (output, "QUIT\r\n");
  assert_reply_prefix (input, "221 ");

  g_assert_true (g_io_stream_close (G_IO_STREAM (connection), NULL, NULL));
  finish_listener (listener);
  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/postfix/lmtp-listener/successful-single-recipient",
      test_successful_single_recipient_transaction);
  g_test_add_func ("/postfix/lmtp-listener/closes-before-second-mail",
      test_successful_transaction_closes_before_second_mail);
  g_test_add_func ("/postfix/lmtp-listener/daemon-unavailable",
      test_daemon_unavailable_returns_temporary_failure);
  g_test_add_func ("/postfix/lmtp-listener/daemon-permanent-failure",
      test_daemon_permanent_failure_returns_permanent_reply);
  g_test_add_func ("/postfix/lmtp-listener/successful-multi-recipient",
      test_successful_multi_recipient_transaction);
  g_test_add_func ("/postfix/lmtp-listener/multi-recipient-mixed-replies",
      test_multi_recipient_mixed_daemon_replies_preserve_rcpt_order);
  g_test_add_func ("/postfix/lmtp-listener/reject-invalid-later-recipient",
      test_invalid_later_recipient_is_rejected_before_data);
  g_test_add_func ("/postfix/lmtp-listener/multi-recipient-permanent-failure",
      test_multi_recipient_daemon_permanent_failure_fans_out);
  g_test_add_func ("/postfix/lmtp-listener/multi-recipient-daemon-unavailable",
      test_multi_recipient_daemon_unavailable_fans_out);
  g_test_add_func ("/postfix/lmtp-listener/multi-recipient-size-limit",
      test_multi_recipient_message_size_limit_fans_out);
  g_test_add_func ("/postfix/lmtp-listener/multi-recipient-empty-data",
      test_multi_recipient_empty_data_local_failure_fans_out);
  g_test_add_func ("/postfix/lmtp-listener/rset-clears-multi-recipient",
      test_rset_clears_multi_recipient_state);
  g_test_add_func ("/postfix/lmtp-listener/message-size-limit",
      test_message_size_limit_rejects_oversized_data);
  g_test_add_func ("/postfix/lmtp-listener/message-size-exact-limit",
      test_message_size_limit_allows_exact_limit);

  return g_test_run ();
}
