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
} FakeServerResponse;

typedef struct
{
  GSocketListener *listener;
  GThread *thread;
  FakeServerResponse response;
  const guint8 *expected_message;
  gsize expected_message_size;
  const char *const *expected_recipients;
} FakeServer;

typedef struct
{
  int exit_status;
  GBytes *stderr_bytes;
} PipeRunResult;

static void
pipe_run_result_clear (PipeRunResult *result)
{
  if (result == NULL)
    return;

  result->exit_status = -1;
  g_clear_pointer (&result->stderr_bytes, g_bytes_unref);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (PipeRunResult, pipe_run_result_clear)
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
      g_dir_make_tmp ("wyrebox-postfix-pipe-exec-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "wyrebox.sock", NULL);
}

static const char *
pipe_executable_path (void)
{
  const char *path = g_getenv ("WYREBOX_POSTFIX_PIPE_EXECUTABLE");

  g_assert_nonnull (path);
  g_assert_cmpstr (path, !=, "");

  return path;
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
assert_recipients_equal (const gchar *const *actual,
    const char *const *expected)
{
  guint i = 0;

  g_assert_nonnull (actual);
  g_assert_nonnull (expected);

  for (i = 0; expected[i] != NULL; i++)
    g_assert_cmpstr (actual[i], ==, expected[i]);

  g_assert_null (actual[i]);
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
      NULL,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  return g_steal_pointer (&encoded);
}

static GBytes *
build_daemon_error_response (const WyreboxDaemonDecodedRequestFrame *decoded)
{
  g_auto (WyreboxDaemonErrorFrame) error_frame = { 0 };
  g_auto (WyreboxDaemonResponseFrame) response = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) encoded = NULL;

  g_assert_true (wyrebox_daemon_error_frame_init (&error_frame,
          decoded->request_id,
          WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE,
          "daemon-free-form-message-must-not-appear",
          "daemon-free-form-retry-must-not-appear",
          &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_response_frame_init_error (&response,
          &error_frame, decoded->correlation_id, &error));
  g_assert_no_error (error);

  encoded = wyrebox_daemon_capnp_codec_encode_response_frame (&response,
      NULL,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (encoded);

  return g_steal_pointer (&encoded);
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
          &decoded,
          &decoded_state,
          &decoded_state_clear,
          NULL,
          &error));
  g_assert_no_error (error);
  g_assert_cmpint (decoded.operation, ==,
      WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION);
  g_assert_cmpstr (decoded.request_id, ==, "postfix:queue-1:delivery-1");
  g_assert_cmpstr (decoded.caller_identity, ==, "postfix");
  g_assert_cmpstr (decoded.account_identity, ==, "account-1");
  g_assert_cmpstr (decoded.tool_identity, ==, "wyrebox-postfix-pipe");
  g_assert_cmpstr (decoded.correlation_id, ==, "postfix:queue-1:delivery-1");
  g_assert_nonnull (decoded.delivery_ingestion);
  g_assert_cmpstr (decoded.delivery_ingestion->delivery_id, ==,
      "delivery-1");
  g_assert_cmpstr (decoded.delivery_ingestion->queue_id, ==, "queue-1");
  g_assert_cmpstr (decoded.delivery_ingestion->envelope_sender, ==,
      "sender@example.com");
  assert_recipients_equal (
      (const gchar * const *) decoded.delivery_ingestion->recipients,
      server->expected_recipients);
  assert_bytes_equal (decoded.delivery_ingestion->message_bytes,
      server->expected_message, server->expected_message_size);

  switch (server->response) {
    case FAKE_SERVER_SUCCESS:
      response = build_success_response (&decoded);
      break;
    case FAKE_SERVER_DAEMON_ERROR:
      response = build_daemon_error_response (&decoded);
      break;
  }

  const guint8 *response_data = NULL;
  gsize response_size = 0;

  response_data = (const guint8 *) g_bytes_get_data (response, &response_size);
  g_assert_true (wyrebox_daemon_frame_io_write_payload (output,
          response_data, response_size, &error));
  g_assert_no_error (error);

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
    const guint8 *expected_message,
    gsize expected_message_size, const char *const *expected_recipients)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketAddress) address = NULL;

  server->listener = g_socket_listener_new ();
  server->response = response;
  server->expected_message = expected_message;
  server->expected_message_size = expected_message_size;
  server->expected_recipients = expected_recipients;

  address = g_unix_socket_address_new (socket_path);
  g_assert_true (g_socket_listener_add_address (server->listener,
          address,
          G_SOCKET_TYPE_STREAM,
          G_SOCKET_PROTOCOL_DEFAULT,
          NULL,
          NULL,
          &error));
  g_assert_no_error (error);

  server->thread =
      g_thread_new ("fake-postfix-pipe-server",
      fake_server_thread_main,
      server);
}

