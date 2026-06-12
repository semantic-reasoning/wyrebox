#include "wyrebox-daemon-connection-session.h"
#include "wyrebox-daemon-frame-io.h"

#include <gio/gio.h>
#include <glib.h>

#include <string.h>
#include <sys/socket.h>

typedef struct
{
  WyreboxDaemonPeerCredentials expected_credentials;
  const guint8 **expected_requests;
  const gsize *expected_request_sizes;
  const guint8 **response_payloads;
  const gsize *response_sizes;
  guint expected_request_count;
  guint call_count;
  guint fail_at_call;
  const gchar *failure_message;
} ConnectionSessionCallbackState;

static GSocket *
socket_from_fd_for_test (int fd)
{
  g_autoptr (GError) error = NULL;
  GSocket *socket = NULL;

  socket = g_socket_new_from_fd (fd, &error);
  g_assert_no_error (error);
  g_assert_nonnull (socket);
  return g_steal_pointer (&socket);
}

static void
create_socket_connection_pair (GSocketConnection **server_connection,
    GSocketConnection **client_connection)
{
  int socket_fds[2] = { -1, -1 };
  g_autoptr (GSocket) server_socket = NULL;
  g_autoptr (GSocket) client_socket = NULL;

  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
          socket_fds), ==, 0);
  server_socket = socket_from_fd_for_test (socket_fds[0]);
  client_socket = socket_from_fd_for_test (socket_fds[1]);

  *server_connection =
      g_socket_connection_factory_create_connection (g_steal_pointer
      (&server_socket));
  *client_connection =
      g_socket_connection_factory_create_connection (g_steal_pointer
      (&client_socket));
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
assert_bytes_equal (GBytes *actual, const guint8 *expected, gsize expected_size)
{
  gsize actual_size = 0;
  const guint8 *actual_data = NULL;

  g_assert_nonnull (actual);
  actual_data = g_bytes_get_data (actual, &actual_size);
  g_assert_cmpuint (actual_size, ==, expected_size);
  g_assert_cmpint (memcmp (actual_data, expected, expected_size), ==, 0);
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

static GBytes *
connection_session_callback (const WyreboxDaemonPeerCredentials *credentials,
    GBytes *request, gpointer user_data, GError **error)
{
  ConnectionSessionCallbackState *state = user_data;
  gsize request_size = 0;
  gsize request_index = 0;
  const guint8 *request_data = NULL;

  (void) error;
  g_assert_nonnull (state);
  g_assert_nonnull (credentials);
  g_assert_nonnull (request);

  state->call_count++;
  request_index = state->call_count - 1;
  g_assert_cmpuint (state->call_count, <=, state->expected_request_count);
  request_data = g_bytes_get_data (request, &request_size);
  g_assert_cmpuint (request_size,
      ==, state->expected_request_sizes[request_index]);
  g_assert_cmpint (memcmp (request_data,
          state->expected_requests[request_index], request_size), ==, 0);
  g_assert_cmpuint (credentials->uid, ==, state->expected_credentials.uid);
  g_assert_cmpuint (credentials->gid, ==, state->expected_credentials.gid);
  g_assert_cmpint (credentials->pid, ==, state->expected_credentials.pid);

  if (state->fail_at_call > 0 && state->call_count == state->fail_at_call) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "%s", state->failure_message);
    return NULL;
  }

  return g_bytes_new (state->response_payloads[request_index],
      state->response_sizes[request_index]);
}

static GBytes *
connection_session_should_not_run_callback (const WyreboxDaemonPeerCredentials
    *credentials, GBytes *request, gpointer user_data, GError **error)
{
  (void) credentials;
  (void) request;
  (void) user_data;
  (void) error;

  g_assert_not_reached ();
  return NULL;
}

