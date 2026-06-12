#include "wyrebox-daemon-connection-server.h"
#include "wyrebox-daemon-frame-io.h"
#include "wyrebox-daemon-request-adapter.h"

#include <gio/gio.h>

#include <glib/gstdio.h>

#include <string.h>

typedef struct
{
  guint decode_calls;
  guint encode_calls;
  guint clear_calls;
  const guint8 *expected_request;
  gsize expected_request_size;
  gboolean expect_peer_credentials;
  WyreboxDaemonPeerCredentials expected_peer_credentials;
  const gchar *request_id;
  const guint8 *encoded_response;
  gsize encoded_response_size;
  gboolean fail_decode;
} FakeAdapterState;

typedef struct
{
  FakeAdapterState *state;
} FakeDecodedState;

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *entry = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_unlink (path);
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
      g_dir_make_tmp ("wyrebox-daemon-connection-server-XXXXXX", NULL);

  g_assert_nonnull (root);
  *out_root = g_strdup (root);

  return g_build_filename (root, "run", "wyrebox.sock", NULL);
}

static GSocketConnection *
connect_with_socket_client (const char *socket_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketClient) client = NULL;
  g_autoptr (GSocketAddress) address = NULL;
  g_autoptr (GSocketConnection) connection = NULL;

  client = g_socket_client_new ();
  address = g_unix_socket_address_new (socket_path);
  connection = g_socket_client_connect (client, G_SOCKET_CONNECTABLE (address),
      NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (connection);

  return g_steal_pointer (&connection);
}

static void
write_framed_payload (GOutputStream *output, const guint8 *payload,
    gsize payload_size)
{
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_frame_io_write_payload (output, payload,
          payload_size, &error));
  g_assert_no_error (error);
}

static void
shutdown_output (GSocketConnection *connection)
{
  g_autoptr (GError) error = NULL;
  GSocket *socket = NULL;

  socket = g_socket_connection_get_socket (connection);
  g_assert_nonnull (socket);
  g_assert_true (g_socket_shutdown (socket, FALSE, TRUE, &error));
  g_assert_no_error (error);
}

static void
await_framed_response_data (GSocket *socket, guint timeout_ms)
{
  gint64 deadline = 0;
  guint prefix_bytes = 0;

  deadline = g_get_monotonic_time ()
  + (gint64) timeout_ms *G_TIME_SPAN_MILLISECOND;
  prefix_bytes = sizeof (guint32);

  while (g_get_monotonic_time () < deadline) {
    if (g_socket_get_available_bytes (socket) >= prefix_bytes)
      return;
    g_main_context_iteration (NULL, TRUE);
  }

  g_assert_not_reached ();
}

static GBytes *
send_request_and_read_response (const char *socket_path,
    const guint8 *request, gsize request_size)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  g_autoptr (GBytes) response = NULL;
  GInputStream *input = NULL;
  GOutputStream *output = NULL;
  GSocket *socket = NULL;

  connection = connect_with_socket_client (socket_path);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));
  socket = g_socket_connection_get_socket (connection);
  g_assert_nonnull (socket);

  write_framed_payload (output, request, request_size);
  shutdown_output (connection);
  await_framed_response_data (socket, 2000);

  response = wyrebox_daemon_frame_io_read_payload (input, &error);
  g_assert_no_error (error);
  g_assert_nonnull (response);

  return g_steal_pointer (&response);
}

static void
assert_connection_closed_by_peer (GSocketConnection *connection)
{
  g_autoptr (GError) error = NULL;
  GInputStream *input = NULL;
  GSocket *socket = NULL;
  guint8 buffer = 0;
  gssize bytes_read = 0;

  socket = g_socket_connection_get_socket (connection);
  input = g_io_stream_get_input_stream (G_IO_STREAM (connection));
  g_assert_nonnull (socket);
  g_assert_nonnull (input);

  g_assert_true (g_socket_condition_timed_wait (socket,
          G_IO_IN | G_IO_HUP | G_IO_ERR, 2 * G_TIME_SPAN_SECOND, NULL, &error));
  g_assert_no_error (error);

  bytes_read = g_input_stream_read (input, &buffer, sizeof (buffer), NULL,
      &error);
  g_assert_no_error (error);
  g_assert_cmpint (bytes_read, ==, 0);
}

static void
send_malformed_request (const char *socket_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) connection = NULL;
  GOutputStream *output = NULL;
  const guint8 truncated_prefix[3] = { 0x00, 0x00, 0x00 };
  gsize bytes_written = 0;

  connection = connect_with_socket_client (socket_path);
  output = g_io_stream_get_output_stream (G_IO_STREAM (connection));

  g_assert_true (g_output_stream_write_all (output, truncated_prefix,
          sizeof (truncated_prefix), &bytes_written, NULL, &error));
  g_assert_cmpuint (bytes_written, ==, sizeof (truncated_prefix));
  g_assert_no_error (error);
  shutdown_output (connection);

  g_main_context_iteration (NULL, TRUE);
}

