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

typedef enum
{
  BLOCKING_ROUTE_DECODED_WIRELOG_QUERY,
  BLOCKING_ROUTE_DECODED_DELIVERY,
} BlockingRouteDecodedKind;

typedef struct
{
  GMutex mutex;
  GCond query_release_cond;
  gboolean query_started;
  gboolean query_release;
  gboolean delivery_called;
} BlockingRouteState;

typedef struct
{
  BlockingRouteDecodedKind kind;
  BlockingRouteState *state;
  WyreboxDaemonWirelogPredicateQueryRequest wirelog_query;
  WyreboxDaemonDeliveryIngestionRequest delivery_ingestion;
} BlockingRouteDecodedState;

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

    while (g_main_context_pending (NULL))
      g_main_context_iteration (NULL, FALSE);

    g_usleep (1000);
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

static void
blocking_route_state_init (BlockingRouteState *state)
{
  g_mutex_init (&state->mutex);
  g_cond_init (&state->query_release_cond);
}

static void
blocking_route_state_clear (BlockingRouteState *state)
{
  g_cond_clear (&state->query_release_cond);
  g_mutex_clear (&state->mutex);
}

static void
blocking_route_decoded_state_clear (gpointer user_data)
{
  BlockingRouteDecodedState *state = user_data;

  if (state == NULL)
    return;

  switch (state->kind) {
    case BLOCKING_ROUTE_DECODED_WIRELOG_QUERY:
      wyrebox_daemon_wirelog_predicate_query_request_clear
          (&state->wirelog_query);
      break;
    case BLOCKING_ROUTE_DECODED_DELIVERY:
      wyrebox_daemon_delivery_ingestion_request_clear
          (&state->delivery_ingestion);
      break;
  }

  g_free (state);
}

static gboolean
blocking_route_decode (const WyreboxDaemonPeerCredentials *peer_credentials,
    GBytes *request, WyreboxDaemonDecodedRequestFrame *out_request,
    gpointer *out_decoded_state, GDestroyNotify *out_decoded_state_clear,
    gpointer user_data, GError **error)
{
  BlockingRouteState *state = user_data;
  BlockingRouteDecodedState *decoded_state = NULL;
  const guint8 *request_data = NULL;
  gsize request_size = 0;

  (void) peer_credentials;

  request_data = g_bytes_get_data (request, &request_size);
  decoded_state = g_new0 (BlockingRouteDecodedState, 1);
  decoded_state->state = state;

  out_request->request_id = NULL;
  out_request->caller_identity = NULL;
  out_request->account_identity = NULL;
  out_request->tool_identity = NULL;
  out_request->correlation_id = NULL;
  out_request->operation = WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_NONE;
  out_request->mailbox_list = NULL;
  out_request->mailbox_select = NULL;
  out_request->mailbox_status = NULL;
  out_request->fact_mutation = NULL;
  out_request->fact_batch_import = NULL;
  out_request->message_fetch = NULL;
  out_request->message_search = NULL;
  out_request->delivery_ingestion = NULL;
  out_request->flag_keyword_update = NULL;
  out_request->wirelog_predicate_query = NULL;
  out_request->duckdb_query_template = NULL;

  *out_decoded_state = decoded_state;
  *out_decoded_state_clear = blocking_route_decoded_state_clear;

  if (request_size == strlen ("wirelog-query")
      && memcmp (request_data, "wirelog-query", request_size) == 0) {
    const char *bindings[] = { NULL };

    decoded_state->kind = BLOCKING_ROUTE_DECODED_WIRELOG_QUERY;
    out_request->request_id = "request-wirelog-query";
    out_request->caller_identity = "trusted-tool";
    out_request->account_identity = "account-1";
    out_request->tool_identity = "fact-tool";
    out_request->correlation_id = "correlation-query";
    if (!wyrebox_daemon_wirelog_predicate_query_request_init
        (&decoded_state->wirelog_query,
            "query-1", "show_in_virtual_folder.v1", "account-1", bindings,
            error))
      return FALSE;
    out_request->operation =
        WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_WIRELOG_PREDICATE_QUERY;
    out_request->wirelog_predicate_query = &decoded_state->wirelog_query;
    return TRUE;
  }

  if (request_size == strlen ("delivery")
      && memcmp (request_data, "delivery", request_size) == 0) {
    const gchar *recipients[] = { "alice@example.com", NULL };
    g_autoptr (GBytes) message = NULL;

    decoded_state->kind = BLOCKING_ROUTE_DECODED_DELIVERY;
    out_request->request_id = "request-delivery";
    out_request->caller_identity = "postfix";
    out_request->account_identity = "account-1";
    out_request->tool_identity = "postfix";
    out_request->correlation_id = "correlation-delivery";
    message = g_bytes_new_static ("message-bytes", strlen ("message-bytes"));
    if (!wyrebox_daemon_delivery_ingestion_request_init
        (&decoded_state->delivery_ingestion,
            "delivery-1", "queue-1", NULL, recipients, message, error))
      return FALSE;
    out_request->operation =
        WYREBOX_DAEMON_REQUEST_FRAME_OPERATION_DELIVERY_INGESTION;
    out_request->delivery_ingestion = &decoded_state->delivery_ingestion;
    return TRUE;
  }

  g_set_error (error,
      G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "unexpected blocking route request");
  return FALSE;
}