static void
test_connection_session_round_trip (void)
{
  const guint8 request[] = "request-payload";
  const guint8 response[] = "response-payload";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  WyreboxDaemonPeerCredentials expected_credentials = {
    .uid = 100,.gid = 200,.pid = 300,
  };
  const guint8 *requests[] = { request };
  const gsize request_sizes[] = { sizeof (request) };
  const guint8 *responses[] = { response };
  const gsize response_sizes[] = { sizeof (response) };
  ConnectionSessionCallbackState state = {
    .expected_credentials = expected_credentials,
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 1,
  };
  GInputStream *client_input = NULL;
  GOutputStream *client_output = NULL;
  g_autoptr (GBytes) response_bytes = NULL;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &expected_credentials, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, request, sizeof (request));
  shutdown_output (client_connection);

  g_assert_true (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (state.call_count, ==, 1);

  client_input = g_io_stream_get_input_stream (G_IO_STREAM (client_connection));
  response_bytes = wyrebox_daemon_frame_io_read_payload (client_input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (response_bytes, response, sizeof (response));

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_embedded_nuls (void)
{
  const guint8 request[] = { 0x00, 'h', 0x00, 'i', 'A', 0x00, 'B', 0xFF };
  const guint8 response[] = "response-with-nuls";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  WyreboxDaemonPeerCredentials expected_credentials = {
    .uid = 111,.gid = 222,.pid = 333,
  };
  const guint8 *requests[] = { request };
  const gsize request_sizes[] = { sizeof (request) };
  const guint8 *responses[] = { response };
  const gsize response_sizes[] = { sizeof (response) };
  ConnectionSessionCallbackState state = {
    .expected_credentials = expected_credentials,
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 1,
  };
  GInputStream *client_input = NULL;
  GOutputStream *client_output = NULL;
  g_autoptr (GBytes) response_bytes = NULL;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &expected_credentials, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, request, sizeof (request));
  shutdown_output (client_connection);

  g_assert_true (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (state.call_count, ==, 1);

  client_input = g_io_stream_get_input_stream (G_IO_STREAM (client_connection));
  response_bytes = wyrebox_daemon_frame_io_read_payload (client_input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (response_bytes, response, sizeof (response));

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_copies_peer_credentials (void)
{
  const guint8 request[] = "copy-check";
  const guint8 response[] = "ok";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  WyreboxDaemonPeerCredentials supplied_credentials = {
    .uid = 1010,.gid = 2020,.pid = 3030,
  };
  const guint8 *requests[] = { request };
  const gsize request_sizes[] = { sizeof (request) };
  const guint8 *responses[] = { response };
  const gsize response_sizes[] = { sizeof (response) };
  ConnectionSessionCallbackState state = {
    .expected_credentials = supplied_credentials,
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 1,
  };
  GOutputStream *client_output = NULL;
  GInputStream *client_input = NULL;
  g_autoptr (GBytes) response_bytes = NULL;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &supplied_credentials, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  supplied_credentials.uid = 10000;
  supplied_credentials.gid = 20000;
  supplied_credentials.pid = 30000;

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, request, sizeof (request));
  shutdown_output (client_connection);

  g_assert_true (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (state.call_count, ==, 1);

  client_input = g_io_stream_get_input_stream (G_IO_STREAM (client_connection));
  response_bytes = wyrebox_daemon_frame_io_read_payload (client_input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (response_bytes, response, sizeof (response));

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_two_frames (void)
{
  const guint8 first_request[] = "first";
  const guint8 second_request[] = "second";
  const guint8 first_response[] = "alpha";
  const guint8 second_response[] = "bravo";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  WyreboxDaemonPeerCredentials expected_credentials = {
    .uid = 444,.gid = 555,.pid = 666,
  };
  const guint8 *requests[] = { first_request, second_request };
  const gsize request_sizes[] = { sizeof (first_request),
    sizeof (second_request),
  };
  const guint8 *responses[] = { first_response, second_response };
  const gsize response_sizes[] = { sizeof (first_response),
    sizeof (second_response),
  };
  ConnectionSessionCallbackState state = {
    .expected_credentials = expected_credentials,
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 2,
  };
  GInputStream *client_input = NULL;
  GOutputStream *client_output = NULL;
  g_autoptr (GBytes) first_read = NULL;
  g_autoptr (GBytes) second_read = NULL;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &expected_credentials, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, first_request, sizeof (first_request));
  write_framed_payload (client_output, second_request, sizeof (second_request));
  shutdown_output (client_connection);

  g_assert_true (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (state.call_count, ==, 2);

  client_input = g_io_stream_get_input_stream (G_IO_STREAM (client_connection));
  first_read = wyrebox_daemon_frame_io_read_payload (client_input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (first_read, first_response, sizeof (first_response));
  second_read = wyrebox_daemon_frame_io_read_payload (client_input, &error);
  g_assert_no_error (error);
  assert_bytes_equal (second_read, second_response, sizeof (second_response));

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_eof_before_frame (void)
{
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  WyreboxDaemonPeerCredentials credentials = { 0 };
  ConnectionSessionCallbackState state = {
    .expected_request_count = 1,
  };

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection, &credentials,
      connection_session_should_not_run_callback, &state, NULL);
  g_assert_nonnull (session);

  shutdown_output (client_connection);

  g_assert_true (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (state.call_count, ==, 0);

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_truncated_prefix_fails (void)
{
  const guint8 truncated_prefix[3] = { 0x00, 0x00, 0x00 };
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  GOutputStream *client_output = NULL;
  WyreboxDaemonPeerCredentials peer_credentials = {
    .uid = 111,.gid = 222,.pid = 333,
  };
  ConnectionSessionCallbackState state = {
    .expected_request_count = 1,
  };
  gsize bytes_written = 0;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &peer_credentials, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  g_assert_true (g_output_stream_write_all (client_output, truncated_prefix,
          sizeof (truncated_prefix), &bytes_written, NULL, &error));
  g_assert_cmpuint (bytes_written, ==, sizeof (truncated_prefix));
  g_assert_no_error (error);
  shutdown_output (client_connection);

  g_assert_false (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (state.call_count, ==, 0);

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_callback_failure_writes_no_response (void)
{
  const guint8 request[] = "request";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  GOutputStream *client_output = NULL;
  GSocket *client_socket = NULL;
  const guint8 *requests[] = { request };
  const gsize request_sizes[] = { sizeof (request) };
  const guint8 *responses[] = { (const guint8 *) "response" };
  const gsize response_sizes[] = { 8 };
  ConnectionSessionCallbackState state = {
    .expected_credentials = {
          .uid = 1,.gid = 2,.pid = 3,
        },
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 1,
    .fail_at_call = 1,
    .failure_message = "callback rejection",
  };
  g_autoptr (GError) read_error = NULL;
  char sink_buffer[16] = { 0 };
  gssize read_bytes = 0;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &(WyreboxDaemonPeerCredentials) {
      .uid = 1,.gid = 2,.pid = 3}, connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_socket = g_socket_connection_get_socket (client_connection);
  g_assert_nonnull (client_socket);
  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, request, sizeof (request));
  g_assert_false (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint (state.call_count, ==, 1);

  g_socket_set_blocking (client_socket, FALSE);
  read_bytes =
      g_socket_receive (client_socket, sink_buffer, sizeof (sink_buffer), NULL,
      &read_error);
  g_assert_cmpint (read_bytes, ==, -1);
  g_assert_error (read_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

static void
test_connection_session_response_write_failure (void)
{
  const guint8 request[] = "request";
  g_autoptr (GError) error = NULL;
  GSocketConnection *server_connection = NULL;
  GSocketConnection *client_connection = NULL;
  g_autoptr (WyreboxDaemonConnectionSession) session = NULL;
  GOutputStream *client_output = NULL;
  const guint8 *requests[] = { request };
  const gsize request_sizes[] = { sizeof (request) };
  const guint8 *responses[] = { (const guint8 *) "response" };
  const gsize response_sizes[] = { 8 };
  ConnectionSessionCallbackState state = {
    .expected_credentials = {
          .uid = 10,.gid = 20,.pid = 30,
        },
    .expected_requests = requests,
    .expected_request_sizes = request_sizes,
    .response_payloads = responses,
    .response_sizes = response_sizes,
    .expected_request_count = 1,
  };
  GInputStream *client_input = NULL;

  create_socket_connection_pair (&server_connection, &client_connection);
  session =
      wyrebox_daemon_connection_session_new (server_connection,
      &(WyreboxDaemonPeerCredentials) {
      .uid = 10,.gid = 20,.pid = 30},
      connection_session_callback, &state, NULL);
  g_assert_nonnull (session);

  client_output =
      g_io_stream_get_output_stream (G_IO_STREAM (client_connection));
  write_framed_payload (client_output, request, sizeof (request));
  shutdown_output (client_connection);
  shutdown_output (server_connection);

  g_assert_false (wyrebox_daemon_connection_session_process_payloads (session,
          &error));
  g_assert_nonnull (error);

  client_input = g_io_stream_get_input_stream (G_IO_STREAM (client_connection));
  g_assert_null (wyrebox_daemon_frame_io_read_payload (client_input, NULL));

  g_clear_object (&server_connection);
  g_clear_object (&client_connection);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/connection-session/round-trip",
      test_connection_session_round_trip);
  g_test_add_func ("/daemon-api/connection-session/embedded-nuls",
      test_connection_session_embedded_nuls);
  g_test_add_func ("/daemon-api/connection-session/copies-peer-credentials",
      test_connection_session_copies_peer_credentials);
  g_test_add_func ("/daemon-api/connection-session/two-frames",
      test_connection_session_two_frames);
  g_test_add_func ("/daemon-api/connection-session/eof-before-frame",
      test_connection_session_eof_before_frame);
  g_test_add_func ("/daemon-api/connection-session/truncated-prefix",
      test_connection_session_truncated_prefix_fails);
  g_test_add_func ("/daemon-api/connection-session/callback-failure",
      test_connection_session_callback_failure_writes_no_response);
  g_test_add_func ("/daemon-api/connection-session/write-failure",
      test_connection_session_response_write_failure);

  return g_test_run ();
}