static void
fake_server_join (FakeServer *server)
{
  if (server->thread != NULL)
    g_thread_join (server->thread);

  g_clear_object (&server->listener);
}

static void
run_pipe (const char *const *arguments,
    const guint8 *stdin_data,
    gsize stdin_size, PipeRunResult *result)
{
  g_autoptr (GSubprocess) subprocess = NULL;
  g_autoptr (GBytes) stdin_bytes = NULL;
  g_autoptr (GBytes) stderr_bytes = NULL;
  g_autoptr (GError) error = NULL;

  pipe_run_result_clear (result);
  stdin_bytes = g_bytes_new_static (stdin_data, stdin_size);

  subprocess = g_subprocess_newv (arguments,
      (GSubprocessFlags) (G_SUBPROCESS_FLAGS_STDIN_PIPE |
          G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
          G_SUBPROCESS_FLAGS_STDERR_PIPE),
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (subprocess);

  g_assert_true (g_subprocess_communicate (subprocess,
          stdin_bytes, NULL, NULL, &stderr_bytes, &error));
  g_assert_no_error (error);

  result->exit_status = g_subprocess_get_exit_status (subprocess);
  result->stderr_bytes = g_steal_pointer (&stderr_bytes);
}

static char *
stderr_to_string (GBytes *stderr_bytes)
{
  gsize size = 0;
  const char *data = (const char *) g_bytes_get_data (stderr_bytes, &size);

  return g_strndup (data, size);
}

static void
assert_stderr_contains (GBytes *stderr_bytes, const char *needle)
{
  g_autofree char *stderr_text = stderr_to_string (stderr_bytes);

  g_assert_nonnull (strstr (stderr_text, needle));
}

static void
assert_stderr_omits (GBytes *stderr_bytes, const char *needle)
{
  g_autofree char *stderr_text = stderr_to_string (stderr_bytes);

  g_assert_null (strstr (stderr_text, needle));
}

static void
test_success_exits_ex_ok_and_sends_delivery_ingestion (void)
{
  const guint8 message[] = {
    'F', 'r', 'o', 'm', ':', ' ', 'a', '@', 'e', 'x', '\r', '\n',
    '\r', '\n',
    'b', 'o', 'd', 'y', '\0', 0xff, '\r', '\n',
  };
  const char *recipients[] = {
    "alice@example.com",
    "bob@example.com",
    NULL
  };
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  const char *argv[] = {
    pipe_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--sender", "sender@example.com",
    "--recipient", "alice@example.com",
    "--recipient", "bob@example.com",
    "--socket", socket_path,
    NULL
  };
  FakeServer server = { 0 };
  g_auto (PipeRunResult) result = { 0 };

  fake_server_start (&server,
      socket_path,
      FAKE_SERVER_SUCCESS,
      message,
      sizeof (message),
      recipients);

  run_pipe (argv, message, sizeof (message), &result);

  g_assert_cmpint (result.exit_status, ==, EX_OK);
  assert_stderr_contains (result.stderr_bytes, "outcome=delivered");
  assert_stderr_contains (result.stderr_bytes, "durable_marker=journal:128:9");
  assert_stderr_omits (result.stderr_bytes, "daemon-free-form-summary");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_daemon_error_maps_to_policy_exit (void)
{
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  const char *recipients[] = {
    "alice@example.com",
    "bob@example.com",
    NULL
  };
  g_autofree char *root = NULL;
  g_autofree char *socket_path = make_socket_path (&root);
  const char *argv[] = {
    pipe_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--sender", "sender@example.com",
    "--recipient", "alice@example.com",
    "--recipient", "bob@example.com",
    "--socket", socket_path,
    NULL
  };
  FakeServer server = { 0 };
  g_auto (PipeRunResult) result = { 0 };

  fake_server_start (&server,
      socket_path,
      FAKE_SERVER_DAEMON_ERROR,
      message,
      sizeof (message) - 1,
      recipients);

  run_pipe (argv, message, sizeof (message) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_DATAERR);
  assert_stderr_contains (result.stderr_bytes,
      "outcome=permanent_failure");
  assert_stderr_contains (result.stderr_bytes,
      "daemon_error_class=permanentFailure");
  assert_stderr_omits (result.stderr_bytes,
      "daemon-free-form-message");
  assert_stderr_omits (result.stderr_bytes,
      "daemon-free-form-retry");
  fake_server_join (&server);
  remove_tree (root);
}

static void
test_missing_socket_exits_tempfail (void)
{
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  const char *argv[] = {
    pipe_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    "--socket", "/tmp/wyrebox-postfix-pipe-missing.sock",
    NULL
  };
  g_auto (PipeRunResult) result = { 0 };

  (void) g_remove ("/tmp/wyrebox-postfix-pipe-missing.sock");

  run_pipe (argv, message, sizeof (message) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stderr_contains (result.stderr_bytes,
      "outcome=temporary_failure");
  assert_stderr_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stderr_contains (result.stderr_bytes,
      "request_id=postfix:queue-1:delivery-1");
}

static void
test_invalid_argv_exits_local_error_path (void)
{
  const guint8 message[] = "Subject: hi\r\n\r\nbody\r\n";
  const char *argv[] = {
    pipe_executable_path (),
    "--account-id", "account-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  g_auto (PipeRunResult) result = { 0 };

  run_pipe (argv, message, sizeof (message) - 1, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stderr_contains (result.stderr_bytes,
      "outcome=temporary_failure");
  assert_stderr_contains (result.stderr_bytes,
      "request_id=<none>");
  assert_stderr_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stderr_omits (result.stderr_bytes, "delivery-id is required");
}

static void
test_empty_stdin_exits_local_error_path (void)
{
  const guint8 message[] = "";
  const char *argv[] = {
    pipe_executable_path (),
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--recipient", "alice@example.com",
    NULL
  };
  g_auto (PipeRunResult) result = { 0 };

  run_pipe (argv, message, 0, &result);

  g_assert_cmpint (result.exit_status, ==, EX_TEMPFAIL);
  assert_stderr_contains (result.stderr_bytes,
      "outcome=temporary_failure");
  assert_stderr_contains (result.stderr_bytes,
      "request_id=<none>");
  assert_stderr_contains (result.stderr_bytes,
      "local_error_domain=g-io-error-quark");
  assert_stderr_omits (result.stderr_bytes, "message input must not be empty");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func (
      "/postfix/pipe-executable/success-exits-ex-ok-and-sends-request",
      test_success_exits_ex_ok_and_sends_delivery_ingestion);
  g_test_add_func (
      "/postfix/pipe-executable/daemon-error-maps-to-policy-exit",
      test_daemon_error_maps_to_policy_exit);
  g_test_add_func (
      "/postfix/pipe-executable/missing-socket-exits-tempfail",
      test_missing_socket_exits_tempfail);
  g_test_add_func (
      "/postfix/pipe-executable/invalid-argv-exits-local-error-path",
      test_invalid_argv_exits_local_error_path);
  g_test_add_func (
      "/postfix/pipe-executable/empty-stdin-exits-local-error-path",
      test_empty_stdin_exits_local_error_path);

  return g_test_run ();
}