static GBytes *
blocking_route_encode (const WyreboxDaemonResponseFrame *response_frame,
    gpointer user_data, GError **error)
{
  (void) user_data;
  (void) error;

  g_assert_nonnull (response_frame);

  switch (response_frame->kind) {
    case WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS:
      return g_bytes_new_static ("delivery-success",
          strlen ("delivery-success"));
    case WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK:
      return g_bytes_new_static ("query-success", strlen ("query-success"));
    default:
      g_assert_not_reached ();
  }
}

static gboolean
blocking_route_query_wirelog (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonWirelogPredicateQueryRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, gpointer user_data,
    GError **error)
{
  BlockingRouteState *state = user_data;
  g_autoptr (GBytes) bytes = NULL;
  gint64 deadline = 0;
  const char *payload = "rows";

  g_assert_cmpstr (identity->request_id, ==, "request-wirelog-query");
  g_assert_cmpstr (request->query_id, ==, "query-1");

  g_mutex_lock (&state->mutex);
  state->query_started = TRUE;
  deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!state->query_release) {
    if (!g_cond_wait_until (&state->query_release_cond, &state->mutex,
            deadline)) {
      g_mutex_unlock (&state->mutex);
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_TIMED_OUT, "timed out waiting to release query");
      return FALSE;
    }
  }
  g_mutex_unlock (&state->mutex);

  bytes = g_bytes_new_static (payload, strlen (payload));
  return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
      identity->request_id,
      NULL, request->query_id, identity->correlation_id, 0, bytes, TRUE, error);
}