static void
assert_payload_equal (GBytes *actual, const guint8 *expected,
    gsize expected_size)
{
  gsize actual_size = 0;
  const guint8 *actual_data = NULL;

  g_assert_nonnull (actual);
  actual_data = g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpint (memcmp (actual_data, expected, expected_size), ==, 0);
}

static void
fake_adapter_decode_clear (gpointer user_data)
{
  FakeDecodedState *state = user_data;

  if (state == NULL)
    return;

  if (state->state != NULL)
    state->state->clear_calls++;

  g_free (state);
}

static gboolean
fake_adapter_decode (const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request, WyreboxDaemonDecodedRequestFrame *out_request,
    gpointer *out_decoded_state, GDestroyNotify *out_decoded_state_clear,
    gpointer user_data, GError **error)
{
  FakeAdapterState *state = user_data;
  const guint8 *request_data = NULL;
  gsize request_size = 0;

  state->decode_calls++;

  if (state->expect_peer_credentials) {
    g_assert_cmpuint (peer_credentials->uid, ==,
        state->expected_peer_credentials.uid);
    g_assert_cmpuint (peer_credentials->gid, ==,
        state->expected_peer_credentials.gid);
    g_assert_cmpint (peer_credentials->pid, ==,
        state->expected_peer_credentials.pid);
  }

  if (state->fail_decode)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "fake adapter forced decode failure");

  if (state->fail_decode || *error != NULL)
    return FALSE;

  request_data = g_bytes_get_data (request, &request_size);
  g_assert_cmpuint (request_size, ==, state->expected_request_size);
  g_assert_cmpint (memcmp (request_data,
          state->expected_request, request_size), ==, 0);

  out_request->request_id = state->request_id;
  out_request->caller_identity = "caller";
  out_request->account_identity = "account";
  out_request->tool_identity = "tool";
  out_request->correlation_id = "correlation";
  out_request->operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE;
  out_request->mailbox_list = NULL;
  out_request->fact_mutation = NULL;

  if (out_decoded_state != NULL && out_decoded_state_clear != NULL) {
    FakeDecodedState *decoded_state = g_new0 (FakeDecodedState, 1);

    decoded_state->state = state;
    *out_decoded_state = decoded_state;
    *out_decoded_state_clear = fake_adapter_decode_clear;
  }

  return TRUE;
}

static GBytes *
fake_adapter_encode (const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data, GError **error)
{
  FakeAdapterState *state = user_data;

  (void) error;

  state->encode_calls++;

  g_assert_nonnull (response_frame);
  g_assert_cmpstr (response_frame->request_id, ==, state->request_id);
  g_assert_cmpint (response_frame->kind,
      ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);

  return g_bytes_new (state->encoded_response, state->encoded_response_size);
}

static WyreboxDaemonRequestAdapter *
create_fake_adapter (FakeAdapterState *state)
{
  return wyrebox_daemon_request_adapter_new (NULL, NULL, NULL,
      NULL, NULL, fake_adapter_decode, state, NULL, fake_adapter_encode, state,
      NULL);
}

static void
test_connection_server_listener_lifecycle (void)
{
  const guint8 request[] = "request";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GSocketConnection) client = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = request,
    .expected_request_size = sizeof (request),
    .expect_peer_credentials = TRUE,
    .expected_peer_credentials = {.uid = 0},
    .request_id = "lifecycle",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);
  state.expected_peer_credentials.uid = getuid ();
  state.expected_peer_credentials.gid = getgid ();
  state.expected_peer_credentials.pid = getpid ();

  adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);

  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (socket_path, G_FILE_TEST_EXISTS));

  client = connect_with_socket_client (socket_path);
  g_assert_nonnull (client);
  g_io_stream_close (G_IO_STREAM (client), NULL, &error);
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);
  g_assert_false (g_file_test (socket_path, G_FILE_TEST_EXISTS));

  remove_tree (root);
}

static void
test_connection_server_round_trip (void)
{
  const guint8 request[] = "request";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) read_response = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = request,
    .expected_request_size = sizeof (request),
    .expect_peer_credentials = TRUE,
    .expected_peer_credentials = {.uid = 0},
    .request_id = "round-trip",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);
  state.expected_peer_credentials.uid = getuid ();
  state.expected_peer_credentials.gid = getgid ();
  state.expected_peer_credentials.pid = getpid ();

  adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  read_response = send_request_and_read_response (socket_path, request,
      sizeof (request));
  g_assert_no_error (error);
  assert_payload_equal (read_response, response, sizeof (response));
  g_assert_cmpuint (state.decode_calls, ==, 1);
  g_assert_cmpuint (state.encode_calls, ==, 1);
  g_assert_cmpuint (state.clear_calls, ==, 1);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  remove_tree (root);
}

static void
test_connection_server_two_clients_sequential (void)
{
  const guint8 first_request[] = "first";
  const guint8 second_request[] = "second";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) first_response = NULL;
  g_autoptr (GBytes) second_response = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = first_request,
    .expected_request_size = sizeof (first_request),
    .expect_peer_credentials = TRUE,
    .expected_peer_credentials = {.uid = 0},
    .request_id = "sequential",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);
  state.expected_peer_credentials.uid = getuid ();
  state.expected_peer_credentials.gid = getgid ();
  state.expected_peer_credentials.pid = getpid ();

  adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  first_response = send_request_and_read_response (socket_path, first_request,
      sizeof (first_request));
  assert_payload_equal (first_response, response, sizeof (response));

  state.expected_request = second_request;
  state.expected_request_size = sizeof (second_request);
  second_response = send_request_and_read_response (socket_path, second_request,
      sizeof (second_request));
  assert_payload_equal (second_response, response, sizeof (response));

  g_assert_cmpuint (state.decode_calls, ==, 2);
  g_assert_cmpuint (state.encode_calls, ==, 2);
  g_assert_cmpuint (state.clear_calls, ==, 2);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  remove_tree (root);
}

static void
    test_connection_server_malformed_payload_does_not_block_subsequent_clients
    (void)
{
  const guint8 request[] = "ok";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) read_response = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = request,
    .expected_request_size = sizeof (request),
    .expect_peer_credentials = TRUE,
    .expected_peer_credentials = {.uid = 0},
    .request_id = "malformed",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);
  state.expected_peer_credentials.uid = getuid ();
  state.expected_peer_credentials.gid = getgid ();
  state.expected_peer_credentials.pid = getpid ();

  adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  send_malformed_request (socket_path);

  read_response = send_request_and_read_response (socket_path, request,
      sizeof (request));
  assert_payload_equal (read_response, response, sizeof (response));

  g_assert_cmpuint (state.decode_calls, ==, 1);
  g_assert_cmpuint (state.encode_calls, ==, 1);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  remove_tree (root);
}

static void
test_connection_server_adapter_lifetime_protected (void)
{
  const guint8 request[] = "request";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) read_response = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = request,
    .expected_request_size = sizeof (request),
    .request_id = "lifetime",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);

  WyreboxDaemonRequestAdapter *adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_object_unref (adapter);

  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  read_response = send_request_and_read_response (socket_path, request,
      sizeof (request));
  assert_payload_equal (read_response, response, sizeof (response));

  g_assert_cmpuint (state.decode_calls, ==, 1);
  g_assert_cmpuint (state.encode_calls, ==, 1);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  remove_tree (root);
}

static void
    test_connection_server_idle_first_client_does_not_block_subsequent_client
    (void)
{
  const guint8 second_request[] = "subsequent";
  const guint8 response[] = "adapter-response";
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) second_response = NULL;
  g_autoptr (GSocketConnection) idle_connection = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  FakeAdapterState state = {
    .expected_request = second_request,
    .expected_request_size = sizeof (second_request),
    .expect_peer_credentials = TRUE,
    .expected_peer_credentials = {.uid = 0},
    .request_id = "idle-blocking",
    .encoded_response = response,
    .encoded_response_size = sizeof (response),
  };

  root = NULL;
  socket_path = make_socket_path (&root);
  state.expected_peer_credentials.uid = getuid ();
  state.expected_peer_credentials.gid = getgid ();
  state.expected_peer_credentials.pid = getpid ();

  adapter = create_fake_adapter (&state);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  idle_connection = connect_with_socket_client (socket_path);
  g_assert_nonnull (idle_connection);

  second_response = send_request_and_read_response (socket_path,
      second_request, sizeof (second_request));
  assert_payload_equal (second_response, response, sizeof (response));

  g_assert_cmpuint (state.decode_calls, ==, 1);
  g_assert_cmpuint (state.encode_calls, ==, 1);
  g_assert_cmpuint (state.clear_calls, ==, 1);

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);
  assert_connection_closed_by_peer (idle_connection);

  remove_tree (root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/connection-server/start-stop",
      test_connection_server_listener_lifecycle);
  g_test_add_func ("/daemon-api/connection-server/round-trip",
      test_connection_server_round_trip);
  g_test_add_func ("/daemon-api/connection-server/sequential-clients",
      test_connection_server_two_clients_sequential);
  g_test_add_func ("/daemon-api/connection-server/malformed-does-not-block",
      test_connection_server_malformed_payload_does_not_block_subsequent_clients);
  g_test_add_func ("/daemon-api/connection-server/adapter-lifetime",
      test_connection_server_adapter_lifetime_protected);
  g_test_add_func
      ("/daemon-api/connection-server/idle-client-does-not-block-subsequent",
      test_connection_server_idle_first_client_does_not_block_subsequent_client);

  return g_test_run ();
}