static gboolean
blocking_route_ingest_delivery (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonDeliveryIngestionRequest *request,
    WyreboxEmlIngestResult *out_result, gpointer user_data, GError **error)
{
  BlockingRouteState *state = user_data;

  (void) error;

  g_assert_cmpstr (identity->request_id, ==, "request-delivery");
  g_assert_cmpstr (request->delivery_id, ==, "delivery-1");

  g_mutex_lock (&state->mutex);
  state->delivery_called = TRUE;
  g_mutex_unlock (&state->mutex);

  out_result->object_key =
      g_strdup
      ("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  out_result->size_bytes = g_bytes_get_size (request->message_bytes);
  out_result->journal_offset = 10;
  out_result->journal_sequence = 11;
  return TRUE;
}

static void
wait_until_query_started (BlockingRouteState *state)
{
  gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;

  while (g_get_monotonic_time () < deadline) {
    gboolean query_started = FALSE;

    while (g_main_context_pending (NULL))
      g_main_context_iteration (NULL, FALSE);

    g_mutex_lock (&state->mutex);
    query_started = state->query_started;
    g_mutex_unlock (&state->mutex);

    if (query_started)
      return;

    g_usleep (1000);
  }

  g_assert_not_reached ();
}

static void
release_blocking_query (BlockingRouteState *state)
{
  g_mutex_lock (&state->mutex);
  state->query_release = TRUE;
  g_cond_signal (&state->query_release_cond);
  g_mutex_unlock (&state->mutex);
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
  out_request->mailbox_select = NULL;
  out_request->mailbox_status = NULL;
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
  return wyrebox_daemon_request_adapter_new (NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, fake_adapter_decode, state, NULL,
      fake_adapter_encode, state, NULL);
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

static void
    test_connection_server_blocking_query_does_not_block_delivery_ingestion
    (void)
{
  const guint8 query_request[] = "wirelog-query";
  const guint8 delivery_request[] = "delivery";
  BlockingRouteState state = { 0 };
  g_autofree char *root = NULL;
  g_autofree char *socket_path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) delivery_response = NULL;
  g_autoptr (GBytes) query_response = NULL;
  g_autoptr (GSocketConnection) query_connection = NULL;
  g_autoptr (WyreboxDaemonDeliveryIngestionService) delivery_service = NULL;
  g_autoptr (WyreboxDaemonWirelogPredicateQueryService) wirelog_service = NULL;
  g_autoptr (WyreboxDaemonRequestAdapter) adapter = NULL;
  g_autoptr (WyreboxDaemonConnectionServer) server = NULL;
  GInputStream *query_input = NULL;
  GOutputStream *query_output = NULL;
  GSocket *query_socket = NULL;

  blocking_route_state_init (&state);

  root = NULL;
  socket_path = make_socket_path (&root);
  delivery_service = wyrebox_daemon_delivery_ingestion_service_new
      (blocking_route_ingest_delivery, &state, NULL);
  wirelog_service = wyrebox_daemon_wirelog_predicate_query_service_new
      (blocking_route_query_wirelog, &state, NULL);
  adapter = wyrebox_daemon_request_adapter_new (delivery_service, NULL, NULL,
      NULL, NULL, NULL, wirelog_service, NULL,
      blocking_route_decode, &state, NULL, blocking_route_encode, &state, NULL);
  server = wyrebox_daemon_connection_server_new (socket_path, adapter);
  g_assert_true (wyrebox_daemon_connection_server_start (server, &error));
  g_assert_no_error (error);

  query_connection = connect_with_socket_client (socket_path);
  query_input = g_io_stream_get_input_stream (G_IO_STREAM (query_connection));
  query_output = g_io_stream_get_output_stream (G_IO_STREAM (query_connection));
  query_socket = g_socket_connection_get_socket (query_connection);
  write_framed_payload (query_output, query_request, strlen ("wirelog-query"));
  wait_until_query_started (&state);

  delivery_response = send_request_and_read_response (socket_path,
      delivery_request, strlen ("delivery"));
  assert_payload_equal (delivery_response, (const guint8 *) "delivery-success",
      strlen ("delivery-success"));

  g_mutex_lock (&state.mutex);
  g_assert_true (state.delivery_called);
  g_mutex_unlock (&state.mutex);

  release_blocking_query (&state);
  await_framed_response_data (query_socket, 2000);
  query_response = wyrebox_daemon_frame_io_read_payload (query_input, &error);
  g_assert_no_error (error);
  assert_payload_equal (query_response, (const guint8 *) "query-success",
      strlen ("query-success"));

  g_assert_true (wyrebox_daemon_connection_server_stop (server, &error));
  g_assert_no_error (error);

  blocking_route_state_clear (&state);
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
  g_test_add_func
      ("/daemon-api/connection-server/blocking-query-does-not-block-delivery",
      test_connection_server_blocking_query_does_not_block_delivery_ingestion);

  return g_test_run ();
}
